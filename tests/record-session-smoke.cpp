/** @fileoverview Tests the recorder: the gpu-screen-recorder command shape,
 *  and a real RecordSession driven against a fake encoder that speaks the
 *  same control protocol and misbehaves on request. */
#include "record-session-smoke.hpp"

#include "capture.hpp"
#include "record-session.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <cerrno>
#include <cstring>
#include <functional>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

RecordConfig baseConfig() {
  MonitorInfo monitor;
  monitor.name = QStringLiteral("HDMI-A-1");
  monitor.geometry = {2560, 0, 2560, 1440};
  monitor.pixelSize = {3840, 2160};
  monitor.scale = 1.5;

  RecordConfig config;
  config.target = makeRecordTarget(monitor, RecordTargetKind::Region,
                                   QRectF(440, 300, 800, 600));
  config.outputPath = QStringLiteral("/tmp/omasnap/recording.part.mkv");
  config.ipcSocketPath = QStringLiteral("/run/user/1000/omasnap/rec.sock");
  return config;
}

/// The value that follows `flag`, or a null string when it is absent.
QString valueAfter(const QStringList &arguments, const QString &flag) {
  const qsizetype index = arguments.indexOf(flag);
  if (index < 0 || index + 1 >= arguments.size())
    return {};
  return arguments.at(index + 1);
}

bool check(bool condition, QString &error, const QString &message) {
  if (condition)
    return true;
  error = message;
  return false;
}

} // namespace

bool runRecordSessionSmoke(QString &error) {
  const RecordConfig config = baseConfig();
  const QStringList arguments = recordSessionArguments(config);

  if (!check(valueAfter(arguments, QStringLiteral("-w")) ==
                 QStringLiteral("800x600+3000+300"),
             error, QStringLiteral("region source argument is wrong")))
    return false;
  if (!check(valueAfter(arguments, QStringLiteral("-o")) == config.outputPath &&
                 valueAfter(arguments, QStringLiteral("-ipc")) ==
                     config.ipcSocketPath,
             error, QStringLiteral("output or control socket is missing")))
    return false;
  if (!check(valueAfter(arguments, QStringLiteral("-fm")) ==
                     QStringLiteral("cfr") &&
                 valueAfter(arguments, QStringLiteral("-f")) ==
                     QStringLiteral("60"),
             error, QStringLiteral("timing policy is not the frozen one")))
    return false;
  if (!check(valueAfter(arguments, QStringLiteral("-exclude-metadata")) ==
                 QStringLiteral("yes"),
             error, QStringLiteral("capture metadata is not excluded")))
    return false;

  // Nothing in the vector may look like a shell fragment: this is exec'd as
  // an argument array and must stay that way even if a path ever carries a
  // space or a quote.
  for (const QString &argument : arguments) {
    if (argument.contains(QLatin1Char(';')) ||
        argument.contains(QLatin1Char('|')) ||
        argument.contains(QLatin1Char('&')) ||
        argument.contains(QLatin1Char('`')) ||
        argument.contains(QLatin1Char('$'))) {
      error = QStringLiteral("argument vector contains a shell metacharacter: "
                             "%1")
                  .arg(argument);
      return false;
    }
  }

  // Audio is opt-in, and each source is its own -a so the encoder can keep
  // them apart.
  if (!check(!arguments.contains(QStringLiteral("-a")), error,
             QStringLiteral("audio was captured without being asked for")))
    return false;
  RecordConfig withAudio = config;
  withAudio.systemAudio = true;
  withAudio.microphone = true;
  const QStringList audible = recordSessionArguments(withAudio);
  if (!check(audible.count(QStringLiteral("-a")) == 2 &&
                 audible.contains(QStringLiteral("default_output")) &&
                 audible.contains(QStringLiteral("default_input")),
             error, QStringLiteral("audio sources were not passed separately")))
    return false;

  // A whole display names the output rather than a rectangle.
  MonitorInfo monitor;
  monitor.name = QStringLiteral("DP-1");
  monitor.geometry = {1120, 0, 1440, 2560};
  monitor.scale = 1.0;
  RecordConfig display = config;
  display.target = makeRecordTarget(monitor, RecordTargetKind::Fullscreen, {});
  if (!check(valueAfter(recordSessionArguments(display),
                        QStringLiteral("-w")) == QStringLiteral("DP-1"),
             error, QStringLiteral("display source argument is wrong")))
    return false;

  // Anything that could not produce a recording produces no command at all,
  // rather than a command that records the wrong thing.
  RecordConfig noTarget = config;
  noTarget.target = {};
  if (!check(recordSessionArguments(noTarget).isEmpty(), error,
             QStringLiteral("an empty target still built a command")))
    return false;
  RecordConfig noOutput = config;
  noOutput.outputPath.clear();
  if (!check(recordSessionArguments(noOutput).isEmpty(), error,
             QStringLiteral("a missing output path still built a command")))
    return false;
  RecordConfig noSocket = config;
  noSocket.ipcSocketPath.clear();
  if (!check(recordSessionArguments(noSocket).isEmpty(), error,
             QStringLiteral("a session with no control socket was allowed")))
    return false;

  return true;
}

// ---------------------------------------------------------------------------
// The fake encoder, and a real RecordSession driven against it.
//
// Re-execs this binary rather than shipping a script, the same way the
// instance-lock checks re-exec it as a lock holder: no new test-time
// dependency, and the fake can be made to misbehave in exactly the ways that
// broke the real thing.
// ---------------------------------------------------------------------------

namespace {

/// How long a slow reply is held back. Comfortably longer than the gap
/// between the pause and the stop it has to lose a race with.
constexpr int kSlowReplyMs = 1200;

QString argumentAfter(int argc, char **argv, const char *flag) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::strcmp(argv[index], flag) == 0)
      return QString::fromLocal8Bit(argv[index + 1]);
  }
  return {};
}

/// Runs `body` until it says it is done or `timeoutMs` passes; returns
/// whether it finished rather than timed out.
bool spinUntil(const std::function<bool()> &done, int timeoutMs) {
  QElapsedTimer clock;
  clock.start();
  while (!done()) {
    if (clock.elapsed() > timeoutMs)
      return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return true;
}

} // namespace

int runFakeRecorder(int argc, char **argv) {
  const QString mode = qEnvironmentVariable(kFakeRecorderVariable);
  const QString socketPath = argumentAfter(argc, argv, "-ipc");
  const QString outputPath = argumentAfter(argc, argv, "-o");
  if (socketPath.isEmpty() || outputPath.isEmpty())
    return 2;

  // An encoder that dies before it ever opens its control socket. The
  // session has to notice and fail rather than wait forever.
  if (mode == QStringLiteral("no-socket"))
    return 3;

  QFile output(outputPath);
  if (!output.open(QIODevice::WriteOnly))
    return 4;
  output.write("fake recording\n");
  output.close();

  const QByteArray path = QFile::encodeName(socketPath);
  struct sockaddr_un address{};
  if (static_cast<size_t>(path.size()) >= sizeof(address.sun_path))
    return 5;
  const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0)
    return 6;
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.constData(),
              static_cast<size_t>(path.size()));
  if (::bind(listener, reinterpret_cast<struct sockaddr *>(&address),
             sizeof(address)) != 0 ||
      ::listen(listener, 4) != 0) {
    ::close(listener);
    return 7;
  }

  for (;;) {
    const int client = ::accept(listener, nullptr, nullptr);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    QByteArray line;
    char byte = 0;
    while (::read(client, &byte, 1) == 1 && byte != '\n')
      line.append(byte);
    const QJsonObject request = QJsonDocument::fromJson(line).object();
    const QString name = request.value(QStringLiteral("name")).toString();
    QJsonObject reply{{QStringLiteral("id"),
                       request.value(QStringLiteral("id")).toInt()},
                      {QStringLiteral("result"), QStringLiteral("ok")}};
    if (name == QStringLiteral("stop")) {
      reply.insert(QStringLiteral("data"), outputPath);
    } else if (mode == QStringLiteral("slow-pause")) {
      // The reply the real encoder sometimes sends after the user has
      // already given up and stopped.
      ::usleep(kSlowReplyMs * 1000);
    }
    const QByteArray encoded =
        QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n';
    static_cast<void>(::write(client, encoded.constData(), encoded.size()));
    ::close(client);
    if (name == QStringLiteral("stop")) {
      ::close(listener);
      ::unlink(path.constData());
      return 0;
    }
  }
  ::close(listener);
  ::unlink(path.constData());
  return 8;
}

bool runRecordSessionLifecycleSmoke(QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("could not create a temporary directory");
    return false;
  }
  // RecordSession looks the encoder up on PATH, so the fake goes on PATH
  // under the name it looks for.
  const QString shim =
      QDir(directory.path()).filePath(QStringLiteral("gpu-screen-recorder"));
  if (!QFile::link(QCoreApplication::applicationFilePath(), shim)) {
    error = QStringLiteral("could not put the fake encoder on PATH");
    return false;
  }
  qputenv("PATH", (directory.path() + QLatin1Char(':') +
                   qEnvironmentVariable("PATH"))
                      .toLocal8Bit());

  const auto configure = [&directory](const QString &name) {
    MonitorInfo monitor;
    monitor.name = QStringLiteral("DP-1");
    monitor.geometry = {0, 0, 1920, 1080};
    monitor.scale = 1.0;
    RecordConfig config;
    config.target =
        makeRecordTarget(monitor, RecordTargetKind::Fullscreen, {});
    config.outputPath =
        QDir(directory.path()).filePath(name + QStringLiteral(".part.mkv"));
    config.ipcSocketPath =
        QDir(directory.path()).filePath(name + QStringLiteral(".sock"));
    return config;
  };

  // 1. The ordinary run: the session notices the encoder is up, the clock
  //    advances, and stop returns the path the encoder reported saving.
  {
    qputenv(kFakeRecorderVariable, "normal");
    const RecordConfig config = configure(QStringLiteral("normal"));
    RecordSession session(config);
    QSignalSpy recording(&session, &RecordSession::recording);
    QSignalSpy finished(&session, &RecordSession::finished);
    QSignalSpy failed(&session, &RecordSession::failed);
    QString startError;
    if (!check(session.start(startError), error,
               QStringLiteral("session did not start: %1").arg(startError)))
      return false;
    if (!check(spinUntil([&] { return !recording.isEmpty(); }, 15000), error,
               QStringLiteral("session never reported recording")))
      return false;
    if (!check(session.state() == RecordSession::State::Recording, error,
               QStringLiteral("session is not in the recording state")))
      return false;
    session.stop();
    if (!check(spinUntil([&] { return !finished.isEmpty(); }, 15000), error,
               QStringLiteral("stop never finished")))
      return false;
    if (!check(finished.at(0).at(0).toString() == config.outputPath, error,
               QStringLiteral("finished reported the wrong path")))
      return false;
    if (!check(failed.isEmpty() &&
                   session.state() == RecordSession::State::Done,
               error, QStringLiteral("a clean stop reported a failure")))
      return false;
  }

  // 2. The race: a set-paused reply that arrives after the user has already
  //    stopped must not move the session back to recording.
  {
    qputenv(kFakeRecorderVariable, "slow-pause");
    const RecordConfig config = configure(QStringLiteral("slow"));
    RecordSession session(config);
    QSignalSpy recording(&session, &RecordSession::recording);
    QSignalSpy pausedChanged(&session, &RecordSession::pausedChanged);
    QSignalSpy finished(&session, &RecordSession::finished);
    QString startError;
    if (!check(session.start(startError), error,
               QStringLiteral("slow session did not start: %1")
                   .arg(startError)))
      return false;
    if (!check(spinUntil([&] { return !recording.isEmpty(); }, 15000), error,
               QStringLiteral("slow session never reported recording")))
      return false;
    session.setPaused(true);
    session.stop();
    if (!check(spinUntil([&] { return !finished.isEmpty(); }, 20000), error,
               QStringLiteral("slow stop never finished")))
      return false;
    // Let the late reply land, then confirm it changed nothing.
    static_cast<void>(spinUntil([] { return false; }, 500));
    if (!check(pausedChanged.isEmpty(), error,
               QStringLiteral("a late pause reply was applied after stop")))
      return false;
    if (!check(session.state() == RecordSession::State::Done, error,
               QStringLiteral("a late pause reply left the session in %1")
                   .arg(static_cast<int>(session.state()))))
      return false;
  }

  // 3. An encoder that dies before opening its control socket is a failure,
  //    not a session that waits forever.
  {
    qputenv(kFakeRecorderVariable, "no-socket");
    const RecordConfig config = configure(QStringLiteral("dead"));
    RecordSession session(config);
    QSignalSpy failed(&session, &RecordSession::failed);
    QString startError;
    if (!check(session.start(startError), error,
               QStringLiteral("dead session did not start: %1")
                   .arg(startError)))
      return false;
    if (!check(spinUntil([&] { return !failed.isEmpty(); }, 15000), error,
               QStringLiteral("a dead encoder never reported a failure")))
      return false;
    if (!check(session.state() == RecordSession::State::Failed, error,
               QStringLiteral("a dead encoder left a live session")))
      return false;
  }

  qunsetenv(kFakeRecorderVariable);
  return true;
}
