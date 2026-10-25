#include "lint.h"

#include "lexer.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

QString bashismMessage(const QString &text)
{
    if (text.startsWith(QLatin1String("[[")) || text.startsWith(QLatin1String("]]")))
        return QStringLiteral("[[ ]] is bash. POSIX sh uses [ ] or test.");
    if (text == QLatin1String("function"))
        return QStringLiteral("function is bash. POSIX sh uses name().");
    if (text == QLatin1String("source"))
        return QStringLiteral("source is bash. POSIX sh uses a dot.");
    if (text.startsWith(QLatin1String("<(")) || text.startsWith(QLatin1String(">(")))
        return QStringLiteral("Process substitution is bash.");
    if (text.startsWith(QLatin1String("$'")))
        return QStringLiteral("$'...' is bash.");
    if (text.startsWith(QLatin1String("$\"")))
        return QStringLiteral("$\"...\" is bash.");
    if (text.startsWith(QLatin1String("<<")))
        return QStringLiteral("Here-strings are bash.");
    if (text.startsWith(QLatin1String("${")))
        return QStringLiteral("This parameter expansion is bash.");
    if (text == QLatin1String("=") || text == QLatin1String("("))
        return QStringLiteral("Arrays are bash.");
    return text + QStringLiteral(" is bash syntax.");
}

QList<Problem> builtinLint(const QString &path, const QString &text, Dialect dialect)
{
    QList<Problem> problems;
    if (text.isEmpty())
        return problems;
    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool namedScript = suffix == QLatin1String("sh") || suffix == QLatin1String("bash")
        || suffix == QLatin1String("bats");
    const QString first = text.section(QLatin1Char('\n'), 0, 0);
    if (namedScript && !path.isEmpty() && !first.startsWith(QLatin1String("#!"))) {
        Problem problem;
        problem.file = path;
        problem.line = 1;
        problem.column = 1;
        problem.level = QStringLiteral("info");
        problem.message = QStringLiteral("No shebang. Run uses the interpreter in the toolbar.");
        problems.append(problem);
    }
    const QStringList lines = text.split(QLatin1Char('\n'));
    LexerState state;
    for (int i = 0; i < lines.size(); ++i) {
        LexerState next;
        const QVector<Token> tokens = Lexer::tokenize(lines.at(i), state, &next, i == 0, dialect);
        if (dialect == Dialect::Sh) {
            for (const Token &token : tokens) {
                if (token.kind != Tok::Bashism)
                    continue;
                Problem problem;
                problem.file = path;
                problem.line = i + 1;
                problem.column = token.start + 1;
                problem.level = QStringLiteral("warning");
                problem.message = bashismMessage(lines.at(i).mid(token.start, token.length));
                problems.append(problem);
            }
        }
        state = next;
    }
    if (state.inQuote()) {
        Problem problem;
        problem.file = path;
        problem.line = qMax(1, lines.size());
        problem.column = 1;
        problem.level = QStringLiteral("error");
        problem.message = QStringLiteral("Unclosed quote.");
        problems.append(problem);
    }
    return problems;
}

QList<Problem> parseShellcheck(const QByteArray &json, const QString &path)
{
    QList<Problem> problems;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    QJsonArray comments;
    if (doc.isArray())
        comments = doc.array();
    else if (doc.isObject())
        comments = doc.object().value(QStringLiteral("comments")).toArray();
    for (const QJsonValue &value : comments) {
        const QJsonObject obj = value.toObject();
        Problem problem;
        problem.file = path;
        problem.line = qMax(1, obj.value(QStringLiteral("line")).toInt());
        problem.column = qMax(1, obj.value(QStringLiteral("column")).toInt());
        problem.level = obj.value(QStringLiteral("level")).toString(QStringLiteral("warning"));
        problem.code = obj.value(QStringLiteral("code")).toInt();
        const QString message = obj.value(QStringLiteral("message")).toString();
        problem.message = problem.code > 0
            ? QStringLiteral("SC%1: %2").arg(problem.code).arg(message)
            : message;
        problems.append(problem);
    }
    return problems;
}
