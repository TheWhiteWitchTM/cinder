#pragma once

#include "common.h"
#include "highlighter.h"

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QSet>

class CodeEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEdit(QWidget *parent = nullptr);

    void setDialect(Dialect dialect);
    Dialect dialect() const { return dialect_; }
    void setIndentTabs(bool tabs);
    bool indentTabs() const { return indentTabs_; }
    void setTabWidth(int spaces);
    int tabWidth() const { return tabWidth_; }
    void setFontPointSize(int points);
    int fontPointSize() const { return fontSize_; }

    void setExecLine(int line);
    int execLine() const { return execLine_; }
    QSet<int> breakpoints() const { return breakpoints_; }
    void setBreakpoints(const QSet<int> &lines);
    void toggleBreakpoint(int line);

    void commentSelection();
    void indentSelection(int direction);
    void setSearchNeedle(const QString &text);

    int gutterWidth() const;
    void paintGutter(QPaintEvent *event);
    void gutterClicked(QMouseEvent *event);

signals:
    void breakpointsChanged();
    void cursorChanged(int line, int column);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    class Gutter;
    void refreshGutter();
    void refreshSelections();
    void insertNewline();
    bool unindentOnBackspace();
    QString oneIndent() const;
    QString leadingWhitespace(const QString &line) const;

    Gutter *gutter_ = nullptr;
    Highlighter *highlighter_ = nullptr;
    Dialect dialect_ = Dialect::Bash;
    bool indentTabs_ = false;
    int tabWidth_ = 4;
    int fontSize_ = 13;
    int execLine_ = 0;
    QSet<int> breakpoints_;
    QString needle_;
};
