#include "highlighter.h"

namespace {

class BlockData : public QTextBlockUserData {
public:
    LexerState state;
};

QTextCharFormat makeFormat(const QString &color, bool bold = false, bool italic = false, bool underline = false)
{
    QTextCharFormat format;
    format.setForeground(QColor(color));
    if (bold)
        format.setFontWeight(QFont::DemiBold);
    if (italic)
        format.setFontItalic(true);
    if (underline)
        format.setFontUnderline(true);
    return format;
}

} // namespace

Highlighter::Highlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    formats_[int(Tok::Plain)] = makeFormat(QStringLiteral("#e8e6e3"));
    formats_[int(Tok::Comment)] = makeFormat(QStringLiteral("#6a6a6a"), false, true);
    formats_[int(Tok::Shebang)] = makeFormat(QStringLiteral("#8a8a8a"), false, true);
    formats_[int(Tok::Keyword)] = makeFormat(QStringLiteral("#ffb454"), true);
    formats_[int(Tok::Builtin)] = makeFormat(QStringLiteral("#7eb6ff"));
    formats_[int(Tok::String)] = makeFormat(QStringLiteral("#b7e1a8"));
    formats_[int(Tok::Variable)] = makeFormat(QStringLiteral("#8fd4ff"));
    formats_[int(Tok::Number)] = makeFormat(QStringLiteral("#ffd18a"));
    formats_[int(Tok::Operator)] = makeFormat(QStringLiteral("#d0d0d0"));
    formats_[int(Tok::Function)] = makeFormat(QStringLiteral("#f0d7a2"));
    formats_[int(Tok::Bashism)] = makeFormat(QStringLiteral("#ff7b72"), false, false, true);
    formats_[int(Tok::Heredoc)] = makeFormat(QStringLiteral("#9fbfa4"));
}

void Highlighter::setDialect(Dialect dialect)
{
    if (dialect_ == dialect)
        return;
    dialect_ = dialect;
    rehighlight();
}

void Highlighter::highlightBlock(const QString &text)
{
    LexerState in;
    const QTextBlock prev = currentBlock().previous();
    if (prev.isValid()) {
        if (const auto *data = dynamic_cast<const BlockData *>(prev.userData()))
            in = data->state;
    }
    LexerState out;
    const bool first = currentBlock().position() == 0;
    const QVector<Token> tokens = Lexer::tokenize(text, in, &out, first, dialect_);
    auto *data = new BlockData;
    data->state = out;
    setCurrentBlockUserData(data);
    const int signature = out.signature();
    setCurrentBlockState(signature == 0 ? 1 : signature);
    for (const Token &token : tokens) {
        if (token.length <= 0 || token.kind == Tok::Plain)
            continue;
        const int kind = int(token.kind);
        if (kind >= 0 && kind < int(Tok::Count))
            setFormat(token.start, token.length, formats_[kind]);
    }
}
