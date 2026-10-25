#pragma once

#include "lexer.h"

#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class Highlighter : public QSyntaxHighlighter {
public:
    explicit Highlighter(QTextDocument *document);
    void setDialect(Dialect dialect);

protected:
    void highlightBlock(const QString &text) override;

private:
    Dialect dialect_ = Dialect::Bash;
    QTextCharFormat formats_[int(Tok::Count)];
};
