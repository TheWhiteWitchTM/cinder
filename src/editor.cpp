#include "editor.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QWheelEvent>

namespace {

QFont editorFont(int points)
{
    QFont font(QStringLiteral("Fira Mono"), points);
    if (!QFontInfo(font).exactMatch())
        font = QFont(QStringLiteral("DejaVu Sans Mono"), points);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

bool extraIndent(const QString &trimmed)
{
    if (trimmed.endsWith(QLatin1Char('{')) || trimmed.endsWith(QLatin1Char('|'))
        || trimmed.endsWith(QLatin1String("&&")) || trimmed.endsWith(QLatin1String("||"))
        || trimmed.endsWith(QLatin1Char('\\')))
        return true;
    static const QRegularExpression opener(
        QStringLiteral("^(if|elif|else|for|while|until|case|select)\\b"));
    static const QRegularExpression tail(QStringLiteral("\\b(then|do)\\s*$"));
    static const QRegularExpression closer(QStringLiteral("^(fi|done|esac)\\b"));
    if (closer.match(trimmed).hasMatch())
        return false;
    return opener.match(trimmed).hasMatch() || tail.match(trimmed).hasMatch();
}

} // namespace

class CodeEdit::Gutter : public QWidget {
public:
    explicit Gutter(CodeEdit *edit)
        : QWidget(edit)
        , edit_(edit)
    {
    }

    QSize sizeHint() const override
    {
        return QSize(edit_->gutterWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        edit_->paintGutter(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        edit_->gutterClicked(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        QCoreApplication::sendEvent(edit_->viewport(), event);
    }

private:
    CodeEdit *edit_;
};

CodeEdit::CodeEdit(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setObjectName(QStringLiteral("codeEdit"));
    gutter_ = new Gutter(this);
    highlighter_ = new Highlighter(document());
    setFontPointSize(13);
    setTabWidth(4);
    setLineWrapMode(NoWrap);
    setCursorWidth(2);
    setFrameStyle(QFrame::NoFrame);
    QPalette pal = palette();
    pal.setColor(QPalette::Base, QColor(QStringLiteral("#000000")));
    pal.setColor(QPalette::Text, QColor(QStringLiteral("#e8e6e3")));
    pal.setColor(QPalette::Highlight, QColor(QStringLiteral("#3a2a14")));
    pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#fff6e4")));
    setPalette(pal);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this](int) { refreshGutter(); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &rect, int dy) {
        if (dy)
            gutter_->scroll(0, dy);
        else
            gutter_->update(0, rect.y(), gutter_->width(), rect.height());
        if (rect.contains(viewport()->rect()))
            refreshGutter();
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        refreshSelections();
        const QTextCursor cursor = textCursor();
        emit cursorChanged(cursor.blockNumber() + 1, cursor.positionInBlock() + 1);
        gutter_->update();
    });
    refreshGutter();
}

void CodeEdit::setDialect(Dialect dialect)
{
    dialect_ = dialect;
    highlighter_->setDialect(dialect);
}

void CodeEdit::setIndentTabs(bool tabs)
{
    indentTabs_ = tabs;
}

void CodeEdit::setTabWidth(int spaces)
{
    tabWidth_ = qBound(1, spaces, 8);
    setTabStopDistance(tabWidth_ * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
}

void CodeEdit::setFontPointSize(int points)
{
    fontSize_ = qBound(8, points, 32);
    setFont(editorFont(fontSize_));
    setTabStopDistance(tabWidth_ * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
    refreshGutter();
}

void CodeEdit::setExecLine(int line)
{
    execLine_ = line;
    if (line > 0) {
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (block.isValid()) {
            QTextCursor cursor(block);
            setTextCursor(cursor);
            ensureCursorVisible();
        }
    }
    refreshSelections();
    gutter_->update();
}

void CodeEdit::setBreakpoints(const QSet<int> &lines)
{
    if (breakpoints_ == lines)
        return;
    breakpoints_ = lines;
    gutter_->update();
    emit breakpointsChanged();
}

void CodeEdit::toggleBreakpoint(int line)
{
    if (line < 1)
        return;
    if (breakpoints_.contains(line))
        breakpoints_.remove(line);
    else
        breakpoints_.insert(line);
    gutter_->update();
    emit breakpointsChanged();
}

void CodeEdit::commentSelection()
{
    QTextCursor cursor = textCursor();
    int start = cursor.selectionStart();
    int end = cursor.selectionEnd();
    cursor.setPosition(start);
    const int first = cursor.blockNumber();
    cursor.setPosition(end);
    int last = cursor.blockNumber();
    if (end > start && cursor.positionInBlock() == 0 && last > first)
        --last;
    cursor.beginEditBlock();
    for (int number = first; number <= last; ++number) {
        QTextBlock block = document()->findBlockByNumber(number);
        if (!block.isValid())
            continue;
        const QString text = block.text();
        int indent = 0;
        while (indent < text.size() && text.at(indent).isSpace())
            ++indent;
        QTextCursor line(block);
        const QString rest = text.mid(indent);
        if (rest.startsWith(QLatin1Char('#'))) {
            line.setPosition(block.position() + indent);
            int remove = 1;
            if (rest.size() > 1 && rest.at(1) == QLatin1Char(' '))
                remove = 2;
            line.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, remove);
            line.removeSelectedText();
        } else {
            line.setPosition(block.position() + indent);
            line.insertText(QStringLiteral("# "));
        }
    }
    cursor.endEditBlock();
}

void CodeEdit::indentSelection(int direction)
{
    QTextCursor cursor = textCursor();
    if (direction > 0 && !cursor.hasSelection()) {
        cursor.insertText(oneIndent());
        return;
    }
    int start = cursor.selectionStart();
    int end = cursor.selectionEnd();
    if (!cursor.hasSelection()) {
        start = cursor.block().position();
        end = start + cursor.block().length() - 1;
    }
    cursor.setPosition(start);
    const int first = cursor.blockNumber();
    cursor.setPosition(qMax(start, end));
    int last = cursor.blockNumber();
    if (end > start && cursor.positionInBlock() == 0 && last > first)
        --last;
    cursor.beginEditBlock();
    for (int number = first; number <= last; ++number) {
        QTextBlock block = document()->findBlockByNumber(number);
        if (!block.isValid())
            continue;
        QTextCursor line(block);
        if (direction > 0) {
            line.setPosition(block.position());
            line.insertText(oneIndent());
            continue;
        }
        const QString text = block.text();
        int remove = 0;
        if (text.startsWith(QLatin1Char('\t')))
            remove = 1;
        else {
            while (remove < tabWidth_ && remove < text.size() && text.at(remove) == QLatin1Char(' '))
                ++remove;
        }
        if (remove > 0) {
            line.setPosition(block.position());
            line.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, remove);
            line.removeSelectedText();
        }
    }
    cursor.endEditBlock();
}

void CodeEdit::setSearchNeedle(const QString &text)
{
    if (needle_ == text)
        return;
    needle_ = text;
    refreshSelections();
}

int CodeEdit::gutterWidth() const
{
    const int digits = qMax(3, QString::number(qMax(1, blockCount())).size());
    return 28 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits + 10;
}

void CodeEdit::paintGutter(QPaintEvent *event)
{
    QPainter painter(gutter_);
    painter.fillRect(event->rect(), QColor(QStringLiteral("#000000")));
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int width = gutter_->width();
    const int current = textCursor().blockNumber() + 1;
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const int line = block.blockNumber() + 1;
            const int height = bottom - top;
            if (line == execLine_)
                painter.fillRect(0, top, width, height, QColor(QStringLiteral("#2a1c08")));
            else if (line == current)
                painter.fillRect(0, top, width, height, QColor(QStringLiteral("#101010")));
            if (breakpoints_.contains(line)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(QStringLiteral("#e23b2f")));
                painter.drawEllipse(QPoint(12, top + height / 2), 5, 5);
            }
            if (line == execLine_) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(QStringLiteral("#ffb454")));
                const int cy = top + height / 2;
                QPolygon triangle;
                triangle << QPoint(20, cy - 5) << QPoint(20, cy + 5) << QPoint(27, cy);
                painter.drawPolygon(triangle);
            }
            painter.setPen((line == current || line == execLine_) ? QColor(QStringLiteral("#ffb454"))
                                                                   : QColor(QStringLiteral("#5c5c5c")));
            painter.drawText(0, top, width - 8, height, Qt::AlignRight | Qt::AlignVCenter, QString::number(line));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
    }
}

void CodeEdit::gutterClicked(QMouseEvent *event)
{
    const QTextCursor cursor = cursorForPosition(QPoint(0, int(event->position().y())));
    toggleBreakpoint(cursor.blockNumber() + 1);
}

void CodeEdit::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect rect = contentsRect();
    gutter_->setGeometry(QRect(rect.left(), rect.top(), gutterWidth(), rect.height()));
}

void CodeEdit::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && !(event->modifiers() & Qt::ControlModifier)) {
        insertNewline();
        return;
    }
    if (event->key() == Qt::Key_Backtab || (event->key() == Qt::Key_Tab && event->modifiers() & Qt::ShiftModifier)) {
        indentSelection(-1);
        return;
    }
    if (event->key() == Qt::Key_Tab && !(event->modifiers() & Qt::ControlModifier)) {
        indentSelection(1);
        return;
    }
    if (event->key() == Qt::Key_Backspace && event->modifiers() == Qt::NoModifier && unindentOnBackspace())
        return;
    QPlainTextEdit::keyPressEvent(event);
}

void CodeEdit::refreshGutter()
{
    setViewportMargins(gutterWidth(), 0, 0, 0);
    const QRect rect = contentsRect();
    gutter_->setGeometry(QRect(rect.left(), rect.top(), gutterWidth(), rect.height()));
    gutter_->update();
}

void CodeEdit::refreshSelections()
{
    QList<QTextEdit::ExtraSelection> extra;
    if (!needle_.isEmpty()) {
        QTextCursor found(document());
        int count = 0;
        while (count < 500) {
            found = document()->find(needle_, found, QTextDocument::FindCaseSensitively);
            if (found.isNull())
                break;
            QTextEdit::ExtraSelection hit;
            hit.format.setBackground(QColor(QStringLiteral("#3d3416")));
            hit.cursor = found;
            extra.append(hit);
            ++count;
        }
    }
    QTextEdit::ExtraSelection line;
    line.format.setBackground(QColor(QStringLiteral("#101010")));
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    extra.append(line);
    if (execLine_ > 0) {
        const QTextBlock block = document()->findBlockByNumber(execLine_ - 1);
        if (block.isValid()) {
            QTextEdit::ExtraSelection mark;
            mark.format.setBackground(QColor(QStringLiteral("#2a1c08")));
            mark.format.setProperty(QTextFormat::FullWidthSelection, true);
            mark.cursor = QTextCursor(block);
            extra.append(mark);
        }
    }
    setExtraSelections(extra);
}

void CodeEdit::insertNewline()
{
    QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();
    const QString indent = leadingWhitespace(line);
    const bool more = extraIndent(line.trimmed());
    cursor.insertText(QStringLiteral("\n") + indent + (more ? oneIndent() : QString()));
}

bool CodeEdit::unindentOnBackspace()
{
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection() || cursor.positionInBlock() == 0)
        return false;
    const QString text = cursor.block().text();
    const int col = cursor.positionInBlock();
    for (int i = 0; i < col; ++i) {
        if (!text.at(i).isSpace())
            return false;
    }
    int remove = 1;
    if (text.at(col - 1) == QLatin1Char(' ')) {
        remove = 0;
        int i = col;
        while (remove < tabWidth_ && i > 0 && text.at(i - 1) == QLatin1Char(' ')) {
            --i;
            ++remove;
        }
        if (remove == 0)
            remove = 1;
    }
    cursor.beginEditBlock();
    cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, remove);
    cursor.removeSelectedText();
    cursor.endEditBlock();
    return true;
}

QString CodeEdit::oneIndent() const
{
    if (indentTabs_)
        return QStringLiteral("\t");
    return QString(tabWidth_, QLatin1Char(' '));
}

QString CodeEdit::leadingWhitespace(const QString &line) const
{
    int i = 0;
    while (i < line.size() && (line.at(i) == QLatin1Char(' ') || line.at(i) == QLatin1Char('\t')))
        ++i;
    return line.left(i);
}
