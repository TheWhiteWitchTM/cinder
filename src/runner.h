#pragma once

#include "common.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

enum class RunMode {
    Run,
    Trace
};

struct RunSpec {
    QString program;
    QStringList prefix;
    QString script;
    QStringList args;
    QString workDir;
    RunMode mode = RunMode::Run;
    Dialect dialect = Dialect::Bash;
    QString selection;
};

class Runner : public QObject {
    Q_OBJECT
public:
    explicit Runner(QObject *parent = nullptr);
    ~Runner() override;

    bool running() const;
    void start(const RunSpec &spec);
    void stop();
    void writeInput(const QString &text);

signals:
    void output(const QString &text, int channel);
    void traceLine(const QString &file, int line);
    void started(const QString &description);
    void finished(int code);

private:
    void readOut();
    void readErr();
    void flush();
    void takeLine(QByteArray line, bool error);
    void shutdown();

    QProcess process_;
    QByteArray outBuf_;
    QByteArray errBuf_;
    bool stopRequested_ = false;
    QString traceFile_;
    std::unique_ptr<QTemporaryDir> temp_;
};
