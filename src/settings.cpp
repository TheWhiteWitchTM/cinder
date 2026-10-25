#include "settings.h"

#include <algorithm>

#include <QVariantMap>

Settings::Settings(QObject *parent)
    : QObject(parent)
{
}

QByteArray Settings::geometry() const
{
    return settings_.value(QStringLiteral("geometry")).toByteArray();
}

void Settings::setGeometry(const QByteArray &bytes)
{
    settings_.setValue(QStringLiteral("geometry"), bytes);
}

QByteArray Settings::splitter() const
{
    return settings_.value(QStringLiteral("splitter")).toByteArray();
}

void Settings::setSplitter(const QByteArray &bytes)
{
    settings_.setValue(QStringLiteral("splitter"), bytes);
}

QByteArray Settings::bottomSplitter() const
{
    return settings_.value(QStringLiteral("bottomSplitter")).toByteArray();
}

void Settings::setBottomSplitter(const QByteArray &bytes)
{
    settings_.setValue(QStringLiteral("bottomSplitter"), bytes);
}

QString Settings::folder() const
{
    return settings_.value(QStringLiteral("folder")).toString();
}

void Settings::setFolder(const QString &path)
{
    settings_.setValue(QStringLiteral("folder"), path);
}

QStringList Settings::recentFiles() const
{
    return settings_.value(QStringLiteral("recent")).toStringList();
}

void Settings::rememberFile(const QString &path)
{
    if (path.isEmpty())
        return;
    QStringList files = recentFiles();
    files.removeAll(path);
    files.prepend(path);
    while (files.size() > 10)
        files.removeLast();
    settings_.setValue(QStringLiteral("recent"), files);
}

int Settings::fontSize() const
{
    return settings_.value(QStringLiteral("fontSize"), 13).toInt();
}

void Settings::setFontSize(int points)
{
    settings_.setValue(QStringLiteral("fontSize"), points);
}

int Settings::tabWidth() const
{
    return settings_.value(QStringLiteral("tabWidth"), 4).toInt();
}

void Settings::setTabWidth(int width)
{
    settings_.setValue(QStringLiteral("tabWidth"), width);
}

bool Settings::wrap() const
{
    return settings_.value(QStringLiteral("wrap"), false).toBool();
}

void Settings::setWrap(bool on)
{
    settings_.setValue(QStringLiteral("wrap"), on);
}

bool Settings::shellOnly() const
{
    return settings_.value(QStringLiteral("shellOnly"), false).toBool();
}

void Settings::setShellOnly(bool on)
{
    settings_.setValue(QStringLiteral("shellOnly"), on);
}

QString Settings::interpreter() const
{
    return settings_.value(QStringLiteral("interpreter"), QStringLiteral("auto")).toString();
}

void Settings::setInterpreter(const QString &mode)
{
    settings_.setValue(QStringLiteral("interpreter"), mode);
}

QVariantMap Settings::breakpointMap() const
{
    return settings_.value(QStringLiteral("breakpoints")).toMap();
}

QSet<int> Settings::breakpoints(const QString &path) const
{
    QSet<int> lines;
    const QStringList raw = breakpointMap().value(path).toStringList();
    for (const QString &item : raw) {
        bool ok = false;
        const int line = item.toInt(&ok);
        if (ok && line > 0)
            lines.insert(line);
    }
    return lines;
}

void Settings::setBreakpoints(const QString &path, const QSet<int> &lines)
{
    QVariantMap map = breakpointMap();
    if (lines.isEmpty()) {
        map.remove(path);
    } else {
        QStringList raw;
        QList<int> ordered = lines.values();
        std::sort(ordered.begin(), ordered.end());
        for (int line : ordered)
            raw.append(QString::number(line));
        map.insert(path, raw);
    }
    settings_.setValue(QStringLiteral("breakpoints"), map);
}

QVariantMap Settings::argumentMap() const
{
    return settings_.value(QStringLiteral("arguments")).toMap();
}

QString Settings::arguments(const QString &path) const
{
    return argumentMap().value(path).toString();
}

void Settings::setArguments(const QString &path, const QString &args)
{
    if (path.isEmpty())
        return;
    QVariantMap map = argumentMap();
    if (args.isEmpty())
        map.remove(path);
    else
        map.insert(path, args);
    settings_.setValue(QStringLiteral("arguments"), map);
}
