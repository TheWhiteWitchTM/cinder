#include "debugger.h"

#include "console.h"

#include <QFile>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>

#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

const char *kPrelude = R"PRE(
cinder_collect() {
  local _cname _cout=""
  while IFS= read -r _cname; do
    case "$_cname" in
      _c*|CINDER_*|BASH_*|FUNCNAME|DIRSTACK|GROUPS|PIPESTATUS|SHELLOPTS|BASHOPTS|EPOCH*|PPID|BASHPID|_|COMP_*|SRANDOM|LINENO|OLDPWD|PWD|SHLVL|UID|EUID) continue ;;
    esac
    if [[ -n "${CINDER_ORIG[$_cname]+x}" ]]; then
      continue
    fi
    _cout+="$(declare -p "$_cname" 2>/dev/null || true)"$'\n'
  done < <(compgen -v || true)
  printf '%s' "$_cout"
}

cinder_on_debug() {
  local _csrc="${BASH_SOURCE[1]:-}"
  local _cline="${BASH_LINENO[0]:-0}"
  local _cdepth=${#BASH_SOURCE[@]}
  [[ -z "$_csrc" || "$_csrc" == "$CINDER_PRELUDE" ]] && return 0
  [[ "$_cline" -le 0 ]] && return 0

  local _cbps=""
  _cbps="$(cat "$CINDER_BPS" 2>/dev/null || true)"
  if [[ "$_cbps" != "$CINDER_BP_TEXT" ]]; then
    CINDER_BP_TEXT="$_cbps"
    unset CINDER_BP
    declare -gA CINDER_BP
    local _cent=""
    while IFS= read -r _cent || [[ -n "${_cent:-}" ]]; do
      [[ -n "${_cent:-}" ]] && CINDER_BP["$_cent"]=1
    done <<< "$_cbps"
  fi

  if [[ "${CINDER_NEXT:-0}" == 1 ]]; then
    if [[ "$_cdepth" -gt "${CINDER_NEXT_DEPTH:-999}" ]]; then
      return 0
    fi
    CINDER_NEXT=0
    CINDER_STEP=1
  fi

  local _ckey="$_csrc:$_cline"
  local _cshould=0
  if [[ "${CINDER_STEP:-0}" == 1 ]]; then
    _cshould=1
  elif [[ -n "${CINDER_BP[$_ckey]:-}" ]]; then
    _cshould=1
  elif [[ -f "$CINDER_PAUSE" ]]; then
    _cshould=1
  fi
  [[ "$_cshould" == 1 ]] || return 0
  if [[ -f "$CINDER_PAUSE" ]]; then
    rm -f "$CINDER_PAUSE" || true
  fi

  if [[ -z "${CINDER_BP[$_ckey]:-}" ]]; then
    local _ctext=""
    if [[ -r "$_csrc" ]]; then
      local -a _clines=()
      mapfile -s "$((_cline - 1))" -n 1 _clines < "$_csrc" || true
      _ctext="${_clines[0]-}"
      _ctext="${_ctext%$'\r'}"
    fi
    local _ctrim="${_ctext#"${_ctext%%[![:space:]]*}"}"
    if [[ -z "$_ctrim" || "$_ctrim" == \#* ]]; then
      return 0
    fi
    if [[ "$_ctrim" =~ ^function[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\{?[[:space:]]*$ ]] \
       || [[ "$_ctrim" =~ ^(function[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\(\)[[:space:]]*\{?[[:space:]]*$ ]]; then
      return 0
    fi
  fi

  local _cblob=""
  _cblob="$(cinder_collect || true)"
  printf 'hit\t%s\t%s\t%s\n' "$_csrc" "$_cline" "$(printf '%s' "$_cblob" | base64 -w 0)" > "$CINDER_EVENTS" || true
  local _ccmd=""
  IFS= read -r _ccmd < "$CINDER_CMDS" || exit 130
  case "$_ccmd" in
    step) CINDER_STEP=1; CINDER_NEXT=0 ;;
    cont|continue) CINDER_STEP=0; CINDER_NEXT=0 ;;
    next)
      CINDER_STEP=0
      CINDER_NEXT=1
      CINDER_NEXT_DEPTH="$_cdepth"
      ;;
    quit) exit 130 ;;
    *) CINDER_STEP=1; CINDER_NEXT=0 ;;
  esac
  return 0
}

declare -gA CINDER_ORIG
while IFS= read -r _cname; do
  CINDER_ORIG["$_cname"]=1
done < <(compgen -v)
unset _cname
declare -gA CINDER_BP
declare -g CINDER_BP_TEXT=""
declare -g CINDER_STEP=__CINDER_STEP__
declare -g CINDER_NEXT=0
declare -g CINDER_NEXT_DEPTH=0
trap '[[ "${CINDER_IN:-0}" == 1 ]] && return 0; CINDER_IN=1; cinder_on_debug; CINDER_IN=0' DEBUG
set -T
)PRE";

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

QList<Var> parseVars(const QByteArray &blob)
{
    QList<Var> vars;
    const QString text = QString::fromUtf8(blob);
    QString current;
    const auto flush = [&vars, &current] {
        if (current.isEmpty())
            return;
        const int eq = current.indexOf(QLatin1Char('='));
        const QString head = eq < 0 ? current : current.left(eq);
        QString value = eq < 0 ? QString() : current.mid(eq + 1);
        const QString name = head.section(QLatin1Char(' '), -1).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
            value = value.mid(1, value.size() - 2);
            value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
            value.replace(QStringLiteral("\\\""), QStringLiteral("\""));
            value.replace(QStringLiteral("\\$"), QStringLiteral("$"));
            value.replace(QStringLiteral("\\`"), QStringLiteral("`"));
        }
        if (!name.isEmpty() && (name.at(0).isLetter() || name.at(0) == QLatin1Char('_')))
            vars.append(Var{name, value});
        current.clear();
    };
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("declare ")) || line.startsWith(QLatin1String("typeset "))) {
            flush();
            current = line;
        } else if (!current.isEmpty()) {
            current += QLatin1Char('\n') + line;
        }
    }
    flush();
    std::sort(vars.begin(), vars.end(), [](const Var &a, const Var &b) { return a.name < b.name; });
    return vars;
}

} // namespace

Debugger::Debugger(QObject *parent)
    : QObject(parent)
{
    process_.setProcessChannelMode(QProcess::SeparateChannels);
#if defined(Q_OS_UNIX)
    process_.setUnixProcessParameters(QProcess::UnixProcessFlag::CreateNewSession);
#endif
    connect(&process_, &QProcess::readyReadStandardOutput, this, &Debugger::readOut);
    connect(&process_, &QProcess::readyReadStandardError, this, &Debugger::readErr);
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (!errBuf_.isEmpty()) {
            emit output(QString::fromUtf8(errBuf_), kStreamErr);
            errBuf_.clear();
        }
        paused_ = false;
        const int reported = stopRequested_ ? -1 : (status == QProcess::NormalExit ? code : -1);
        stopRequested_ = false;
        cleanup();
        emit finished(reported);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            emit failed(QStringLiteral("Could not start bash."));
            cleanup();
            emit finished(-1);
        }
    });
}

Debugger::~Debugger()
{
    stopRequested_ = true;
    if (process_.state() != QProcess::NotRunning) {
        signalGroup(process_, SIGKILL);
        process_.waitForFinished(500);
    }
    cleanup();
}

bool Debugger::running() const
{
    return process_.state() != QProcess::NotRunning;
}

void Debugger::setBreakpoints(const QList<QPair<QString, int>> &breakpoints)
{
    breakpoints_ = breakpoints;
    writeBreakpoints();
}

void Debugger::writeBreakpoints()
{
    if (breakpointPath_.isEmpty())
        return;
    QFile file(breakpointPath_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    for (const auto &item : breakpoints_) {
        file.write(item.first.toUtf8());
        file.write(":");
        file.write(QByteArray::number(item.second));
        file.write("\n");
    }
}

void Debugger::start(const DebugSpec &spec)
{
    if (running()) {
        stopRequested_ = true;
        signalGroup(process_, SIGKILL);
        process_.waitForFinished(800);
    }
    cleanup();
    paused_ = false;
    stopRequested_ = false;
    outBuf_.clear();
    errBuf_.clear();
    eventBuf_.clear();
    breakpoints_ = spec.breakpoints;

    temp_ = std::make_unique<QTemporaryDir>();
    if (!temp_->isValid()) {
        emit failed(QStringLiteral("Could not create a debug directory."));
        emit finished(-1);
        return;
    }
    const QString preludePath = temp_->filePath(QStringLiteral("prelude.sh"));
    eventsPath_ = temp_->filePath(QStringLiteral("events"));
    commandsPath_ = temp_->filePath(QStringLiteral("cmds"));
    breakpointPath_ = temp_->filePath(QStringLiteral("breakpoints"));
    pausePath_ = temp_->filePath(QStringLiteral("pause"));
    if (::mkfifo(eventsPath_.toLocal8Bit().constData(), 0600) != 0
        || ::mkfifo(commandsPath_.toLocal8Bit().constData(), 0600) != 0) {
        emit failed(QStringLiteral("Could not create the debug pipes."));
        cleanup();
        emit finished(-1);
        return;
    }
    QFile prelude(preludePath);
    if (!prelude.open(QIODevice::WriteOnly)) {
        emit failed(QStringLiteral("Could not write the debug prelude."));
        cleanup();
        emit finished(-1);
        return;
    }
    QString body = QString::fromUtf8(kPrelude);
    body.replace(QStringLiteral("__CINDER_STEP__"), spec.breakOnEntry ? QStringLiteral("1") : QStringLiteral("0"));
    prelude.write(body.toUtf8());
    prelude.close();
    writeBreakpoints();

    eventFd_ = ::open(eventsPath_.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    commandFd_ = ::open(commandsPath_.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
    if (eventFd_ < 0 || commandFd_ < 0) {
        emit failed(QStringLiteral("Could not open the debug pipes."));
        cleanup();
        emit finished(-1);
        return;
    }
    ::fcntl(eventFd_, F_SETFD, FD_CLOEXEC);
    ::fcntl(commandFd_, F_SETFD, FD_CLOEXEC);
    notifier_ = new QSocketNotifier(eventFd_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] { readEvents(); });

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("BASH_ENV"), preludePath);
    env.insert(QStringLiteral("CINDER_PRELUDE"), preludePath);
    env.insert(QStringLiteral("CINDER_EVENTS"), eventsPath_);
    env.insert(QStringLiteral("CINDER_CMDS"), commandsPath_);
    env.insert(QStringLiteral("CINDER_BPS"), breakpointPath_);
    env.insert(QStringLiteral("CINDER_PAUSE"), pausePath_);
    process_.setProcessEnvironment(env);
    if (!spec.workDir.isEmpty())
        process_.setWorkingDirectory(spec.workDir);
    const QString bash = findProgram(QStringLiteral("bash"));
    process_.setProgram(bash);
    QStringList args;
    args << spec.script << spec.args;
    process_.setArguments(args);
    process_.start();
    emit started(bash + QLatin1Char(' ') + args.join(QLatin1Char(' ')));
}

void Debugger::stop()
{
    if (!running())
        return;
    stopRequested_ = true;
    paused_ = false;
    send("quit");
    signalGroup(process_, SIGTERM);
    QPointer<Debugger> self(this);
    QTimer::singleShot(500, this, [self] {
        if (!self || self->process_.state() == QProcess::NotRunning)
            return;
        signalGroup(self->process_, SIGKILL);
    });
}

void Debugger::stepInto()
{
    if (!paused_)
        return;
    paused_ = false;
    send("step");
}

void Debugger::stepOver()
{
    if (!paused_)
        return;
    paused_ = false;
    send("next");
}

void Debugger::cont()
{
    if (!paused_)
        return;
    paused_ = false;
    send("cont");
}

void Debugger::requestPause()
{
    if (!running() || pausePath_.isEmpty())
        return;
    QFile file(pausePath_);
    if (file.open(QIODevice::WriteOnly))
        file.write("1\n");
}

void Debugger::writeInput(const QString &text)
{
    if (!running())
        return;
    QString line = text;
    if (!line.endsWith(QLatin1Char('\n')))
        line += QLatin1Char('\n');
    process_.write(line.toUtf8());
}

void Debugger::send(const QByteArray &command)
{
    if (commandFd_ < 0)
        return;
    QByteArray line = command;
    if (!line.endsWith('\n'))
        line.append('\n');
    const ssize_t wrote = ::write(commandFd_, line.constData(), size_t(line.size()));
    Q_UNUSED(wrote);
}

void Debugger::readEvents()
{
    if (eventFd_ < 0)
        return;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(eventFd_, buf, sizeof buf);
        if (n > 0)
            eventBuf_.append(buf, int(n));
        else
            break;
    }
    int nl = eventBuf_.indexOf('\n');
    while (nl >= 0) {
        const QByteArray line = eventBuf_.left(nl);
        eventBuf_.remove(0, nl + 1);
        const QList<QByteArray> parts = line.split('\t');
        if (parts.size() >= 3 && parts.at(0) == "hit") {
            const QString file = QString::fromUtf8(parts.at(1));
            const int number = parts.at(2).toInt();
            QByteArray blob;
            if (parts.size() >= 4)
                blob = QByteArray::fromBase64(parts.at(3));
            paused_ = true;
            emit pausedAt(file, number, parseVars(blob));
        }
        nl = eventBuf_.indexOf('\n');
    }
}

void Debugger::readOut()
{
    const QByteArray data = process_.readAllStandardOutput();
    if (!data.isEmpty())
        emit output(QString::fromUtf8(data), kStreamOut);
}

void Debugger::readErr()
{
    const QByteArray data = process_.readAllStandardError();
    if (!data.isEmpty())
        emit output(QString::fromUtf8(data), kStreamErr);
}

void Debugger::shutdown()
{
    stop();
}

void Debugger::cleanup()
{
    if (notifier_) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    if (eventFd_ >= 0) {
        ::close(eventFd_);
        eventFd_ = -1;
    }
    if (commandFd_ >= 0) {
        ::close(commandFd_);
        commandFd_ = -1;
    }
    eventsPath_.clear();
    commandsPath_.clear();
    breakpointPath_.clear();
    pausePath_.clear();
    temp_.reset();
}
