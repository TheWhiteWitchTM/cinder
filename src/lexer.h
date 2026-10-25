#pragma once

#include "common.h"

#include <QString>
#include <QVector>

enum class Tok : int {
    Plain = 0,
    Comment,
    Shebang,
    Keyword,
    Builtin,
    String,
    Variable,
    Number,
    Operator,
    Function,
    Bashism,
    Heredoc,
    Count
};

enum class Mode : int {
    Normal = 0,
    Single,
    Double,
    Backtick,
    Heredoc
};

struct Frame {
    Mode mode = Mode::Normal;
    int parens = 0;
    QString here;
    bool strip = false;
    bool literal = false;
};

struct LexerState {
    QVector<Frame> stack;
    QVector<Frame> pending;
    bool nextIsFunc = false;

    int signature() const;
    bool inQuote() const;
};

struct Token {
    int start = 0;
    int length = 0;
    Tok kind = Tok::Plain;
};

class Lexer {
public:
    static QVector<Token> tokenize(const QString &text, const LexerState &in, LexerState *out,
                                   bool firstLine, Dialect dialect);
};
