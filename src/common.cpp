#include "common.h"

#include <QFileInfo>
#include <QStandardPaths>

QString dialectName(Dialect dialect)
{
    return dialect == Dialect::Bash ? QStringLiteral("Bash") : QStringLiteral("sh");
}

static QString firstLine(const QString &text)
{
    const int n = text.indexOf(QLatin1Char('\n'));
    return n < 0 ? text : text.left(n);
}

Shebang parseShebang(const QString &text)
{
    Shebang out;
    QString line = firstLine(text).trimmed();
    if (line.endsWith(QLatin1Char('\r')))
        line.chop(1);
    if (!line.startsWith(QLatin1String("#!")))
        return out;
    line = line.mid(2).trimmed();
    const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return out;
    if (parts.at(0).endsWith(QLatin1String("/env")) && parts.size() >= 2) {
        out.program = parts.at(1);
        out.extra = parts.mid(2);
        return out;
    }
    out.program = parts.at(0);
    out.extra = parts.mid(1);
    return out;
}

Dialect detectDialect(const QString &path, const QString &text)
{
    const Shebang shebang = parseShebang(text);
    const QString program = shebang.program.isEmpty()
        ? QString()
        : QFileInfo(shebang.program).fileName();
    if (program == QLatin1String("bash") || program == QLatin1String("bash.exe"))
        return Dialect::Bash;
    if (program == QLatin1String("sh") || program == QLatin1String("dash") || program == QLatin1String("ash"))
        return Dialect::Sh;
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("bash") || suffix == QLatin1String("bats"))
        return Dialect::Bash;
    if (suffix == QLatin1String("sh"))
        return Dialect::Sh;
    return Dialect::Bash;
}

bool isShellPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("sh") || suffix == QLatin1String("bash")
        || suffix == QLatin1String("bats") || suffix == QLatin1String("ksh")
        || suffix == QLatin1String("zsh");
}

QStringList splitArgs(const QString &text, bool *ok)
{
    QStringList out;
    QString cur;
    QChar quote;
    bool any = false;
    bool escape = false;
    const auto flush = [&]() {
        if (any)
            out.append(cur);
        cur.clear();
        any = false;
    };
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (escape) {
            cur.append(c);
            any = true;
            escape = false;
            continue;
        }
        if (c == QLatin1Char('\\') && quote != QLatin1Char('\'')) {
            escape = true;
            any = true;
            continue;
        }
        if (quote.isNull() && (c == QLatin1Char('\'') || c == QLatin1Char('"'))) {
            quote = c;
            any = true;
            continue;
        }
        if (!quote.isNull() && c == quote) {
            quote = QChar();
            any = true;
            continue;
        }
        if (quote.isNull() && c.isSpace()) {
            flush();
            continue;
        }
        cur.append(c);
        any = true;
    }
    if (escape)
        cur.append(QLatin1Char('\\'));
    flush();
    if (ok)
        *ok = quote.isNull();
    return out;
}

QString shellQuote(const QString &text)
{
    QString out = QStringLiteral("'");
    for (const QChar c : text) {
        if (c == QLatin1Char('\''))
            out += QStringLiteral("'\\''");
        else
            out += c;
    }
    out += QLatin1Char('\'');
    return out;
}

bool looksBinary(const QByteArray &bytes)
{
    const int n = qMin(bytes.size(), 8192);
    for (int i = 0; i < n; ++i) {
        if (bytes.at(i) == '\0')
            return true;
    }
    return false;
}

QString findProgram(const QString &name)
{
    if (name.isEmpty())
        return {};
    if (name.contains(QLatin1Char('/')))
        return name;
    const QString found = QStandardPaths::findExecutable(name);
    if (!found.isEmpty())
        return found;
    if (name == QLatin1String("bash"))
        return QStringLiteral("/bin/bash");
    if (name == QLatin1String("sh"))
        return QStringLiteral("/bin/sh");
    return name;
}
