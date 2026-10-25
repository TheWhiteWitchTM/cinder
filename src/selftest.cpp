#include "selftest.h"

#include "common.h"
#include "editor.h"
#include "lexer.h"
#include "lint.h"
#include "debugger.h"
#include "runner.h"
#include "ui/mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include <cstdio>
#include <functional>

namespace {

int gFailed = 0;

void fail(const QString &message)
{
    const QByteArray bytes = message.toUtf8();
    std::fprintf(stderr, "FAIL %s\n", bytes.constData());
    ++gFailed;
}

void ok(const char *message)
{
    std::fprintf(stderr, "ok %s\n", message);
}

Tok kindAt(const QVector<Token> &tokens, int column)
{
    for (const Token &token : tokens) {
        if (column >= token.start && column < token.start + token.length)
            return token.kind;
    }
    return Tok::Plain;
}

QString dumpTokens(const QString &text, const QVector<Token> &tokens)
{
    QString out;
    for (const Token &token : tokens)
        out += QStringLiteral(" [%1,%2,%3]").arg(token.start).arg(token.length).arg(int(token.kind));
    return text + out;
}

void expectKind(const QString &text, int column, Tok kind, Dialect dialect, bool firstLine, const char *label)
{
    LexerState in;
    LexerState out;
    const QVector<Token> tokens = Lexer::tokenize(text, in, &out, firstLine, dialect);
    const Tok got = kindAt(tokens, column);
    if (got != kind)
        fail(QStringLiteral("%1: column %2 kind %3, wanted %4. %5")
                 .arg(QString::fromUtf8(label))
                 .arg(column)
                 .arg(int(got))
                 .arg(int(kind))
                 .arg(dumpTokens(text, tokens)));
}

bool waitLoop(const std::function<bool()> &done, int timeoutMs)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done())
            loop.quit();
    });
    poll.start(30);
    timer.start(timeoutMs);
    loop.exec();
    return done();
}

QString writeScript(QTemporaryDir &dir, const QString &name, const QString &body)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write(body.toUtf8());
    file.close();
    return QFileInfo(path).absoluteFilePath();
}

void testLexer()
{
    expectKind(QStringLiteral("#!/usr/bin/env bash"), 0, Tok::Shebang, Dialect::Bash, true, "shebang");
    expectKind(QStringLiteral("# comment"), 2, Tok::Comment, Dialect::Bash, false, "comment");
    expectKind(QStringLiteral("if true; then"), 0, Tok::Keyword, Dialect::Bash, false, "if");
    expectKind(QStringLiteral("if true; then"), 3, Tok::Builtin, Dialect::Bash, false, "true");
    expectKind(QStringLiteral("echo \"$HOME\""), 0, Tok::Builtin, Dialect::Bash, false, "echo");
    expectKind(QStringLiteral("echo \"$HOME\""), 7, Tok::Variable, Dialect::Bash, false, "HOME");
    expectKind(QStringLiteral("echo \"$HOME\""), 5, Tok::String, Dialect::Bash, false, "quote");
    expectKind(QStringLiteral("foo() {"), 0, Tok::Function, Dialect::Bash, false, "function");
    expectKind(QStringLiteral("name=1"), 0, Tok::Variable, Dialect::Bash, false, "assign");
    expectKind(QStringLiteral("name=1"), 5, Tok::Number, Dialect::Bash, false, "number");
    expectKind(QStringLiteral("[[ -n $x ]]"), 0, Tok::Keyword, Dialect::Bash, false, "bash [[");
    expectKind(QStringLiteral("[[ -n $x ]]"), 0, Tok::Bashism, Dialect::Sh, false, "sh [[");
    expectKind(QStringLiteral("source file"), 0, Tok::Bashism, Dialect::Sh, false, "sh source");

    LexerState in;
    LexerState mid;
    Lexer::tokenize(QStringLiteral("cat <<EOF"), in, &mid, false, Dialect::Bash);
    LexerState body;
    const QVector<Token> inside = Lexer::tokenize(QStringLiteral("hello $USER"), mid, &body, false, Dialect::Bash);
    bool sawHere = false;
    bool sawVar = false;
    for (const Token &token : inside) {
        if (token.kind == Tok::Heredoc)
            sawHere = true;
        if (token.kind == Tok::Variable)
            sawVar = true;
    }
    if (!sawHere || !sawVar)
        fail(QStringLiteral("heredoc body: ") + dumpTokens(QStringLiteral("hello $USER"), inside));
    LexerState after;
    const QVector<Token> end = Lexer::tokenize(QStringLiteral("EOF"), body, &after, false, Dialect::Bash);
    if (kindAt(end, 0) != Tok::Keyword || after.stack.size() != 0)
        fail(QStringLiteral("heredoc end: ") + dumpTokens(QStringLiteral("EOF"), end));
    if (gFailed == 0)
        ok("lexer");
    else
        ok("lexer finished with failures above");
}

void testArgs()
{
    bool okArgs = false;
    const QStringList args = splitArgs(QStringLiteral("a \"b c\" 'd e' f\\ g"), &okArgs);
    const QStringList want = {QStringLiteral("a"), QStringLiteral("b c"), QStringLiteral("d e"), QStringLiteral("f g")};
    if (!okArgs || args != want)
        fail(QStringLiteral("splitArgs -> %1").arg(args.join(QLatin1Char('|'))));
    bool bad = true;
    splitArgs(QStringLiteral("\"abc"), &bad);
    if (bad)
        fail(QStringLiteral("splitArgs accepted an unclosed quote"));
    if (parseShebang(QStringLiteral("#!/usr/bin/env bash -e\necho")).program != QLatin1String("bash"))
        fail(QStringLiteral("shebang parser"));
    if (detectDialect(QStringLiteral("a.sh"), QStringLiteral("#!/bin/sh\n")) != Dialect::Sh)
        fail(QStringLiteral("sh dialect"));
    ok("args");
}

void testLint()
{
    const QList<Problem> quote = builtinLint(QStringLiteral("a.sh"), QStringLiteral("echo \"hi\n"), Dialect::Bash);
    bool unclosed = false;
    for (const Problem &problem : quote) {
        if (problem.message.contains(QStringLiteral("Unclosed")))
            unclosed = true;
    }
    if (!unclosed)
        fail(QStringLiteral("lint missed an unclosed quote"));
    const QList<Problem> bash = builtinLint(QStringLiteral("a.sh"),
                                            QStringLiteral("#!/bin/sh\nif [[ -n $x ]]; then\n  echo yes\nfi\n"),
                                            Dialect::Sh);
    bool warned = false;
    for (const Problem &problem : bash) {
        if (problem.level == QLatin1String("warning"))
            warned = true;
    }
    if (!warned)
        fail(QStringLiteral("lint missed a bashism"));
    ok("lint");
}

void testEditor()
{
    CodeEdit edit;
    edit.setPlainText(QStringLiteral("if true; then"));
    QTextCursor cursor = edit.textCursor();
    cursor.movePosition(QTextCursor::End);
    edit.setTextCursor(cursor);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(&edit, &press);
    if (!edit.toPlainText().contains(QStringLiteral("\n    ")))
        fail(QStringLiteral("indent: '%1'").arg(edit.toPlainText()));
    edit.selectAll();
    edit.commentSelection();
    if (!edit.toPlainText().trimmed().startsWith(QLatin1Char('#')))
        fail(QStringLiteral("comment: '%1'").arg(edit.toPlainText()));
    edit.commentSelection();
    edit.toggleBreakpoint(2);
    if (!edit.breakpoints().contains(2))
        fail(QStringLiteral("breakpoint toggle"));
    edit.setDialect(Dialect::Sh);
    if (edit.dialect() != Dialect::Sh)
        fail(QStringLiteral("dialect"));
    ok("editor");
}

void testRun()
{
    QTemporaryDir dir;
    const QString path = writeScript(dir, QStringLiteral("read.sh"),
                                     QStringLiteral("#!/bin/bash\nread x\necho \"got=$x\"\n"));
    if (path.isEmpty()) {
        fail(QStringLiteral("could not write the run script"));
        return;
    }
    Runner runner;
    QString output;
    int code = -999;
    bool finished = false;
    QObject::connect(&runner, &Runner::output, &runner, [&](const QString &text, int channel) {
        if (channel == kStreamOut || channel == kStreamErr)
            output += text;
    });
    QObject::connect(&runner, &Runner::started, &runner, [&] { runner.writeInput(QStringLiteral("hi")); });
    QObject::connect(&runner, &Runner::finished, &runner, [&](int value) {
        code = value;
        finished = true;
    });
    RunSpec spec;
    spec.program = findProgram(QStringLiteral("bash"));
    spec.script = path;
    spec.workDir = dir.path();
    spec.mode = RunMode::Run;
    spec.dialect = Dialect::Bash;
    runner.start(spec);
    if (!waitLoop([&] { return finished; }, 8000) || code != 0 || !output.contains(QStringLiteral("got=hi")))
        fail(QStringLiteral("run code %1 output [%2]").arg(code).arg(output));
    else
        ok("run");
}

void testTrace()
{
    QTemporaryDir dir;
    const QString path = writeScript(dir, QStringLiteral("trace.sh"),
                                     QStringLiteral("#!/bin/bash\necho traced\n"));
    Runner runner;
    int line = 0;
    QString output;
    int code = -999;
    bool finished = false;
    QObject::connect(&runner, &Runner::traceLine, &runner, [&](const QString &, int number) { line = number; });
    QObject::connect(&runner, &Runner::output, &runner, [&](const QString &text, int) { output += text; });
    QObject::connect(&runner, &Runner::finished, &runner, [&](int value) {
        code = value;
        finished = true;
    });
    RunSpec spec;
    spec.program = findProgram(QStringLiteral("bash"));
    spec.prefix = {QStringLiteral("-x")};
    spec.script = path;
    spec.workDir = dir.path();
    spec.mode = RunMode::Trace;
    runner.start(spec);
    if (!waitLoop([&] { return finished; }, 8000) || code != 0 || line != 2
        || !output.contains(QStringLiteral("traced")))
        fail(QStringLiteral("trace code %1 line %2 output [%3]").arg(code).arg(line).arg(output));
    else
        ok("trace");
}

void testDebug()
{
    QTemporaryDir dir;
    const QString path = writeScript(dir, QStringLiteral("step.sh"),
                                     QStringLiteral("#!/bin/bash\necho one\necho two\ny=7\necho \"y=$y\"\n"));
    Debugger debugger;
    QList<int> lines;
    QList<Var> vars;
    QString output;
    int code = -999;
    bool finished = false;
    QObject::connect(&debugger, &Debugger::output, &debugger, [&](const QString &text, int channel) {
        if (channel == kStreamOut || channel == kStreamErr)
            output += text;
    });
    QObject::connect(&debugger, &Debugger::pausedAt, &debugger,
                     [&](const QString &, int line, const QList<Var> &got) {
                         lines.append(line);
                         vars = got;
                         if (lines.size() == 1)
                             debugger.stepInto();
                         else if (lines.size() == 2)
                             debugger.stepInto();
                         else
                             debugger.cont();
                     });
    QObject::connect(&debugger, &Debugger::finished, &debugger, [&](int value) {
        code = value;
        finished = true;
    });
    QObject::connect(&debugger, &Debugger::failed, &debugger, [&](const QString &message) {
        fail(QStringLiteral("debug failed: ") + message);
        finished = true;
    });
    DebugSpec spec;
    spec.script = path;
    spec.workDir = dir.path();
    spec.breakpoints.append({path, 3});
    spec.breakOnEntry = false;
    debugger.start(spec);
    if (!waitLoop([&] { return finished; }, 8000))
        fail(QStringLiteral("debug timed out, lines %1 output [%2]").arg(lines.size()).arg(output));
    else if (lines.size() < 3 || lines.at(0) != 3 || lines.at(1) != 4 || lines.at(2) != 5 || code != 0
             || !output.contains(QStringLiteral("one")) || !output.contains(QStringLiteral("y=7")))
        fail(QStringLiteral("debug lines %1 code %2 output [%3]")
                 .arg([&] {
                     QStringList parts;
                     for (int line : lines)
                         parts.append(QString::number(line));
                     return parts.join(QLatin1Char(','));
                 }())
                 .arg(code)
                 .arg(output));
    else {
        bool saw = false;
        for (const Var &var : vars) {
            if (var.name == QLatin1String("y") && var.value == QLatin1String("7"))
                saw = true;
        }
        if (!saw)
            fail(QStringLiteral("debug did not show y=7"));
        else
            ok("debug step");
    }

    const QString overPath = writeScript(dir, QStringLiteral("over.sh"),
                                         QStringLiteral("#!/bin/bash\nfoo() {\n  echo inside\n}\nfoo\necho after\n"));
    Debugger over;
    QList<int> overLines;
    QString overOut;
    int overCode = -999;
    bool overDone = false;
    QObject::connect(&over, &Debugger::output, &over, [&](const QString &text, int channel) {
        if (channel == kStreamOut)
            overOut += text;
    });
    QObject::connect(&over, &Debugger::pausedAt, &over, [&](const QString &, int line, const QList<Var> &) {
        overLines.append(line);
        if (overLines.size() == 1)
            over.stepOver();
        else
            over.cont();
    });
    QObject::connect(&over, &Debugger::finished, &over, [&](int value) {
        overCode = value;
        overDone = true;
    });
    DebugSpec overSpec;
    overSpec.script = overPath;
    overSpec.workDir = dir.path();
    overSpec.breakpoints.append({overPath, 5});
    over.start(overSpec);
    if (!waitLoop([&] { return overDone; }, 8000) || overLines.size() < 2 || overLines.at(0) != 5 || overLines.at(1) != 6
        || overCode != 0 || !overOut.contains(QStringLiteral("inside")) || !overOut.contains(QStringLiteral("after"))) {
        QStringList parts;
        for (int line : overLines)
            parts.append(QString::number(line));
        fail(QStringLiteral("step over lines %1 code %2 output [%3]").arg(parts.join(QLatin1Char(','))).arg(overCode).arg(overOut));
    } else {
        ok("debug step over");
    }
}

void testWindow()
{
    const QString org = QCoreApplication::organizationName();
    QCoreApplication::setOrganizationName(QStringLiteral("cinder-selftest"));
    QTemporaryDir dir;
    const QString path = writeScript(dir, QStringLiteral("view.sh"),
                                     QStringLiteral("#!/usr/bin/env bash\n# a comment\nif true; then\n  echo \"$HOME\"\nfi\n"));
    int before = gFailed;
    {
        MainWindow window;
        window.resize(1200, 760);
        window.openPath(path);
        window.show();
        QApplication::processEvents();
        QApplication::processEvents();
        const QImage image = window.grab().toImage();
        image.save(QStringLiteral("/tmp/cinder-preview.png"));
        if (image.isNull() || image.width() < 100)
            fail(QStringLiteral("window grab failed"));
        else {
            const QColor center = image.pixelColor(image.width() / 2, image.height() / 3);
            if (center.red() > 30 || center.green() > 30 || center.blue() > 30)
                fail(QStringLiteral("editor background is %1,%2,%3").arg(center.red()).arg(center.green()).arg(center.blue()));
            bool ember = false;
            for (int y = 0; y < image.height() && !ember; y += 2) {
                for (int x = 0; x < image.width(); x += 2) {
                    const QColor color = image.pixelColor(x, y);
                    if (color.red() > 200 && color.green() > 120 && color.blue() < 150) {
                        ember = true;
                        break;
                    }
                }
            }
            if (!ember)
                fail(QStringLiteral("dark theme is missing the ember accent"));
        }
        const QList<QToolButton *> buttons = window.findChildren<QToolButton *>(QStringLiteral("primary"));
        if (buttons.isEmpty())
            fail(QStringLiteral("Run button was not marked primary"));
    }
    QCoreApplication::setOrganizationName(org);
    QDir config(QDir::homePath() + QStringLiteral("/.config/cinder-selftest"));
    config.removeRecursively();
    if (gFailed == before)
        ok("window");
}

} // namespace

int runSelfTest()
{
    testLexer();
    testArgs();
    testLint();
    testEditor();
    testRun();
    testTrace();
    testDebug();
    testWindow();
    if (gFailed) {
        std::fprintf(stderr, "cinder: self-test failed (%d)\n", gFailed);
        return 1;
    }
    std::fprintf(stderr, "cinder: self-test ok\n");
    return 0;
}
