#include "lexer.h"

#include <QHash>
#include <QSet>

namespace {

QSet<QString> keywordSet()
{
    return {
        QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"), QStringLiteral("elif"),
        QStringLiteral("fi"), QStringLiteral("for"), QStringLiteral("while"), QStringLiteral("until"),
        QStringLiteral("do"), QStringLiteral("done"), QStringLiteral("case"), QStringLiteral("esac"),
        QStringLiteral("in"), QStringLiteral("function"), QStringLiteral("select"), QStringLiteral("time"),
        QStringLiteral("coproc"),
    };
}

QSet<QString> builtinSet()
{
    return {
        QStringLiteral("echo"), QStringLiteral("printf"), QStringLiteral("read"), QStringLiteral("cd"),
        QStringLiteral("pwd"), QStringLiteral("pushd"), QStringLiteral("popd"), QStringLiteral("export"),
        QStringLiteral("unset"), QStringLiteral("local"), QStringLiteral("declare"), QStringLiteral("typeset"),
        QStringLiteral("readonly"), QStringLiteral("shift"), QStringLiteral("exit"), QStringLiteral("return"),
        QStringLiteral("break"), QStringLiteral("continue"), QStringLiteral("source"), QStringLiteral("eval"),
        QStringLiteral("exec"), QStringLiteral("trap"), QStringLiteral("set"), QStringLiteral("shopt"),
        QStringLiteral("alias"), QStringLiteral("unalias"), QStringLiteral("ulimit"), QStringLiteral("umask"),
        QStringLiteral("wait"), QStringLiteral("jobs"), QStringLiteral("fg"), QStringLiteral("bg"),
        QStringLiteral("kill"), QStringLiteral("let"), QStringLiteral("test"), QStringLiteral("mapfile"),
        QStringLiteral("readarray"), QStringLiteral("getopts"), QStringLiteral("hash"), QStringLiteral("type"),
        QStringLiteral("command"), QStringLiteral("builtin"), QStringLiteral("enable"), QStringLiteral("caller"),
        QStringLiteral("complete"), QStringLiteral("compgen"), QStringLiteral("dirs"), QStringLiteral("true"),
        QStringLiteral("false"), QStringLiteral("logout"), QStringLiteral("times"), QStringLiteral(":"),
        QStringLiteral("."), QStringLiteral("["),
    };
}

QSet<QString> bashOnlySet()
{
    return {
        QStringLiteral("source"), QStringLiteral("declare"), QStringLiteral("typeset"),
        QStringLiteral("coproc"), QStringLiteral("select"), QStringLiteral("mapfile"),
        QStringLiteral("readarray"), QStringLiteral("shopt"), QStringLiteral("caller"),
    };
}

const QSet<QString> &keywords()
{
    static const QSet<QString> set = keywordSet();
    return set;
}

const QSet<QString> &builtins()
{
    static const QSet<QString> set = builtinSet();
    return set;
}

const QSet<QString> &bashOnly()
{
    static const QSet<QString> set = bashOnlySet();
    return set;
}

bool isNameStart(QChar c)
{
    return c.isLetter() || c == QLatin1Char('_');
}

bool isNameChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

bool isWordChar(QChar c)
{
    if (c.isSpace() || c == QLatin1Char('\r'))
        return false;
    switch (c.unicode()) {
    case '#':
    case '\'':
    case '"':
    case '`':
    case '$':
    case '\\':
    case ';':
    case '|':
    case '&':
    case '<':
    case '>':
    case '(':
    case ')':
    case '{':
    case '}':
    case '!':
        return false;
    default:
        return true;
    }
}

bool isIdent(const QString &word)
{
    if (word.isEmpty() || !isNameStart(word.at(0)))
        return false;
    for (int i = 1; i < word.size(); ++i) {
        if (!isNameChar(word.at(i)))
            return false;
    }
    return true;
}

bool isNumber(const QString &word)
{
    if (word.isEmpty())
        return false;
    int i = 0;
    if (word.size() > 2 && word.at(0) == QLatin1Char('0')
        && (word.at(1) == QLatin1Char('x') || word.at(1) == QLatin1Char('X'))) {
        i = 2;
        if (i >= word.size())
            return false;
        for (; i < word.size(); ++i) {
            if (!word.at(i).isDigit() && !word.at(i).isLetter())
                return false;
            const QChar c = word.at(i).toLower();
            if (c.isLetter() && (c < QLatin1Char('a') || c > QLatin1Char('f')))
                return false;
        }
        return true;
    }
    for (; i < word.size(); ++i) {
        if (!word.at(i).isDigit())
            return false;
    }
    return true;
}

bool bashParam(const QString &body)
{
    QString inner = body;
    if (inner.startsWith(QLatin1String("${")))
        inner = inner.mid(2);
    if (inner.endsWith(QLatin1Char('}')))
        inner.chop(1);
    if (inner.startsWith(QLatin1Char('!')))
        return true;
    if (inner.contains(QLatin1String("//")) || inner.contains(QLatin1String("^^"))
        || inner.contains(QLatin1String(",,")))
        return true;
    for (int i = 0; i < inner.size(); ++i) {
        if (inner.at(i) != QLatin1Char(':'))
            continue;
        const QChar next = (i + 1 < inner.size()) ? inner.at(i + 1) : QChar();
        if (next != QLatin1Char('-') && next != QLatin1Char('=') && next != QLatin1Char('?')
            && next != QLatin1Char('+') && next != QLatin1Char(':'))
            return true;
    }
    return false;
}

struct Scan {
    const QString &text;
    Dialect dialect;
    LexerState *state;
    QVector<Token> tokens;
    bool arrayNext = false;

    explicit Scan(const QString &text, Dialect dialect, LexerState *state)
        : text(text)
        , dialect(dialect)
        , state(state)
    {
    }

    int n() const { return text.size(); }

    void add(int start, int length, Tok kind)
    {
        if (length > 0)
            tokens.append(Token{start, length, kind});
    }

    bool functionAhead(int index) const
    {
        int j = index;
        while (j < n() && text.at(j).isSpace())
            ++j;
        return j + 1 < n() && text.at(j) == QLatin1Char('(') && text.at(j + 1) == QLatin1Char(')');
    }

    void emitWord(int start, int length)
    {
        const QString word = text.mid(start, length);
        if (state->nextIsFunc && isIdent(word)) {
            add(start, length, Tok::Function);
            state->nextIsFunc = false;
            return;
        }
        state->nextIsFunc = false;
        const int eq = word.indexOf(QLatin1Char('='));
        if (eq > 0 && isIdent(word.left(eq))) {
            add(start, eq, Tok::Variable);
            const bool array = eq + 1 == word.size() && start + length < n()
                && text.at(start + length) == QLatin1Char('(');
            add(start + eq, 1, (array && dialect == Dialect::Sh) ? Tok::Bashism : Tok::Operator);
            if (eq + 1 < word.size()) {
                const QString value = word.mid(eq + 1);
                add(start + eq + 1, value.size(), isNumber(value) ? Tok::Number : Tok::Plain);
            } else if (array && dialect == Dialect::Sh) {
                arrayNext = true;
            }
            return;
        }
        Tok kind = Tok::Plain;
        if (keywords().contains(word))
            kind = Tok::Keyword;
        else if (dialect == Dialect::Sh && bashOnly().contains(word))
            kind = Tok::Bashism;
        else if (builtins().contains(word))
            kind = Tok::Builtin;
        else if (isNumber(word))
            kind = Tok::Number;
        else if (isIdent(word) && functionAhead(start + length))
            kind = Tok::Function;
        if (word == QLatin1String("function")) {
            kind = dialect == Dialect::Sh ? Tok::Bashism : Tok::Keyword;
            state->nextIsFunc = true;
        } else if (kind == Tok::Keyword && dialect == Dialect::Sh
                   && (word == QLatin1String("select") || word == QLatin1String("coproc"))) {
            kind = Tok::Bashism;
        }
        add(start, length, kind);
    }

    int scanDollar(int i)
    {
        if (i + 1 < n() && text.at(i + 1) == QLatin1Char('(')) {
            add(i, 2, Tok::Operator);
            Frame frame;
            frame.mode = Mode::Normal;
            frame.parens = 1;
            state->stack.append(frame);
            return i + 2;
        }
        if (i + 1 < n() && (text.at(i + 1) == QLatin1Char('\'') || text.at(i + 1) == QLatin1Char('"'))) {
            const QChar quote = text.at(i + 1);
            int j = i + 2;
            while (j < n()) {
                if (text.at(j) == QLatin1Char('\\') && quote == QLatin1Char('\'')) {
                    j = qMin(n(), j + 2);
                    continue;
                }
                if (text.at(j) == QLatin1Char('\\') && quote == QLatin1Char('"')) {
                    j = qMin(n(), j + 2);
                    continue;
                }
                if (text.at(j) == quote) {
                    ++j;
                    break;
                }
                ++j;
            }
            const Tok kind = dialect == Dialect::Sh ? Tok::Bashism : Tok::String;
            add(i, j - i, kind);
            return j;
        }
        if (i + 1 < n() && text.at(i + 1) == QLatin1Char('{')) {
            int j = i + 2;
            int depth = 1;
            while (j < n() && depth > 0) {
                const QChar c = text.at(j);
                if (c == QLatin1Char('\\')) {
                    j = qMin(n(), j + 2);
                    continue;
                }
                if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
                    const QChar quote = c;
                    ++j;
                    while (j < n() && text.at(j) != quote) {
                        if (text.at(j) == QLatin1Char('\\') && quote == QLatin1Char('"'))
                            j += 2;
                        else
                            ++j;
                    }
                    if (j < n())
                        ++j;
                    continue;
                }
                if (c == QLatin1Char('{'))
                    ++depth;
                else if (c == QLatin1Char('}'))
                    --depth;
                ++j;
            }
            const QString body = text.mid(i, j - i);
            const Tok kind = (dialect == Dialect::Sh && bashParam(body)) ? Tok::Bashism : Tok::Variable;
            add(i, j - i, kind);
            return j;
        }
        int j = i + 1;
        if (j < n()) {
            const QChar c = text.at(j);
            if (c.isDigit() || QStringLiteral("@*#?$!-_").contains(c))
                ++j;
            else
                while (j < n() && isNameChar(text.at(j)))
                    ++j;
        }
        if (j == i + 1) {
            add(i, 1, Tok::Operator);
            return i + 1;
        }
        add(i, j - i, Tok::Variable);
        return j;
    }

    int scanSingle(int i)
    {
        const int start = i;
        while (i < n()) {
            if (text.at(i) == QLatin1Char('\'')) {
                add(start, i + 1 - start, Tok::String);
                if (!state->stack.isEmpty())
                    state->stack.removeLast();
                return i + 1;
            }
            ++i;
        }
        add(start, n() - start, Tok::String);
        return n();
    }

    int scanDouble(int i)
    {
        int start = i;
        while (i < n()) {
            const QChar c = text.at(i);
            if (c == QLatin1Char('\\')) {
                i = qMin(n(), i + 2);
                continue;
            }
            if (c == QLatin1Char('"')) {
                add(start, i + 1 - start, Tok::String);
                if (!state->stack.isEmpty())
                    state->stack.removeLast();
                return i + 1;
            }
            if (c == QLatin1Char('$')) {
                if (i > start)
                    add(start, i - start, Tok::String);
                const int before = state->stack.size();
                const int next = scanDollar(i);
                if (state->stack.size() != before)
                    return next;
                start = next;
                i = next;
                continue;
            }
            if (c == QLatin1Char('`')) {
                if (i > start)
                    add(start, i - start, Tok::String);
                add(i, 1, Tok::Operator);
                Frame frame;
                frame.mode = Mode::Backtick;
                state->stack.append(frame);
                return i + 1;
            }
            ++i;
        }
        if (start < n())
            add(start, n() - start, Tok::String);
        return n();
    }

    int scanHeredoc(int i)
    {
        Frame frame = state->stack.back();
        QString view = text;
        int skip = 0;
        if (frame.strip) {
            while (skip < view.size() && view.at(skip) == QLatin1Char('\t'))
                ++skip;
            view = view.mid(skip);
        }
        if (view == frame.here) {
            add(0, n(), Tok::Keyword);
            state->stack.removeLast();
            return n();
        }
        if (frame.literal || i > 0) {
            add(0, n(), Tok::Heredoc);
            return n();
        }
        int start = 0;
        while (i < n()) {
            const QChar c = text.at(i);
            if (c == QLatin1Char('\\')) {
                i = qMin(n(), i + 2);
                continue;
            }
            if (c == QLatin1Char('$') || c == QLatin1Char('`')) {
                if (i > start)
                    add(start, i - start, Tok::Heredoc);
                if (c == QLatin1Char('$')) {
                    const int before = state->stack.size();
                    const int next = scanDollar(i);
                    if (state->stack.size() != before) {
                        // Command substitution inside a heredoc still belongs to the body.
                        // Drop the frame so the rest of the line stays a heredoc.
                        while (state->stack.size() > before)
                            state->stack.removeLast();
                        add(i, next - i, Tok::Variable);
                    }
                    start = next;
                    i = next;
                    continue;
                }
                add(i, 1, Tok::Operator);
                start = i + 1;
                i = start;
                continue;
            }
            ++i;
        }
        if (start < n())
            add(start, n() - start, Tok::Heredoc);
        return n();
    }

    int scanNormal(int i)
    {
        const bool backtick = !state->stack.isEmpty() && state->stack.back().mode == Mode::Backtick;
        const QChar c = text.at(i);
        if (backtick && c == QLatin1Char('`')) {
            add(i, 1, Tok::Operator);
            state->stack.removeLast();
            return i + 1;
        }
        if (c.isSpace() || c == QLatin1Char('\r')) {
            int j = i + 1;
            while (j < n() && (text.at(j).isSpace() || text.at(j) == QLatin1Char('\r')))
                ++j;
            return j;
        }
        if (c == QLatin1Char('\\')) {
            const int length = (i + 1 < n()) ? 2 : 1;
            add(i, length, Tok::Operator);
            arrayNext = false;
            return i + length;
        }
        if (c == QLatin1Char('#')) {
            bool start = i == 0;
            if (!start) {
                const QChar prev = text.at(i - 1);
                start = prev.isSpace() || prev == QLatin1Char(';') || prev == QLatin1Char('|')
                    || prev == QLatin1Char('&') || prev == QLatin1Char('(') || prev == QLatin1Char('{')
                    || prev == QLatin1Char('<') || prev == QLatin1Char('>');
            }
            if (start) {
                add(i, n() - i, Tok::Comment);
                state->nextIsFunc = false;
                return n();
            }
        }
        if (c == QLatin1Char('\'')) {
            add(i, 1, Tok::String);
            Frame frame;
            frame.mode = Mode::Single;
            state->stack.append(frame);
            state->nextIsFunc = false;
            arrayNext = false;
            return i + 1;
        }
        if (c == QLatin1Char('"')) {
            add(i, 1, Tok::String);
            Frame frame;
            frame.mode = Mode::Double;
            state->stack.append(frame);
            state->nextIsFunc = false;
            arrayNext = false;
            return i + 1;
        }
        if (c == QLatin1Char('`')) {
            add(i, 1, Tok::Operator);
            Frame frame;
            frame.mode = Mode::Backtick;
            state->stack.append(frame);
            state->nextIsFunc = false;
            return i + 1;
        }
        if (c == QLatin1Char('$')) {
            arrayNext = false;
            return scanDollar(i);
        }
        if (c == QLatin1Char('<') && i + 2 < n() && text.at(i + 1) == QLatin1Char('<')
            && text.at(i + 2) == QLatin1Char('<')) {
            add(i, 3, dialect == Dialect::Sh ? Tok::Bashism : Tok::Operator);
            return i + 3;
        }
        if (c == QLatin1Char('<') && i + 1 < n() && text.at(i + 1) == QLatin1Char('<')) {
            int j = i + 2;
            bool strip = false;
            if (j < n() && text.at(j) == QLatin1Char('-')) {
                strip = true;
                ++j;
            }
            const int opEnd = j;
            while (j < n() && text.at(j).isSpace())
                ++j;
            QString word;
            bool literal = false;
            int wordStart = j;
            if (j < n() && (text.at(j) == QLatin1Char('\'') || text.at(j) == QLatin1Char('"'))) {
                literal = true;
                const QChar quote = text.at(j);
                ++j;
                wordStart = j;
                while (j < n() && text.at(j) != quote)
                    ++j;
                word = text.mid(wordStart, j - wordStart);
                if (j < n())
                    ++j;
            } else if (j < n() && text.at(j) == QLatin1Char('\\')) {
                literal = true;
                ++j;
                wordStart = j;
                while (j < n() && isWordChar(text.at(j)))
                    ++j;
                word = text.mid(wordStart, j - wordStart);
            } else {
                wordStart = j;
                while (j < n() && isWordChar(text.at(j)))
                    ++j;
                word = text.mid(wordStart, j - wordStart);
            }
            if (!word.isEmpty()) {
                add(i, opEnd - i, Tok::Operator);
                int k = opEnd;
                while (k < wordStart && k < n() && text.at(k).isSpace())
                    ++k;
                if (k > opEnd)
                    add(opEnd, k - opEnd, Tok::Plain);
                add(k, j - k, Tok::String);
                Frame here;
                here.mode = Mode::Heredoc;
                here.here = word;
                here.strip = strip;
                here.literal = literal;
                state->pending.append(here);
                return j;
            }
        }
        auto two = [&](QChar a, QChar b) {
            return i + 1 < n() && c == a && text.at(i + 1) == b;
        };
        if (two(QLatin1Char('&'), QLatin1Char('&')) || two(QLatin1Char('|'), QLatin1Char('|'))
            || two(QLatin1Char(';'), QLatin1Char(';')) || two(QLatin1Char('|'), QLatin1Char('&'))
            || two(QLatin1Char('>'), QLatin1Char('>')) || two(QLatin1Char('<'), QLatin1Char('&'))
            || two(QLatin1Char('>'), QLatin1Char('&')) || two(QLatin1Char('<'), QLatin1Char('>'))) {
            add(i, 2, Tok::Operator);
            state->nextIsFunc = false;
            return i + 2;
        }
        if ((c == QLatin1Char('<') || c == QLatin1Char('>')) && i + 1 < n()
            && text.at(i + 1) == QLatin1Char('(')) {
            add(i, 2, dialect == Dialect::Sh ? Tok::Bashism : Tok::Operator);
            return i + 2;
        }
        if (c == QLatin1Char('[') && i + 1 < n() && text.at(i + 1) == QLatin1Char('[')) {
            add(i, 2, dialect == Dialect::Sh ? Tok::Bashism : Tok::Keyword);
            return i + 2;
        }
        if (c == QLatin1Char(']') && i + 1 < n() && text.at(i + 1) == QLatin1Char(']')) {
            add(i, 2, dialect == Dialect::Sh ? Tok::Bashism : Tok::Keyword);
            return i + 2;
        }
        if (c == QLatin1Char('(') && arrayNext) {
            add(i, 1, Tok::Bashism);
            arrayNext = false;
            return i + 1;
        }
        if (QStringLiteral(";|&<>(){}!").contains(c)) {
            if (c == QLatin1Char(')') && !state->stack.isEmpty()
                && state->stack.back().mode == Mode::Normal && state->stack.back().parens > 0) {
                state->stack.back().parens--;
                add(i, 1, Tok::Operator);
                if (state->stack.back().parens == 0)
                    state->stack.removeLast();
                return i + 1;
            }
            if (c == QLatin1Char('(') && !state->stack.isEmpty()
                && state->stack.back().mode == Mode::Normal && state->stack.back().parens > 0)
                state->stack.back().parens++;
            add(i, 1, Tok::Operator);
            if (c != QLatin1Char('(') && c != QLatin1Char(')'))
                state->nextIsFunc = false;
            arrayNext = false;
            return i + 1;
        }
        int j = i + 1;
        while (j < n() && isWordChar(text.at(j)))
            ++j;
        emitWord(i, j - i);
        return j;
    }
};

} // namespace

int LexerState::signature() const
{
    quint32 hash = 2166136261u;
    const auto mix = [&hash](quint32 value) {
        hash ^= value;
        hash *= 16777619u;
    };
    mix(quint32(stack.size() + 1));
    for (const Frame &frame : stack) {
        mix(quint32(frame.mode) + 3);
        mix(quint32(frame.parens + 9));
        mix(quint32(frame.strip) + (frame.literal ? 4u : 0u));
        mix(qHash(frame.here));
    }
    mix(quint32(pending.size() + 17));
    for (const Frame &frame : pending) {
        mix(qHash(frame.here));
        mix(frame.strip ? 1u : 2u);
        mix(frame.literal ? 3u : 5u);
    }
    mix(nextIsFunc ? 8u : 9u);
    if (hash == 0)
        hash = 1;
    return int(hash & 0x7fffffff);
}

bool LexerState::inQuote() const
{
    if (stack.isEmpty())
        return false;
    const Mode mode = stack.back().mode;
    return mode == Mode::Single || mode == Mode::Double;
}

QVector<Token> Lexer::tokenize(const QString &text, const LexerState &in, LexerState *out, bool firstLine,
                               Dialect dialect)
{
    LexerState state = in;
    if ((state.stack.isEmpty() || state.stack.back().mode != Mode::Heredoc) && !state.pending.isEmpty()) {
        state.stack.append(state.pending.front());
        state.pending.removeFirst();
    }
    if (firstLine && text.startsWith(QLatin1String("#!"))) {
        if (out)
            *out = state;
        return {Token{0, int(text.size()), Tok::Shebang}};
    }
    Scan scan(text, dialect, &state);
    int i = 0;
    int guard = 0;
    while (i < text.size() && guard < text.size() + 8) {
        ++guard;
        const int before = i;
        const Mode mode = state.stack.isEmpty() ? Mode::Normal : state.stack.back().mode;
        if (mode == Mode::Heredoc)
            i = scan.scanHeredoc(i);
        else if (mode == Mode::Single)
            i = scan.scanSingle(i);
        else if (mode == Mode::Double)
            i = scan.scanDouble(i);
        else
            i = scan.scanNormal(i);
        if (i <= before)
            i = before + 1;
    }
    if (i < text.size())
        scan.add(i, text.size() - i, Tok::Plain);
    if (out)
        *out = state;
    return scan.tokens;
}
