#pragma once

#include <QList>
#include <QString>
#include <QStringList>

enum class Dialect {
    Bash,
    Sh
};

struct Problem {
    QString file;
    int line = 1;
    int column = 1;
    QString level; // "error", "warning", "info"
    QString message;
    int code = 0;
};

struct Var {
    QString name;
    QString value;
};

QString dialectName(Dialect dialect);
Dialect detectDialect(const QString &path, const QString &text);
bool isShellPath(const QString &path);

// Program from a shebang, plus any extra arguments that follow it.
// Empty program means the line is not a shebang.
struct Shebang {
    QString program;
    QStringList extra;
};

Shebang parseShebang(const QString &text);
QStringList splitArgs(const QString &text, bool *ok = nullptr);
QString shellQuote(const QString &text);
bool looksBinary(const QByteArray &bytes);
QString findProgram(const QString &name);
