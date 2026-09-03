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
    writeNotifier_->setEnabled(false);
    int socketError = 0;
    socklen_t length = sizeof(socketError);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socketError, &length) != 0 ||
        socketError != 0) {
      answer(false, QString::fromLocal8Bit(std::strerror(
                        socketError != 0 ? socketError : errno)));
      return;
    }
    // Requests are one short line; a partial write would mean the encoder is
    // not reading at all, which the timeout already covers.
    if (::send(fd_, request_.constData(), static_cast<size_t>(request_.size()),
               MSG_NOSIGNAL) != request_.size()) {
      answer(false, QStringLiteral("could not send the request"));
      return;
    }
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
  int id_ = 0;
  int fd_ = -1;
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
  // A leftover socket from a killed session makes readiness detection lie.
  QFile::remove(config_.ipcSocketPath);

  process_ = new QProcess(this);
  process_->setProgram(executable);
  process_->setArguments(arguments);
  process_->setProcessChannelMode(QProcess::MergedChannels);
  process_->setChildProcessModifier([] {
    // If this process dies, the encoder is asked to stop rather than
    // carrying on invisibly with nothing showing that the screen is being
    // recorded.
    ::prctl(PR_SET_PDEATHSIG, SIGINT);
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
          [this](int exitCode) { handleProcessFinished(exitCode); });

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
  // GSR creates its IPC socket as it starts up, so the socket appearing is
  // both "the encoder is alive" and "there is a channel to stop it with".
  if (QFile::exists(config_.ipcSocketPath)) {
    readyTimer_->stop();
    state_ = State::Recording;
    clock_.start();
    emit recording();
    return;
  }
  if (++readyAttempts_ >= kReadyAttempts) {
    readyTimer_->stop();
    fail(QStringLiteral("gpu-screen-recorder did not start recording"));
  }
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

void RecordSession::stop() {
  if (state_ != State::Recording && state_ != State::Paused)
    return;
  state_ = State::Stopping;
  sendCommand(QStringLiteral("stop"), {}, [this](bool ok, const QString &path) {
    if (ok && !path.isEmpty()) {
      savedPath_ = path;
      // The encoder exits next and handleProcessFinished reports. If it does
      // not, end it rather than leaving the indicator saving forever.
      QTimer::singleShot(kExitWatchdogMs, this, [this] {
        if (state_ != State::Stopping || !process_ ||
            process_->state() == QProcess::NotRunning)
          return;
        process_->terminate();
        if (!process_->waitForFinished(2000))
          process_->kill();
      });
      return;
    }
    // The socket went away or refused. The child is still ours by pid, and
    // SIGINT is GSR's documented save-and-exit.
    if (process_ && process_->state() != QProcess::NotRunning)
      ::kill(static_cast<pid_t>(process_->processId()), SIGINT);
    else
      handleProcessFinished(0);
  });
}

void RecordSession::handleProcessFinished(int exitCode) {
  if (state_ == State::Done || state_ == State::Failed)
    return;
  if (state_ != State::Stopping) {
    fail(exitCode == 0
             ? QStringLiteral("gpu-screen-recorder stopped unexpectedly")
             : QStringLiteral("gpu-screen-recorder exited with code %1")
                   .arg(exitCode));
    return;
  }
  state_ = State::Done;
  // GSR reports the path it wrote; fall back to the one we asked for, which
  // is the same file whenever it stopped on a signal instead of a reply.
  emit finished(savedPath_.isEmpty() ? config_.outputPath : savedPath_);
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
