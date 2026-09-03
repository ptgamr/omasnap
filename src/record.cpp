/** @fileoverview The recorder process: encoder ownership, the indicator, and
 *  what happens to the file afterwards. */
#include "record.hpp"

#include "capture.hpp"
#include "record-indicator.hpp"
#include "record-session.hpp"
#include "record-target.hpp"
#include "quit-signals.hpp"

#include <LayerShellQt/Window>

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLockFile>
#include <QProcess>
#include <QScreen>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>
#include <QWindow>

#include <memory>

#include <cerrno>
#include <csignal>

namespace {

/// Layer-shell namespace for the indicator. Kept separate from the
/// screenshot overlay's `omasnap` scope so a Hyprland `noscreenshare` rule
/// can name it on its own; see README.
const auto kIndicatorScope = QStringLiteral("omasnap-record");
constexpr int kIndicatorTopMargin = 8;
constexpr int kIndicatorRightMargin = 12;
constexpr int kClockIntervalMs = 250;
/// How long the pill stays up after a failure, so the reason is readable.
constexpr int kFailureLingerMs = 4000;

QString runtimeRecordDirectory() {
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty())
    return {};
  const QString directory = QDir(runtime).filePath(QStringLiteral("record"));
  return ensurePrivateDirectory(directory) ? directory : QString();
}

void restrictToOwner(const QString &path) {
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

/// `recording-<timestamp>[-<what>]`, date first so the folder sorts
/// chronologically, exactly like the screenshot names do.
QString recordingStem(const RecordTarget &target) {
  QString stem = QStringLiteral("recording-%1")
                     .arg(QDateTime::currentDateTime().toString(
                         QStringLiteral("yyyy-MM-dd_HH-mm-ss")));
  const QString slug = recordTargetSlug(target);
  if (!slug.isEmpty())
    stem += QLatin1Char('-') + slug;
  return stem;
}

/// A name that is not taken yet, so a recovered master never overwrites one.
QString uncontendedPath(const QDir &directory, const QString &stem,
                        const QString &suffix) {
  QString candidate = directory.filePath(stem + suffix);
  for (int index = 2; QFileInfo::exists(candidate) && index < 1000; ++index)
    candidate = directory.filePath(
        QStringLiteral("%1-%2%3").arg(stem).arg(index).arg(suffix));
  return candidate;
}

/**
 * Promotes masters left behind by a recorder that died. Only ever called
 * while holding the recording lock, which is what makes "left behind" true:
 * no other recorder of ours can be writing one.
 */
int recoverAbandonedRecordings(const QDir &directory) {
  int recovered = 0;
  const QStringList parts = directory.entryList(
      {QStringLiteral("*.part.mkv")}, QDir::Files, QDir::Name);
  for (const QString &part : parts) {
    const QString stem = part.chopped(QStringLiteral(".part.mkv").size());
    const QString promoted =
        uncontendedPath(directory, stem, QStringLiteral(".mkv"));
    if (QFile::rename(directory.filePath(part), promoted))
      ++recovered;
  }
  return recovered;
}

/**
 * Keeps the Matroska master as the finished recording, under a name nothing
 * else has taken, and returns the path it ended up at.
 */
QString promoteMatroska(const QDir &directory, const QString &stem,
                        const QString &master) {
  const QString kept = uncontendedPath(directory, stem, QStringLiteral(".mkv"));
  const QString promoted = QFile::rename(master, kept) ? kept : master;
  restrictToOwner(promoted);
  return promoted;
}

/**
 * Whether `pid` is an omasnap right now. Read from /proc immediately before
 * signalling, because a lock file only records what the pid was when it was
 * written and the kernel reuses pids.
 */
bool processIsOmasnap(qint64 pid) {
  QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
  if (!comm.open(QIODevice::ReadOnly))
    return false;
  return QString::fromLatin1(comm.readLine()).trimmed() ==
         QCoreApplication::applicationName();
}

/** The Studio next to this executable, then one on PATH, else nothing. */
QString studioExecutable() {
  const QString sibling = QDir(QCoreApplication::applicationDirPath())
                              .filePath(QStringLiteral("omasnap-studio"));
  if (QFileInfo(sibling).isExecutable())
    return sibling;
  return QStandardPaths::findExecutable(QStringLiteral("omasnap-studio"));
}

void notifyRecording(const QString &headline, const QString &path) {
  QStringList arguments{QStringLiteral("-g"), QStringLiteral(""),
                        QStringLiteral("--app-name"),
                        QStringLiteral("omasnap"), headline};
  if (!path.isEmpty()) {
    QString studio = studioExecutable();
    const bool haveStudio = !studio.isEmpty();
    if (!haveStudio)
      studio = QStringLiteral("xdg-open");
    arguments << (haveStudio ? QStringLiteral("Click to open in Studio")
                             : QFileInfo(path).fileName())
              << QStringLiteral("--exec")
              << QStringLiteral("%1 %2").arg(shellQuote(studio),
                                             shellQuote(path));
  }
  arguments << QStringLiteral("-t") << QStringLiteral("6000");
  QProcess::startDetached(QStringLiteral("omarchy-notification-send"),
                          arguments);
}

/**
 * Anchors the indicator under the top bar on the recorded output. Returns the
 * layer surface, or nullptr when one could not be created.
 */
LayerShellQt::Window *showIndicator(RecordIndicator &indicator,
                                    const QString &outputName) {
  QScreen *screen = QGuiApplication::primaryScreen();
  for (QScreen *candidate : QGuiApplication::screens()) {
    if (candidate->name() == outputName) {
      screen = candidate;
      break;
    }
  }
  indicator.resize(indicator.sizeHint());
  static_cast<void>(indicator.winId());
  QWindow *handle = indicator.windowHandle();
  LayerShellQt::Window *layer =
      handle ? LayerShellQt::Window::get(handle) : nullptr;
  if (!handle || !layer)
    return nullptr;
  if (screen)
    layer->setScreen(screen);
  layer->setScope(kIndicatorScope);
  LayerShellQt::Window::Anchors anchors;
  anchors.setFlag(LayerShellQt::Window::AnchorTop);
  anchors.setFlag(LayerShellQt::Window::AnchorRight);
  layer->setAnchors(anchors);
  layer->setMargins(QMargins(0, kIndicatorTopMargin, kIndicatorRightMargin, 0));
  // Zero rather than -1: the indicator then sits below whatever the bar has
  // reserved instead of on top of it.
  layer->setExclusiveZone(0);
  layer->setDesiredSize(indicator.size());
  // Never takes the keyboard: the user is recording something, and that
  // something needs the keys.
  layer->setKeyboardInteractivity(
      LayerShellQt::Window::KeyboardInteractivityNone);
  layer->setActivateOnShow(false);
  layer->setLayer(LayerShellQt::Window::LayerOverlay);
  // The pill grows when "Starting…" becomes a clock with two buttons; the
  // surface has to be told, or the compositor keeps the old size and clips.
  QObject::connect(&indicator, &RecordIndicator::desiredSizeChanged, layer,
                   [layer](const QSize &size) { layer->setDesiredSize(size); });
  indicator.show();
  return layer;
}

} // namespace

QString recordingsDirectory() {
  QString base =
      QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  if (base.isEmpty())
    base = QDir(QDir::homePath()).filePath(QStringLiteral("Videos"));
  return QDir(base).filePath(QStringLiteral("Recordings"));
}

bool handOffToRecorder(const RecordTarget &target, const RecordOptions &options,
                       QString &error) {
  const QString directory = runtimeRecordDirectory();
  if (directory.isEmpty()) {
    error = QStringLiteral("Could not create the private recording directory");
    return false;
  }
  const QString path =
      QDir(directory).filePath(QStringLiteral("target-%1.json")
                                   .arg(QUuid::createUuid().toString(
                                       QUuid::Id128)));
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    error = QStringLiteral("Could not write the recording target");
    return false;
  }
  restrictToOwner(path);
  const QByteArray payload = writeRecordTarget(target);
  if (file.write(payload) != payload.size() || !file.flush()) {
    file.close();
    QFile::remove(path);
    error = QStringLiteral("Could not write the recording target");
    return false;
  }
  file.close();

  QStringList arguments{QStringLiteral("--record-run"), path};
  if (options.systemAudio)
    arguments << QStringLiteral("--audio");
  if (options.microphone)
    arguments << QStringLiteral("--mic");
  arguments << QStringLiteral("--fps") << QString::number(options.fps);
  if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                               arguments)) {
    QFile::remove(path);
    error = QStringLiteral("Could not start the recorder");
    return false;
  }
  return true;
}

bool stopActiveRecording(QString &error) {
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty()) {
    error = QStringLiteral("Could not open the private runtime directory");
    return false;
  }
  QLockFile lock(QDir(runtime).filePath(QStringLiteral("omasnap.record")));
  lock.setStaleLockTime(0);
  // Taking the lock means nobody is holding it, and QLockFile clears one
  // whose owner is gone, so this also rules out a dead recorder's leftovers.
  if (lock.tryLock(0)) {
    lock.unlock();
    error = QStringLiteral("No recording is running");
    return false;
  }
  qint64 pid = 0;
  QString hostname;
  QString application;
  if (!lock.getLockInfo(&pid, &hostname, &application) || pid <= 0 ||
      (!hostname.isEmpty() && hostname != QSysInfo::machineHostName())) {
    error = QStringLiteral("No recording is running");
    return false;
  }
  // Pids are reused. Signalling one read out of a file, on the strength of
  // the file alone, is how a recorder's stop key ends up killing a stranger's
  // process; check what the pid actually is, immediately before signalling.
  if (!processIsOmasnap(pid)) {
    error = QStringLiteral("No recording is running");
    return false;
  }
  // SIGTERM rather than a socket of our own: the recorder already turns the
  // first one into the same stop-and-save the indicator button does.
  if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) {
    error = errno == ESRCH
                ? QStringLiteral("No recording is running")
                : QStringLiteral("Could not stop the recorder (pid %1)")
                      .arg(pid);
    return false;
  }
  return true;
}

int runRecorder(const QString &targetPath, const RecordOptions &options,
                PosixSignalNotifier *quitSignals) {
  QFile targetFile(targetPath);
  QByteArray payload;
  if (targetFile.open(QIODevice::ReadOnly))
    payload = targetFile.readAll();
  targetFile.close();
  // The target is consumed: it is a handoff, not a document.
  QFile::remove(targetPath);

  RecordTarget target;
  QString error;
  if (!readRecordTarget(payload, target, error)) {
    qCritical().noquote() << error;
    notifyRecording(QStringLiteral("Recording failed: %1").arg(error), {});
    return 1;
  }

  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty()) {
    qCritical() << "Could not create private runtime directory";
    return 1;
  }
  // Separate from omasnap.instance on purpose: a screenshot taken during a
  // recording must not end the recording, and starting a recording must not
  // dismiss an open screenshot overlay.
  QLockFile recordingLock(
      QDir(runtime).filePath(QStringLiteral("omasnap.record")));
  recordingLock.setStaleLockTime(0);
  if (!recordingLock.tryLock(0)) {
    notifyRecording(QStringLiteral("A recording is already running"), {});
    qCritical() << "A recording is already running";
    return 1;
  }

  const QString directoryPath = recordingsDirectory();
  if (!QDir().mkpath(directoryPath)) {
    const QString message =
        QStringLiteral("Could not create %1").arg(directoryPath);
    qCritical().noquote() << message;
    notifyRecording(message, {});
    return 1;
  }
  const QDir directory(directoryPath);
  if (const int recovered = recoverAbandonedRecordings(directory);
      recovered > 0) {
    notifyRecording(recovered == 1
                        ? QStringLiteral("Recovered an interrupted recording")
                        : QStringLiteral("Recovered %1 interrupted recordings")
                              .arg(recovered),
                    {});
  }

  const QString stem = recordingStem(target);
  RecordConfig config;
  config.target = target;
  config.fps = options.fps;
  config.systemAudio = options.systemAudio;
  config.microphone = options.microphone;
  config.outputPath = directory.filePath(stem + QStringLiteral(".part.mkv"));
  // Short by necessity: a unix socket path is capped at 108 bytes, which a
  // path under the recordings directory would not fit inside.
  const QString socketDirectory = runtimeRecordDirectory();
  if (socketDirectory.isEmpty()) {
    qCritical() << "Could not create the private recording directory";
    return 1;
  }
  config.ipcSocketPath =
      QDir(socketDirectory)
          .filePath(QStringLiteral("gsr-%1.sock")
                        .arg(QCoreApplication::applicationPid()));

  RecordIndicator indicator;
  RecordSession session(config);

  auto finish = [&indicator](int code) {
    indicator.hide();
    QCoreApplication::exit(code);
  };

  QObject::connect(&session, &RecordSession::recording, &indicator, [&] {
    indicator.setPhase(RecordIndicator::Phase::Recording);
  });
  QObject::connect(&session, &RecordSession::pausedChanged, &indicator,
                   [&](bool paused) {
                     indicator.setPhase(paused
                                            ? RecordIndicator::Phase::Paused
                                            : RecordIndicator::Phase::Recording);
                   });
  QObject::connect(&indicator, &RecordIndicator::pauseRequested, &session,
                   [&](bool paused) { session.setPaused(paused); });
  const auto requestStop = [&] {
    if (session.state() != RecordSession::State::Recording &&
        session.state() != RecordSession::State::Paused)
      return;
    indicator.setPhase(RecordIndicator::Phase::Stopping);
    session.stop();
  };
  QObject::connect(&indicator, &RecordIndicator::stopRequested, &session,
                   requestStop);
  // Logging out, `omasnap --record --stop`, or a plain Ctrl-C finishes the
  // recording rather than abandoning it. A second signal quits regardless.
  if (quitSignals)
    quitSignals->setFirstSignalHandler(requestStop);

  QObject::connect(&session, &RecordSession::failed, &indicator,
                   [&](const QString &message) {
                     qCritical().noquote() << message;
                     indicator.setPhase(RecordIndicator::Phase::Failed);
                     indicator.setMessage(message.left(60));
                     notifyRecording(
                         QStringLiteral("Recording failed: %1").arg(message),
                         {});
                     QTimer::singleShot(kFailureLingerMs, &indicator,
                                        [&] { finish(1); });
                   });

  QObject::connect(
      &session, &RecordSession::finished, &indicator,
      [&](const QString &savedPath) {
        const QString master = QFileInfo::exists(savedPath) ? savedPath
                                                            : config.outputPath;
        if (!QFileInfo::exists(master)) {
          notifyRecording(QStringLiteral("Recording produced no file"), {});
          finish(1);
          return;
        }
        restrictToOwner(master);
        // Matroska survives an interrupted session; MP4 is what everything
        // else opens. Remuxing copies the streams, so it costs a rewrite of
        // the container and nothing of the video.
        const QString mp4 =
            uncontendedPath(directory, stem, QStringLiteral(".mp4"));
        const QString ffmpeg =
            QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) {
          notifyRecording(QStringLiteral("Recording saved"),
                          promoteMatroska(directory, stem, master));
          finish(0);
          return;
        }
        auto *remux = new QProcess(&indicator);
        // finished and errorOccurred can both arrive; whichever lands first
        // decides, and the other is ignored.
        auto settled = std::make_shared<bool>(false);
        auto conclude = [&, settled, remux, master, mp4](bool ok) {
          if (*settled)
            return;
          *settled = true;
          remux->deleteLater();
          if (ok && QFileInfo::exists(mp4)) {
            restrictToOwner(mp4);
            QFile::remove(master);
            notifyRecording(QStringLiteral("Recording saved"), mp4);
          } else {
            QFile::remove(mp4);
            notifyRecording(QStringLiteral("Recording saved"),
                            promoteMatroska(directory, stem, master));
          }
          finish(0);
        };
        QObject::connect(remux, &QProcess::finished, &indicator,
                         [conclude](int exitCode, QProcess::ExitStatus status) {
                           conclude(exitCode == 0 &&
                                    status == QProcess::NormalExit);
                         });
        QObject::connect(remux, &QProcess::errorOccurred, &indicator,
                         [conclude] { conclude(false); });
        remux->start(ffmpeg, {QStringLiteral("-hide_banner"),
                              QStringLiteral("-loglevel"),
                              QStringLiteral("error"), QStringLiteral("-y"),
                              QStringLiteral("-i"), master,
                              QStringLiteral("-c"), QStringLiteral("copy"),
                              QStringLiteral("-map_metadata"),
                              QStringLiteral("-1"),
                              QStringLiteral("-movflags"),
                              QStringLiteral("+faststart"), mp4});
      });

  if (showIndicator(indicator, target.output) == nullptr) {
    qCritical() << "Could not create the recording indicator layer";
    return 1;
  }
  // start() reports a synchronous failure through `error`; letting it also
  // emit failed would notify twice about the same thing.
  bool started = false;
  {
    const QSignalBlocker quiet(&session);
    started = session.start(error);
  }
  if (!started) {
    qCritical().noquote() << error;
    notifyRecording(QStringLiteral("Recording failed: %1").arg(error), {});
    return 1;
  }

  QTimer clock;
  clock.setInterval(kClockIntervalMs);
  QObject::connect(&clock, &QTimer::timeout, &indicator,
                   [&] { indicator.setElapsed(session.elapsedMs()); });
  clock.start();

  return QApplication::exec();
}
