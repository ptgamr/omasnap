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
#include <QRegularExpression>
#include <QScreen>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QDeadlineTimer>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWindow>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <memory>
#include <utility>

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
/// How long the selector waits for the recorder it started to take the
/// recording lock. Short: the child takes it almost immediately, and this
/// runs after the overlay is gone.
constexpr int kHandoffLockWaitMs = 3000;
constexpr int kHandoffPollMs = 15;
/// Reading container metadata is a fast, bounded operation.
constexpr int kProbeTimeoutMs = 5000;

QString runtimeRecordDirectory() {
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty())
    return {};
  const QString directory = QDir(runtime).filePath(QStringLiteral("record"));
  return ensurePrivateDirectory(directory) ? directory : QString();
}

void restrictToOwner(const QString &path) {
  if (!QFile::setPermissions(path,
                             QFileDevice::ReadOwner | QFileDevice::WriteOwner))
    qWarning().noquote()
        << QStringLiteral("Could not make %1 owner-only").arg(path);
}

/**
 * Creates `path` owner-only, and only if nothing is there. O_EXCL because
 * the name is predictable: without it a same-named file left by an earlier
 * recovery, or a symlink someone dropped in, would be truncated instead.
 */
bool createExclusiveFile(const QString &path) {
  const QByteArray encoded = QFile::encodeName(path);
  const int fd = ::open(encoded.constData(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        S_IRUSR | S_IWUSR);
  if (fd < 0)
    return false;
  ::close(fd);
  return true;
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

/** Path of ffprobe, or empty when it is not installed. */
QString findProbeExecutable() {
  return QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
}

/** Asks for the one video stream's codec and the container duration. */
QStringList mediaProbeArguments(const QString &path) {
  return {QStringLiteral("-v"),
          QStringLiteral("error"),
          QStringLiteral("-select_streams"),
          QStringLiteral("v:0"),
          QStringLiteral("-show_entries"),
          QStringLiteral("stream=codec_name"),
          QStringLiteral("-show_entries"),
          QStringLiteral("format=duration"),
          QStringLiteral("-of"),
          QStringLiteral("csv=p=0"),
          path};
}

/// A codec name and a duration above zero. A container ffprobe can open but
/// which holds no playable time is not a recording anyone can watch.
bool probeOutputIsPlayable(const QByteArray &output) {
  const QList<QByteArray> lines = output.trimmed().split('\n');
  if (lines.size() < 2 || lines.first().trimmed().isEmpty())
    return false;
  bool numeric = false;
  const double duration = lines.last().trimmed().toDouble(&numeric);
  return numeric && duration > 0.0;
}

/**
 * Whether `path` is media something can actually open. Without ffprobe the
 * best available answer is "it has bytes in it", and saying so is better
 * than pretending the check happened.
 */
bool looksPlayable(const QString &path, const QString &probe) {
  if (QFileInfo(path).size() <= 0)
    return false;
  if (probe.isEmpty())
    return true;
  QProcess ffprobe;
  ffprobe.start(probe, mediaProbeArguments(path));
  if (!ffprobe.waitForFinished(kProbeTimeoutMs)) {
    ffprobe.kill();
    ffprobe.waitForFinished(1000);
    return false;
  }
  return ffprobe.exitStatus() == QProcess::NormalExit &&
         ffprobe.exitCode() == 0 &&
         probeOutputIsPlayable(ffprobe.readAllStandardOutput());
}

/**
 * Promotes masters left behind by a recorder that died. Only ever called
 * while holding the recording lock, which is what makes "left behind" true:
 * no other recorder of ours can be writing one.
 *
 * Runs before any window exists, which is why the validation may block.
 */
int recoverAbandonedRecordings(const QDir &directory) {
  // Only files this program names this way. `~/Videos/Recordings` is an
  // ordinary user directory: somebody else's half-downloaded `movie.part.mkv`
  // is not ours to rename.
  static const QRegularExpression ours(
      QStringLiteral("\\Arecording-\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2}"
                     "[A-Za-z0-9-]*\\.part\\.mkv\\z"));
  const QString probe = findProbeExecutable();
  int recovered = 0;
  const QStringList parts = directory.entryList(
      {QStringLiteral("*.part.mkv")}, QDir::Files, QDir::Name);
  for (const QString &part : parts) {
    if (!ours.match(part).hasMatch())
      continue;
    const QString source = directory.filePath(part);
    // Never follow a link out of the directory: renaming through one, and
    // then tightening permissions on what it points at, would act on a file
    // that is not ours at all.
    if (QFileInfo(source).isSymLink()) {
      qWarning().noquote()
          << QStringLiteral("Ignored a symlinked interrupted recording: %1")
                 .arg(part);
      continue;
    }
    // A zero-byte master is the file the recorder creates before the encoder
    // writes anything: there is no recording in it to lose.
    if (QFileInfo(source).size() <= 0) {
      QFile::remove(source);
      continue;
    }
    // Anything else that cannot be read is left exactly where it is:
    // renaming it would announce an unplayable file as a recovered
    // recording, and deleting it would throw away something we cannot read
    // but ffmpeg might.
    if (!looksPlayable(source, probe)) {
      qWarning().noquote()
          << QStringLiteral("Left an unplayable interrupted recording in "
                            "place: %1")
                 .arg(part);
      continue;
    }
    const QString stem = part.chopped(QStringLiteral(".part.mkv").size());
    const QString promoted =
        uncontendedPath(directory, stem, QStringLiteral(".mkv"));
    if (QFile::rename(source, promoted)) {
      restrictToOwner(promoted);
      ++recovered;
    }
  }
  return recovered;
}

/**
 * Runs a media tool and reports the result without blocking anything. The
 * process deletes itself; `done` runs once, whichever of finished or
 * errorOccurred arrives first.
 */
void runTool(QObject *context, const QString &program,
             const QStringList &arguments,
             std::function<void(bool ok, const QByteArray &output)> done) {
  auto *process = new QProcess(context);
  process->setProcessChannelMode(QProcess::SeparateChannels);
  auto settled = std::make_shared<bool>(false);
  auto conclude = [process, settled, done = std::move(done)](bool ok) {
    if (*settled)
      return;
    *settled = true;
    const QByteArray output = process->readAllStandardOutput();
    process->deleteLater();
    done(ok, output);
  };
  QObject::connect(process, &QProcess::finished, context,
                   [conclude](int code, QProcess::ExitStatus status) {
                     conclude(code == 0 && status == QProcess::NormalExit);
                   });
  QObject::connect(process, &QProcess::errorOccurred, context,
                   [conclude] { conclude(false); });
  process->start(program, arguments);
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
 * Signals `pid` without the pid-reuse window between checking what it is and
 * signalling it. A pidfd refers to the process itself, so `verify` runs on a
 * process that cannot be replaced underneath it. Falls back to kill() where
 * the syscalls are unavailable, which reopens only the window that always
 * existed there.
 */
bool signalVerifiedProcess(qint64 pid, int signalNumber,
                           const std::function<bool(qint64)> &verify) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  const int pidfd =
      static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0));
  if (pidfd >= 0) {
    // The handle is open, so the pid cannot now be reused by something else;
    // whatever /proc says about it is still true when the signal lands.
    const bool ours = verify(pid);
    const bool sent =
        ours && ::syscall(SYS_pidfd_send_signal, pidfd, signalNumber,
                          nullptr, 0) == 0;
    ::close(pidfd);
    return sent;
  }
#endif
  return verify(pid) && ::kill(static_cast<pid_t>(pid), signalNumber) == 0;
}

/**
 * Pids of gpu-screen-recorder processes this user already has running.
 *
 * Read-only on purpose: the plan is explicit that a recorder we did not
 * spawn is never signalled, paused, adopted, or stopped. Knowing it is there
 * is enough, because the stock Omarchy recorder controls encoders by broad
 * discovery and could otherwise pause or stop ours.
 */
QVector<qint64> runningEncoders() {
  // /proc/<pid>/comm is capped at 15 characters, so the name arrives
  // truncated and has to be compared that way.
  static const QString encoder =
      QStringLiteral("gpu-screen-recorder").left(15);
  const uint self = ::geteuid();
  QVector<qint64> pids;
  const QStringList entries =
      QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QString &entry : entries) {
    bool numeric = false;
    const qint64 pid = entry.toLongLong(&numeric);
    if (!numeric)
      continue;
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!comm.open(QIODevice::ReadOnly) ||
        QString::fromLatin1(comm.readLine()).trimmed() != encoder)
      continue;
    if (QFileInfo(QStringLiteral("/proc/%1").arg(pid)).ownerId() != self)
      continue;
    pids.append(pid);
  }
  return pids;
}

/**
 * Whether `path` is a handoff this process wrote: a regular file directly
 * inside the private handoff directory, named the way handOffToRecorder()
 * names them. --record-run is hidden, not private -- it can be typed -- and
 * the recorder removes what it reads.
 */
bool isHandoffFile(const QString &path) {
  const QString directory = runtimeRecordDirectory();
  if (directory.isEmpty())
    return false;
  const QFileInfo file(path);
  if (!file.isFile() || file.isSymLink())
    return false;
  static const QRegularExpression name(
      QStringLiteral("\\Atarget-[0-9a-f]{32}\\.json\\z"));
  if (!name.match(file.fileName()).hasMatch())
    return false;
  const QString parent = QFileInfo(directory).canonicalFilePath();
  return !parent.isEmpty() && file.canonicalPath() == parent;
}

/**
 * Whether `pid` is an omasnap right now. Read from /proc immediately before
 * signalling, because a lock file only records what the pid was when it was
 * written and the kernel reuses pids.
 */
bool processIsRecorder(qint64 pid) {
  // Same user, or it is not a recorder of ours whatever it calls itself.
  if (QFileInfo(QStringLiteral("/proc/%1").arg(pid)).ownerId() != ::geteuid())
    return false;
  QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
  if (!comm.open(QIODevice::ReadOnly) ||
      QString::fromLatin1(comm.readLine()).trimmed() !=
          QCoreApplication::applicationName())
    return false;
  // An omasnap that is not a recorder is a screenshot overlay, and stopping
  // a recording must never close one of those.
  QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(pid));
  if (!cmdline.open(QIODevice::ReadOnly))
    return false;
  return cmdline.read(4096).split('\0').contains("--record-run");
}

/** The Studio next to this executable, then one on PATH, else nothing. */
QString studioExecutable() {
  QString sibling = QDir(QCoreApplication::applicationDirPath())
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
  qint64 recorderPid = 0;
  if (!QProcess::startDetached(QCoreApplication::applicationFilePath(),
                               arguments, {}, &recorderPid)) {
    QFile::remove(path);
    error = QStringLiteral("Could not start the recorder");
    return false;
  }

  // The recorder is not recording until it holds the lock, and `omasnap
  // --record --stop` reads that lock to find it. Returning before the child
  // takes it makes an immediate stop -- a double-pressed key, or a script --
  // report that nothing is running and then leave a recording behind.
  const QString runtime = secureRuntimeDirectory();
  if (runtime.isEmpty())
    return true;
  QLockFile lock(QDir(runtime).filePath(QStringLiteral("omasnap.record")));
  lock.setStaleLockTime(0);
  const QDeadlineTimer deadline(kHandoffLockWaitMs);
  while (!deadline.hasExpired()) {
    qint64 holder = 0;
    QString hostname;
    QString application;
    if (lock.getLockInfo(&holder, &hostname, &application) &&
        holder == recorderPid)
      return true;
    // The child can also have failed and said so itself; either way there is
    // nothing more for this process to wait on once it is gone.
    if (!QFileInfo::exists(QStringLiteral("/proc/%1").arg(recorderPid)))
      return true;
    QThread::msleep(kHandoffPollMs);
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
  // process. SIGTERM rather than a socket of our own: the recorder already
  // turns the first one into the same stop-and-save the indicator does.
  if (!signalVerifiedProcess(pid, SIGTERM, processIsRecorder)) {
    error = QStringLiteral("No recording is running");
    return false;
  }
  return true;
}

int runRecorder(const QString &targetPath, const RecordOptions &options,
                PosixSignalNotifier *quitSignals) {
  if (!isHandoffFile(targetPath)) {
    qCritical() << "--record-run takes a recording handoff written by "
                   "omasnap, not an arbitrary path";
    return 2;
  }
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
  const bool freshDirectory = !QFileInfo::exists(directoryPath);
  if (!QDir().mkpath(directoryPath)) {
    const QString message =
        QStringLiteral("Could not create %1").arg(directoryPath);
    qCritical().noquote() << message;
    notifyRecording(message, {});
    return 1;
  }
  if (freshDirectory) {
    // Owner-only when we are the one creating it. An existing directory the
    // user already set up is left as they left it.
    QFile::setPermissions(directoryPath, QFileDevice::ReadOwner |
                                             QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner);
  }
  // Before recovery, not after: an encoder we did not spawn may be writing
  // one of those .part.mkv files right now, and promoting a file that is
  // still being written would be worse than refusing.
  if (const QVector<qint64> others = runningEncoders(); !others.isEmpty()) {
    const QString message =
        QStringLiteral("Another screen recorder is already running (pid %1); "
                       "stop it first")
            .arg(others.first());
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
  config.outputPath =
      uncontendedPath(directory, stem, QStringLiteral(".part.mkv"));
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

  // Create the master before the encoder does, so it is owner-only for the
  // whole recording rather than from the moment it is finished. The encoder
  // truncates it and keeps the mode. Exclusively, so this can only ever be
  // a file we made.
  if (!createExclusiveFile(config.outputPath)) {
    const QString message =
        QStringLiteral("Could not write into %1").arg(directoryPath);
    qCritical().noquote() << message;
    notifyRecording(message, {});
    return 1;
  }

  RecordIndicator indicator;
  RecordSession session(config);

  auto finish = [&indicator](int code) {
    indicator.hide();
    QCoreApplication::exit(code);
  };
  // Set when the user stops before the encoder is up. What follows is an
  // encoder exiting on our own signal, which is not a failure to report.
  bool cancelling = false;

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
    switch (session.state()) {
    case RecordSession::State::Recording:
    case RecordSession::State::Paused:
      indicator.setPhase(RecordIndicator::Phase::Stopping);
      session.stop();
      return;
    case RecordSession::State::Idle:
    case RecordSession::State::Starting:
      // Nothing has been recorded yet, so there is nothing to save. Leave
      // rather than sit on "Starting…": the encoder's parent-death signal
      // takes the child with us.
      cancelling = true;
      finish(1);
      return;
    case RecordSession::State::Stopping:
    case RecordSession::State::Done:
    case RecordSession::State::Failed:
      // Already finishing. A second signal quits regardless, so a stop that
      // hangs still cannot trap the process.
      return;
    }
  };
  QObject::connect(&indicator, &RecordIndicator::stopRequested, &session,
                   requestStop);
  // Logging out, `omasnap --record --stop`, or a plain Ctrl-C finishes the
  // recording rather than abandoning it. A second signal quits regardless.
  if (quitSignals)
    quitSignals->setFirstSignalHandler(requestStop);

  QObject::connect(
      &session, &RecordSession::failed, &indicator,
      [&](const QString &message) {
        if (cancelling) {
          // The encoder exiting on the signal we just sent it.
          finish(1);
          return;
        }
        qCritical().noquote() << message;
        indicator.setPhase(RecordIndicator::Phase::Failed);
        indicator.setMessage(message.left(60));
        // Whatever was captured before it went wrong is left exactly where
        // it is: the encoder is being shut down and may still hold the file
        // open, so renaming it here would race that. It stays a .part.mkv
        // and the next recording promotes it, after checking it can be read.
        const bool kept = QFileInfo(config.outputPath).size() > 0;
        if (!kept)
          QFile::remove(config.outputPath); // The empty one we made.
        notifyRecording(
            kept ? QStringLiteral("Recording failed: %1 · what was captured "
                                  "is kept and restored next time you record")
                       .arg(message)
                 : QStringLiteral("Recording failed: %1").arg(message),
            {});
        QTimer::singleShot(kFailureLingerMs, &indicator, [&] { finish(1); });
      });

  QObject::connect(
      &session, &RecordSession::finished, &indicator,
      [&](const QString &savedPath) {
        // The encoder reports the path it wrote. Only our own output is
        // acted on, because the remux deletes what it reads: a protocol
        // change or a bad reply must not point that at another file.
        if (!savedPath.isEmpty() && savedPath != config.outputPath)
          qWarning().noquote()
              << QStringLiteral("The encoder reported saving a different "
                                "file; using the requested one");
        const QString master = config.outputPath;
        if (QFileInfo(master).size() <= 0) {
          // The file we created before the encoder started, still empty.
          QFile::remove(master);
          notifyRecording(QStringLiteral("Recording produced no file"), {});
          finish(1);
          return;
        }
        restrictToOwner(master);

        const auto keepMaster = [&, master](const QString &why) {
          const QString kept = promoteMatroska(directory, stem, master);
          notifyRecording(why.isEmpty() ? QStringLiteral("Recording saved")
                                        : why,
                          kept);
          finish(0);
        };

        // Matroska survives an interrupted session; MP4 is what everything
        // else opens. Remuxing copies the streams, so it costs a rewrite of
        // the container and nothing of the video.
        const QString ffmpeg =
            QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpeg.isEmpty()) {
          keepMaster({});
          return;
        }
        // Written under a working name and renamed into place only once it
        // has been read back: a crash mid-remux must not leave a corrupt
        // file sitting at the name the notification points at. Reserved the
        // same way the master is, so ffmpeg's -y can only ever truncate a
        // file this process just created.
        const QString mp4 =
            uncontendedPath(directory, stem, QStringLiteral(".mp4"));
        const QString draft = mp4 + QStringLiteral(".part");
        const QString mp4Failed =
            QStringLiteral("Recording saved (could not convert to MP4)");
        if (!createExclusiveFile(draft)) {
          keepMaster(mp4Failed);
          return;
        }

        // Declared here rather than inside the callbacks below: these
        // outlive each asynchronous hop by being copied into the next one,
        // and their references point at this handler's captures, which live
        // as long as the connection does.
        const auto giveUpDraft = [draft, keepMaster, mp4Failed] {
          QFile::remove(draft);
          keepMaster(mp4Failed);
        };
        const auto acceptDraft = [&, master, draft, mp4, giveUpDraft] {
          restrictToOwner(draft);
          if (!QFile::rename(draft, mp4)) {
            giveUpDraft();
            return;
          }
          // Only now is the master redundant.
          QFile::remove(master);
          notifyRecording(QStringLiteral("Recording saved"), mp4);
          finish(0);
        };

        // -map 0 rather than ffmpeg's default stream selection, which keeps
        // one audio stream: recording with --audio and --mic produces two,
        // and the remux would silently drop the microphone.
        runTool(&indicator, ffmpeg,
                {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                 QStringLiteral("error"), QStringLiteral("-y"),
                 QStringLiteral("-i"), master, QStringLiteral("-map"),
                 QStringLiteral("0"), QStringLiteral("-c"),
                 QStringLiteral("copy"), QStringLiteral("-map_metadata"),
                 QStringLiteral("-1"), QStringLiteral("-f"),
                 QStringLiteral("mp4"), QStringLiteral("-movflags"),
                 QStringLiteral("+faststart"), draft},
                [&, draft, acceptDraft, giveUpDraft](bool ok,
                                                     const QByteArray &) {
                  if (!ok || QFileInfo(draft).size() <= 0) {
                    giveUpDraft();
                    return;
                  }
                  const QString probe = findProbeExecutable();
                  if (probe.isEmpty()) {
                    acceptDraft();
                    return;
                  }
                  // Read back asynchronously: the indicator is on screen
                  // saying "Saving…" and has to keep painting.
                  runTool(&indicator, probe, mediaProbeArguments(draft),
                          [acceptDraft, giveUpDraft](
                              bool readable, const QByteArray &output) {
                            if (readable && probeOutputIsPlayable(output))
                              acceptDraft();
                            else
                              giveUpDraft();
                          });
                });
      });

  // start() reports a synchronous failure through `error`; letting it also
  // emit failed would notify twice about the same thing. It also blocks
  // briefly waiting for the child to start, which is why it runs before the
  // indicator exists rather than freezing it.
  bool started = false;
  {
    const QSignalBlocker quiet(&session);
    started = session.start(error);
  }
  if (!started) {
    qCritical().noquote() << error;
    notifyRecording(QStringLiteral("Recording failed: %1").arg(error), {});
    QFile::remove(config.outputPath); // The empty master we made, unused.
    return 1;
  }
  if (showIndicator(indicator, target.output) == nullptr) {
    // A recording with no visible indicator is exactly what must not happen,
    // so leave. The encoder is already running and holds the master open;
    // the session destructor stops it, and whatever it wrote is left for the
    // next launch to recover rather than deleted out from under it.
    qCritical() << "Could not create the recording indicator layer";
    return 1;
  }

  QTimer clock;
  clock.setInterval(kClockIntervalMs);
  QObject::connect(&clock, &QTimer::timeout, &indicator,
                   [&] { indicator.setElapsed(session.elapsedMs()); });
  clock.start();

  return QApplication::exec();
}
