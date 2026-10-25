#pragma once

#include "common.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

struct DebugSpec {
    QString script;
    QStringList args;
    QString workDir;
    QList<QPair<QString, int>> breakpoints;
    bool breakOnEntry = false;
};

class Debugger : public QObject {
    Q_OBJECT
public:
    explicit Debugger(QObject *parent = nullptr);
    ~Debugger() override;

    bool running() const;
    bool paused() const { return paused_; }
    void start(const DebugSpec &spec);
    void stop();
    void stepInto();
    void stepOver();
    void cont();
    void requestPause();
    void writeInput(const QString &text);
    void setBreakpoints(const QList<QPair<QString, int>> &breakpoints);

signals:
    void output(const QString &text, int channel);
    void pausedAt(const QString &file, int line, const QList<Var> &vars);
    void started(const QString &description);
    void finished(int code);
    void failed(const QString &message);

private:
    void send(const QByteArray &command);
    void readEvents();
    void readOut();
    void readErr();
    void writeBreakpoints();
    void shutdown();
    void cleanup();

    QProcess process_;
    QByteArray outBuf_;
    QByteArray errBuf_;
    QByteArray eventBuf_;
    std::unique_ptr<QTemporaryDir> temp_;
    class QSocketNotifier *notifier_ = nullptr;
    int eventFd_ = -1;
    int commandFd_ = -1;
    bool paused_ = false;
    bool stopRequested_ = false;
    QString eventsPath_;
    QString commandsPath_;
    QString breakpointPath_;
    QString pausePath_;
    QList<QPair<QString, int>> breakpoints_;
};
