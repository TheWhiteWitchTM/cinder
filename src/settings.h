#pragma once

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QStringList>

class Settings : public QObject {
    Q_OBJECT
public:
    explicit Settings(QObject *parent = nullptr);

    QByteArray geometry() const;
    void setGeometry(const QByteArray &bytes);
    QByteArray splitter() const;
    void setSplitter(const QByteArray &bytes);
    QByteArray bottomSplitter() const;
    void setBottomSplitter(const QByteArray &bytes);

    QString folder() const;
    void setFolder(const QString &path);
    QStringList recentFiles() const;
    void rememberFile(const QString &path);

    int fontSize() const;
    void setFontSize(int points);
    int tabWidth() const;
    void setTabWidth(int width);
    bool wrap() const;
    void setWrap(bool on);
    bool shellOnly() const;
    void setShellOnly(bool on);
    QString interpreter() const;
    void setInterpreter(const QString &mode);

    QSet<int> breakpoints(const QString &path) const;
    void setBreakpoints(const QString &path, const QSet<int> &lines);
    QString arguments(const QString &path) const;
    void setArguments(const QString &path, const QString &args);

private:
    QVariantMap breakpointMap() const;
    QVariantMap argumentMap() const;

    QSettings settings_;
};
