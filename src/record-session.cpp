/** @fileoverview The owned gpu-screen-recorder session. */
#include "record-session.hpp"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

/// How often, and for how long, to look for the encoder's control socket
/// before giving up on it having started.
constexpr int kReadyPollMs = 50;
constexpr int kReadyAttempts = 240; // 12 s
/// A stop reply only arrives once the file is written, which for a long
/// recording is not instant.
constexpr int kStopTimeoutMs = 20000;
constexpr int kCommandTimeoutMs = 5000;
/// After the encoder says it saved the file it should exit almost at once.
/// Long enough not to race a slow unmount, short enough that the indicator
/// does not sit on "Saving…" forever.
constexpr int kExitWatchdogMs = 10000;
/// Between asking the encoder to terminate and killing it outright.
constexpr int kTerminateGraceMs = 2000;
/// Enough context to name a failure; not a log of the whole session.
constexpr qsizetype kDiagnosticTailBytes = 2048;

/**
 * One request on gpu-screen-recorder's control socket, over a raw AF_UNIX
 * connection rather than QLocalSocket: Qt Network would become a shared
 * library the screenshot binary loads at startup for a channel only the
 * recorder ever opens.
 *
 * Non-blocking throughout, because the reply to `stop` waits for the encoder
 * to finish writing the file and the indicator has to keep painting.
 */
class GsrRequest final : public QObject {
public:
  using Reply = std::function<void(bool ok, const QString &payload)>;

  GsrRequest(const QString &socketPath, int id, QByteArray request,
             int timeoutMs, Reply reply, QObject *parent)
      : QObject(parent), request_(std::move(request)), reply_(std::move(reply)),
        id_(id) {
    auto *timeout = new QTimer(this);
    timeout->setSingleShot(true);
    timeout->setInterval(timeoutMs);
    connect(timeout, &QTimer::timeout, this,
            [this] { answer(false, QStringLiteral("timed out")); });
    timeout->start();

    const QByteArray path = QFile::encodeName(socketPath);
    struct sockaddr_un address{};
    if (static_cast<size_t>(path.size()) >= sizeof(address.sun_path)) {
      finishLater(false, QStringLiteral("control socket path is too long"));
      return;
    }
    fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
      finishLater(false, QString::fromLocal8Bit(std::strerror(errno)));
      return;
    }
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.constData(),
                static_cast<size_t>(path.size()));
    const int connected =
        ::connect(fd_, reinterpret_cast<struct sockaddr *>(&address),
                  sizeof(address));
    if (connected != 0 && errno != EINPROGRESS) {
      finishLater(false, QString::fromLocal8Bit(std::strerror(errno)));
      return;
    }
    writeNotifier_ = new QSocketNotifier(fd_, QSocketNotifier::Write, this);
    connect(writeNotifier_, &QSocketNotifier::activated, this,
            &GsrRequest::send);
    if (connected == 0)
      send();
  }

  ~GsrRequest() override {
    if (fd_ >= 0)
      ::close(fd_);
  }

private:
  void send() {
    if (answered_)
      return;
    if (!connected_) {
      int socketError = 0;
      socklen_t length = sizeof(socketError);
      if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 ||
          socketError != 0) {
        writeNotifier_->setEnabled(false);
        answer(false, QString::fromLocal8Bit(std::strerror(
                          socketError != 0 ? socketError : errno)));
        return;
      }
      connected_ = true;
    }
    // Short writes are legal on a nonblocking stream socket, so keep an
    // offset and come back when it is writable again rather than calling a
    // partly sent request a failure.
    while (sent_ < request_.size()) {
      const ssize_t wrote =
          ::send(fd_, request_.constData() + sent_,
                 static_cast<size_t>(request_.size() - sent_), MSG_NOSIGNAL);
      if (wrote > 0) {
        sent_ += wrote;
        continue;
      }
      if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return; // Still armed; the notifier calls back.
      if (wrote < 0 && errno == EINTR)
        continue;
      writeNotifier_->setEnabled(false);
      answer(false, QStringLiteral("could not send the request"));
      return;
    }
    writeNotifier_->setEnabled(false);
    if (readNotifier_)
      return;
    readNotifier_ = new QSocketNotifier(fd_, QSocketNotifier::Read, this);
    connect(readNotifier_, &QSocketNotifier::activated, this,
            &GsrRequest::receive);
  }

  void receive() {
    char buffer[1024];
    const ssize_t got = ::read(fd_, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return;
      answer(false, QString::fromLocal8Bit(std::strerror(errno)));
      return;
    }
    if (got == 0) {
      answer(false, QStringLiteral("the encoder closed the connection"));
      return;
    }
    response_.append(buffer, got);
    if (response_.size() > kMaxReplyBytes) {
      answer(false, QStringLiteral("the reply was implausibly large"));
      return;
    }
    qsizetype end = response_.indexOf('\n');
    while (end >= 0) {
      const QByteArray line = response_.left(end);
      response_.remove(0, end + 1);
      const QJsonObject object = QJsonDocument::fromJson(line).object();
      if (object.value(QStringLiteral("id")).toInt() == id_) {
        answer(object.value(QStringLiteral("result")).toString() ==
                   QStringLiteral("ok"),
               object.value(QStringLiteral("data")).toString());
        return;
      }
      end = response_.indexOf('\n');
    }
  }

  void finishLater(bool ok, const QString &payload) {
    QTimer::singleShot(0, this, [this, ok, payload] { answer(ok, payload); });
  }

  void answer(bool ok, const QString &payload) {
    if (answered_)
      return;
    answered_ = true;
    if (writeNotifier_)
      writeNotifier_->setEnabled(false);
    if (readNotifier_)
      readNotifier_->setEnabled(false);
    // Hand the callback off the object before scheduling its destruction, so
    // nothing it does can reach back into a half-dead request.
    const Reply reply = std::move(reply_);
    deleteLater();
    if (reply)
      reply(ok, payload);
  }

  static constexpr qsizetype kMaxReplyBytes = 64 * 1024;

  QByteArray request_;
  QByteArray response_;
  Reply reply_;
  QSocketNotifier *writeNotifier_ = nullptr;
  QSocketNotifier *readNotifier_ = nullptr;
  qsizetype sent_ = 0;
  int id_ = 0;
  int fd_ = -1;
  bool connected_ = false;
  bool answered_ = false;
};

} // namespace

QStringList recordSessionArguments(const RecordConfig &config) {
  const QStringList source = recordTargetSourceArguments(config.target);
  if (source.isEmpty() || config.outputPath.isEmpty() ||
      config.ipcSocketPath.isEmpty())
    return {};

  QStringList arguments = source;
  arguments << QStringLiteral("-f")
            << QString::number(qBound(1, config.fps, 500))
            // Constant frame rate: the Studio timeline addresses frames, and
            // a variable-rate master makes every seek an estimate.
            << QStringLiteral("-fm") << QStringLiteral("cfr")
            << QStringLiteral("-k") << QStringLiteral("h264")
            << QStringLiteral("-fallback-cpu-encoding") << QStringLiteral("yes")
            << QStringLiteral("-cursor")
            << (config.cursor ? QStringLiteral("yes") : QStringLiteral("no"))
            // The container carries the capture machine's name and the
            // encoder's provenance otherwise; a screen recording is shared.
            << QStringLiteral("-exclude-metadata") << QStringLiteral("yes");
  if (config.systemAudio)
    arguments << QStringLiteral("-a") << QStringLiteral("default_output");
  if (config.microphone)
    arguments << QStringLiteral("-a") << QStringLiteral("default_input");
  arguments << QStringLiteral("-ipc") << config.ipcSocketPath
            << QStringLiteral("-o") << config.outputPath;
  return arguments;
}

QString findRecorderExecutable() {
  return QStandardPaths::findExecutable(
      QStringLiteral("gpu-screen-recorder"));
}

RecordSession::RecordSession(RecordConfig config, QObject *parent)
    : QObject(parent), config_(std::move(config)) {}

RecordSession::~RecordSession() {
  if (!process_)
    return;
  // Nothing is being awaited any more, so leave no encoder behind. The
  // parent-death signal covers an abrupt exit; this covers an orderly one.
  if (process_->state() != QProcess::NotRunning) {
    ::kill(static_cast<pid_t>(process_->processId()), SIGINT);
    if (!process_->waitForFinished(3000))
      process_->kill();
  }
}

bool RecordSession::start(QString &error) {
  if (state_ != State::Idle) {
    error = QStringLiteral("The recording session has already been started");
    return false;
  }
  const QString executable = findRecorderExecutable();
  if (executable.isEmpty()) {
    error = QStringLiteral(
        "gpu-screen-recorder is not installed; install it to record "
        "(omarchy-pkg-add gpu-screen-recorder)");
    return false;
  }
  const QStringList arguments = recordSessionArguments(config_);
  if (arguments.isEmpty()) {
    error = QStringLiteral("Nothing usable to record");
    return false;
  }
  if (QFile::encodeName(config_.ipcSocketPath).size() >
      kMaxControlSocketPathBytes) {
    // Better here than as an unexplained bind() failure inside the encoder.
    error = QStringLiteral("The control socket path is too long (%1 bytes, "
                           "limit %2)")
                .arg(QFile::encodeName(config_.ipcSocketPath).size())
                .arg(kMaxControlSocketPathBytes);
    return false;
  }
  // A leftover socket from a killed session makes readiness detection lie.
  QFile::remove(config_.ipcSocketPath);

  process_ = new QProcess(this);
  process_->setProgram(executable);
  process_->setArguments(arguments);
  process_->setProcessChannelMode(QProcess::MergedChannels);
  // Evaluated here, in the parent, so the child can tell whether it was
  // reparented before it got as far as asking for a death signal.
  const pid_t parentPid = ::getpid();
  process_->setChildProcessModifier([parentPid] {
    // Until exec() the child still carries this process's SIGINT/SIGTERM
    // handlers, and those write to a socket instead of terminating -- they
    // would swallow the parent-death signal below.
    ::signal(SIGINT, SIG_DFL);
    ::signal(SIGTERM, SIG_DFL);
    // If this process dies, the encoder is asked to stop rather than
    // carrying on invisibly with nothing showing that the screen is being
    // recorded.
    ::prctl(PR_SET_PDEATHSIG, SIGINT);
    // The parent can have died between fork() and that line, in which case
    // no death signal will ever be generated: this child is already an
    // orphan and must not go on to exec an encoder nobody owns.
    if (::getppid() != parentPid)
      ::_exit(1);
  });
  connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
    stderrTail_.append(QString::fromUtf8(process_->readAllStandardOutput()));
    if (stderrTail_.size() > kDiagnosticTailBytes)
      stderrTail_ = stderrTail_.right(kDiagnosticTailBytes);
  });
  connect(process_, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError processError) {
            if (processError == QProcess::FailedToStart)
              fail(QStringLiteral("Could not start gpu-screen-recorder"));
          });
  connect(process_, &QProcess::finished, this,
          [this](int exitCode, QProcess::ExitStatus status) {
            handleProcessFinished(exitCode, static_cast<int>(status));
          });

  state_ = State::Starting;
  process_->start();
  if (!process_->waitForStarted(5000)) {
    state_ = State::Failed;
    error = QStringLiteral("Could not start gpu-screen-recorder");
    return false;
  }

  readyTimer_ = new QTimer(this);
  readyTimer_->setInterval(kReadyPollMs);
  connect(readyTimer_, &QTimer::timeout, this,
          &RecordSession::pollForReadiness);
  readyTimer_->start();
  return true;
}

void RecordSession::pollForReadiness() {
  if (state_ != State::Starting) {
    readyTimer_->stop();
    return;
  }
  // The socket appearing only means a file exists. Ready means the encoder
  // answers on it, so the socket is a cue to ask rather than the answer.
  if (!probeInFlight_ && QFile::exists(config_.ipcSocketPath))
    probeReadiness();
  if (++readyAttempts_ < kReadyAttempts)
    return;
  readyTimer_->stop();
  // Do not leave a child recording behind a session that has given up on it.
  endChildProcess();
  fail(QStringLiteral("gpu-screen-recorder did not start recording"));
}

void RecordSession::probeReadiness() {
  probeInFlight_ = true;
  // set-paused false is the one request that is both harmless and
  // idempotent: it asks the encoder to be exactly what it already is, and
  // an `ok` proves it is listening rather than merely having made a file.
  sendCommand(QStringLiteral("set-paused"), QJsonValue(false),
              [this](bool ok, const QString &) {
                probeInFlight_ = false;
                if (!ok || state_ != State::Starting)
                  return; // Keep polling; the readiness budget still applies.
                readyTimer_->stop();
                state_ = State::Recording;
                clock_.start();
                emit recording();
              });
}

void RecordSession::sendCommand(
    const QString &name, const QJsonValue &data,
    std::function<void(bool ok, const QString &payload)> reply) {
  const int id = nextRequestId_++;
  QJsonObject request{{QStringLiteral("id"), id},
                      {QStringLiteral("name"), name}};
  if (!data.isNull() && !data.isUndefined())
    request.insert(QStringLiteral("data"), data);
  const QByteArray line =
      QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
  // A stop reply only arrives once the file is written, which for a long
  // recording is not instant.
  const int timeout = name == QStringLiteral("stop") ? kStopTimeoutMs
                                                     : kCommandTimeoutMs;
  new GsrRequest(config_.ipcSocketPath, id, line, timeout, std::move(reply),
                 this);
}

void RecordSession::setPaused(bool paused) {
  if (paused ? state_ != State::Recording : state_ != State::Paused)
    return;
  // set-paused rather than toggle-pause: the request carries the state it
  // wants, so a dropped reply cannot leave the two sides disagreeing.
  sendCommand(QStringLiteral("set-paused"), QJsonValue(paused),
              [this, paused](bool ok, const QString &) {
                // The reply can land after the user has already stopped, and
                // moving back to Recording there would restart the clock on a
                // session that is finishing.
                if (!ok || (state_ != State::Recording &&
                            state_ != State::Paused))
                  return;
                if (paused) {
                  pauseStartedMs_ = clock_.elapsed();
                  state_ = State::Paused;
                } else {
                  pausedTotalMs_ += clock_.elapsed() - pauseStartedMs_;
                  state_ = State::Recording;
                }
                emit pausedChanged(paused);
              });
}

void RecordSession::armExitWatchdog(int milliseconds) {
  QTimer::singleShot(milliseconds, this, [this] {
    if (state_ != State::Stopping || !process_ ||
        process_->state() == QProcess::NotRunning)
      return;
    process_->terminate();
    // A timer rather than waitForFinished(): the indicator is on screen and
    // has to keep painting while this happens.
    QTimer::singleShot(kTerminateGraceMs, this, [this] {
      if (process_ && process_->state() != QProcess::NotRunning)
        process_->kill();
    });
  });
}

void RecordSession::endChildProcess() {
  if (!process_ || process_->state() == QProcess::NotRunning)
    return;
  ::kill(static_cast<pid_t>(process_->processId()), SIGINT);
  QTimer::singleShot(kTerminateGraceMs, this, [this] {
    if (process_ && process_->state() != QProcess::NotRunning)
      process_->kill();
  });
}

void RecordSession::stop() {
  if (state_ != State::Recording && state_ != State::Paused)
    return;
  state_ = State::Stopping;
  // Armed now, not on the reply: a stop request that times out, is refused,
  // or loses its socket has to end somewhere too, or the indicator sits on
  // "Saving…" for good.
  armExitWatchdog(kStopTimeoutMs + kExitWatchdogMs);
  sendCommand(QStringLiteral("stop"), {}, [this](bool ok, const QString &path) {
    if (ok && !path.isEmpty()) {
      savedPath_ = path;
      // The encoder exits next and handleProcessFinished reports. If it says
      // it saved and then does not leave, stop waiting for it.
      armExitWatchdog(kExitWatchdogMs);
      return;
    }
    // The socket went away or refused. The child is still ours by pid, and
    // SIGINT is GSR's documented save-and-exit.
    if (process_ && process_->state() != QProcess::NotRunning)
      ::kill(static_cast<pid_t>(process_->processId()), SIGINT);
    else
      handleProcessFinished(0, static_cast<int>(QProcess::NormalExit));
  });
}

void RecordSession::handleProcessFinished(int exitCode, int exitStatus) {
  if (state_ == State::Done || state_ == State::Failed)
    return;
  const bool clean =
      exitStatus == static_cast<int>(QProcess::NormalExit) && exitCode == 0;
  if (state_ != State::Stopping) {
    fail(clean ? QStringLiteral("gpu-screen-recorder stopped unexpectedly")
               : QStringLiteral("gpu-screen-recorder exited with code %1")
                     .arg(exitCode));
    return;
  }
  if (!clean) {
    // Crashing during the stop is still a crash. Calling it Done would send
    // a partly written file down the path that presents it as saved.
    fail(QStringLiteral("gpu-screen-recorder did not finish saving (exit "
                        "code %1)")
             .arg(exitCode));
    return;
  }
  state_ = State::Done;
  emit finished(config_.outputPath);
}

void RecordSession::fail(const QString &message) {
  if (state_ == State::Failed || state_ == State::Done)
    return;
  state_ = State::Failed;
  const QString tail = stderrTail_.trimmed().section(QLatin1Char('\n'), -1);
  emit failed(tail.isEmpty() ? message
                             : QStringLiteral("%1 (%2)").arg(message, tail));
}

qint64 RecordSession::elapsedMs() const {
  if (!clock_.isValid())
    return 0;
  qint64 elapsed = clock_.elapsed() - pausedTotalMs_;
  if (state_ == State::Paused)
    elapsed -= clock_.elapsed() - pauseStartedMs_;
  return qMax<qint64>(0, elapsed);
}
