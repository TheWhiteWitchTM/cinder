#include "runner.h"

#include "console.h"

#include <QFile>
#include <QPointer>
#include <QTimer>

#include <csignal>
#include <unistd.h>

namespace {

void signalGroup(QProcess &process, int sig)
{
    const qint64 pid = process.processId();
    if (pid > 0)
        ::kill(static_cast<pid_t>(-pid), sig);
    else if (sig == SIGKILL)
        process.kill();
    else
        process.terminate();
}

} // namespace

Runner::Runner(QObject *parent)
    : QObject(parent)
{
    process_.setProcessChannelMode(QProcess::SeparateChannels);
#if defined(Q_OS_UNIX)
    process_.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#endif
    connect(&process_, &QProcess::readyReadStandardOutput, this, &Runner::readOut);
    connect(&process_, &QProcess::readyReadStandardError, this, &Runner::readErr);
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        flush();
        const int reported = stopRequested_ ? -1 : (status == QProcess::NormalExit ? code : -1);
        stopRequested_ = false;
        temp_.reset();
        emit finished(reported);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            emit output(QStringLiteral("Could not start the interpreter.\n"), kStreamNote);
            emit finished(-1);
        }
    });
}

Runner::~Runner()
{
    stopRequested_ = true;
    if (process_.state() != QProcess::NotRunning) {
        signalGroup(process_, SIGKILL);
        process_.waitForFinished(500);
    }
}

bool Runner::running() const
{
    return process_.state() != QProcess::NotRunning;
}

void Runner::start(const RunSpec &spec)
{
    if (running())
        stop();
    stopRequested_ = false;
    outBuf_.clear();
    errBuf_.clear();
    temp_.reset();

    QString script = spec.script;
    QStringList args = spec.prefix;
    if (!spec.selection.isEmpty()) {
        temp_ = std::make_unique<QTemporaryDir>();
        if (!temp_->isValid()) {
            emit output(QStringLiteral("Could not create a temporary script.\n"), kStreamNote);
            emit finished(-1);
            return;
        }
        script = temp_->filePath(QStringLiteral("selection.sh"));
        QFile file(script);
        if (!file.open(QIODevice::WriteOnly)) {
            emit output(QStringLiteral("Could not write the selection.\n"), kStreamNote);
            emit finished(-1);
            return;
        }
        QString body = spec.selection;
        if (!body.startsWith(QLatin1String("#!"))) {
            body.prepend(spec.dialect == Dialect::Bash ? QStringLiteral("#!/usr/bin/env bash\n")
                                                       : QStringLiteral("#!/bin/sh\n"));
        }
        if (!body.endsWith(QLatin1Char('\n')))
            body += QLatin1Char('\n');
        file.write(body.toUtf8());
        file.close();
    }

    traceFile_ = script;
    args.append(script);
    args.append(spec.args);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (spec.mode == RunMode::Trace)
        env.insert(QStringLiteral("PS4"), QStringLiteral("CINDER\x1e${BASH_SOURCE}\x1e${LINENO}\x1e"));
    process_.setProcessEnvironment(env);
    if (!spec.workDir.isEmpty())
        process_.setWorkingDirectory(spec.workDir);
    process_.setProgram(spec.program);
    process_.setArguments(args);

    QStringList shown;
    shown << spec.program << args;
    process_.start();
    emit started(shown.join(QLatin1Char(' ')));
}

void Runner::stop()
{
    if (!running())
        return;
    stopRequested_ = true;
    signalGroup(process_, SIGTERM);
    QPointer<Runner> self(this);
    QTimer::singleShot(500, this, [self] {
        if (!self || self->process_.state() == QProcess::NotRunning)
            return;
        signalGroup(self->process_, SIGKILL);
    });
}

void Runner::writeInput(const QString &text)
{
    if (!running())
        return;
    QString line = text;
    if (!line.endsWith(QLatin1Char('\n')))
        line += QLatin1Char('\n');
    process_.write(line.toUtf8());
}

void Runner::readOut()
{
    const QByteArray data = process_.readAllStandardOutput();
    if (!data.isEmpty())
        emit output(QString::fromUtf8(data), kStreamOut);
}

void Runner::readErr()
{
    errBuf_ += process_.readAllStandardError();
    int nl = errBuf_.indexOf('\n');
    while (nl >= 0) {
        QByteArray line = errBuf_.left(nl);
        errBuf_.remove(0, nl + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        takeLine(line, true);
        nl = errBuf_.indexOf('\n');
    }
}

void Runner::flush()
{
    if (!errBuf_.isEmpty()) {
        takeLine(errBuf_, true);
        errBuf_.clear();
    }
}

void Runner::takeLine(QByteArray line, bool error)
{
    if (error && line.startsWith("CINDER\x1e")) {
        const QList<QByteArray> parts = line.split('\x1e');
        if (parts.size() >= 4) {
            const QString file = QString::fromUtf8(parts.at(1));
            bool ok = false;
            const int number = parts.at(2).toInt(&ok);
            QByteArray command;
            for (int i = 3; i < parts.size(); ++i) {
                if (i > 3)
                    command += '\x1e';
                command += parts.at(i);
            }
            if (ok && number > 0)
                emit traceLine(file.isEmpty() ? traceFile_ : file, number);
            emit output(QStringLiteral("+ ") + QString::fromUtf8(command) + QLatin1Char('\n'), kStreamTrace);
            return;
        }
    }
    emit output(QString::fromUtf8(line) + QLatin1Char('\n'), error ? kStreamErr : kStreamOut);
}

void Runner::shutdown()
{
    stop();
}
