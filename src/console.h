#pragma once

#include <QColor>
#include <QPlainTextEdit>
#include <QWidget>

constexpr int kStreamOut = 0;
constexpr int kStreamErr = 1;
constexpr int kStreamTrace = 2;
constexpr int kStreamNote = 3;

class Console : public QWidget {
    Q_OBJECT
public:
    explicit Console(QWidget *parent = nullptr);
    void appendStream(const QString &text, int channel);
    void clearAll();
    void setInputEnabled(bool on);

signals:
    void submitInput(const QString &line);

private:
    void appendAnsi(const QString &text, const QColor &base);
    void flushPlain();
    void applySgr(const QString &params);

    QPlainTextEdit *view_ = nullptr;
    class QLineEdit *input_ = nullptr;
    QString pending_;
    QString plain_;
    QTextCharFormat format_;
    QColor base_ = QColor(QStringLiteral("#e8e6e3"));
    bool inEscape_ = false;
    QString escape_;
};
