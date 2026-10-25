#include "console.h"

#include <QFont>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QTextCursor>
#include <QVBoxLayout>

namespace {

QFont mono(int points)
{
    QFont font(QStringLiteral("Fira Mono"), points);
    if (!QFontInfo(font).exactMatch())
        font = QFont(QStringLiteral("DejaVu Sans Mono"), points);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

QColor ansiColor(int code, const QColor &fallback)
{
    switch (code) {
    case 30: return QColor(QStringLiteral("#6a6a6a"));
    case 31: return QColor(QStringLiteral("#ff7b72"));
    case 32: return QColor(QStringLiteral("#b7e1a8"));
    case 33: return QColor(QStringLiteral("#ffd18a"));
    case 34: return QColor(QStringLiteral("#7eb6ff"));
    case 35: return QColor(QStringLiteral("#d2a8ff"));
    case 36: return QColor(QStringLiteral("#8fd4ff"));
    case 37: return QColor(QStringLiteral("#e8e6e3"));
    case 90: return QColor(QStringLiteral("#8a8a8a"));
    case 91: return QColor(QStringLiteral("#ff9a94"));
    case 92: return QColor(QStringLiteral("#d4f5c8"));
    case 93: return QColor(QStringLiteral("#ffe1b0"));
    case 94: return QColor(QStringLiteral("#a8d0ff"));
    case 95: return QColor(QStringLiteral("#e2c4ff"));
    case 96: return QColor(QStringLiteral("#b5e8ff"));
    case 97: return QColor(QStringLiteral("#ffffff"));
    default: return fallback;
    }
}

} // namespace

Console::Console(QWidget *parent)
    : QWidget(parent)
{
    view_ = new QPlainTextEdit(this);
    view_->setObjectName(QStringLiteral("consoleView"));
    view_->setReadOnly(true);
    view_->setLineWrapMode(QPlainTextEdit::NoWrap);
    view_->setFrameStyle(QFrame::NoFrame);
    view_->setFont(mono(12));
    view_->setMaximumBlockCount(8000);
    QPalette pal = view_->palette();
    pal.setColor(QPalette::Base, QColor(QStringLiteral("#000000")));
    pal.setColor(QPalette::Text, QColor(QStringLiteral("#e8e6e3")));
    view_->setPalette(pal);

    input_ = new QLineEdit(this);
    input_->setPlaceholderText(QStringLiteral("stdin"));
    input_->setEnabled(false);
    connect(input_, &QLineEdit::returnPressed, this, [this] {
        const QString line = input_->text();
        input_->clear();
        emit submitInput(line);
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(view_, 1);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(8, 6, 8, 6);
    bar->addWidget(input_);
    layout->addLayout(bar);
    format_.setForeground(base_);
}

void Console::setInputEnabled(bool on)
{
    input_->setEnabled(on);
    if (on)
        input_->setFocus();
}

void Console::clearAll()
{
    view_->clear();
    pending_.clear();
    plain_.clear();
    inEscape_ = false;
    escape_.clear();
    format_ = QTextCharFormat();
    format_.setForeground(base_);
}

void Console::appendStream(const QString &text, int channel)
{
    QColor base(QStringLiteral("#e8e6e3"));
    if (channel == kStreamErr)
        base = QColor(QStringLiteral("#ffb4ae"));
    else if (channel == kStreamTrace)
        base = QColor(QStringLiteral("#9a9a9a"));
    else if (channel == kStreamNote)
        base = QColor(QStringLiteral("#8a8a8a"));
    if (channel == kStreamNote || channel == kStreamTrace) {
        QTextCursor cursor(view_->document());
        cursor.movePosition(QTextCursor::End);
        QTextCharFormat format;
        format.setForeground(base);
        if (channel == kStreamNote)
            format.setFontItalic(true);
        cursor.insertText(text, format);
        view_->setTextCursor(cursor);
        view_->ensureCursorVisible();
        return;
    }
    appendAnsi(text, base);
}

void Console::flushPlain()
{
    if (plain_.isEmpty())
        return;
    QTextCursor cursor(view_->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(plain_, format_);
    plain_.clear();
    view_->setTextCursor(cursor);
    view_->ensureCursorVisible();
}

void Console::applySgr(const QString &params)
{
    const QStringList parts = params.split(QLatin1Char(';'));
    if (parts.isEmpty() || (parts.size() == 1 && parts.at(0).isEmpty())) {
        format_ = QTextCharFormat();
        format_.setForeground(base_);
        return;
    }
    for (const QString &part : parts) {
        bool ok = false;
        const int code = part.isEmpty() ? 0 : part.toInt(&ok);
        if (!ok && !part.isEmpty())
            continue;
        if (code == 0) {
            format_ = QTextCharFormat();
            format_.setForeground(base_);
        } else if (code == 1) {
            format_.setFontWeight(QFont::DemiBold);
        } else if (code == 22) {
            format_.setFontWeight(QFont::Normal);
        } else if (code == 39) {
            format_.setForeground(base_);
        } else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
            format_.setForeground(ansiColor(code, base_));
        }
    }
}

void Console::appendAnsi(const QString &text, const QColor &base)
{
    base_ = base;
    if (!format_.foreground().color().isValid())
        format_.setForeground(base_);
    const QString incoming = pending_ + text;
    pending_.clear();
    for (int i = 0; i < incoming.size(); ++i) {
        const QChar c = incoming.at(i);
        if (inEscape_) {
            escape_.append(c);
            if (escape_.size() > 32) {
                inEscape_ = false;
                escape_.clear();
                continue;
            }
            if (c.isLetter()) {
                if (escape_.startsWith(QLatin1String("\033[")) && c == QLatin1Char('m'))
                    applySgr(escape_.mid(2, escape_.size() - 3));
                inEscape_ = false;
                escape_.clear();
            }
            continue;
        }
        if (c == QChar(0x1b)) {
            flushPlain();
            inEscape_ = true;
            escape_ = c;
            if (i + 1 >= incoming.size()) {
                pending_ = escape_;
                inEscape_ = false;
                escape_.clear();
            }
            continue;
        }
        plain_.append(c);
    }
    if (inEscape_) {
        pending_ = escape_;
        inEscape_ = false;
        escape_.clear();
    }
    flushPlain();
}
