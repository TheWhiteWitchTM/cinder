#pragma once

#include "common.h"

#include <QList>

QList<Problem> builtinLint(const QString &path, const QString &text, Dialect dialect);
QList<Problem> parseShellcheck(const QByteArray &json, const QString &path);
QString bashismMessage(const QString &text);
