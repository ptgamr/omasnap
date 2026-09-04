/** @fileoverview Declares the owned gpu-screen-recorder session: one exact
 *  child process, one private IPC socket, and the state machine the
 *  indicator reads. */
#pragma once

#include "record-target.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QJsonValue;
class QProcess;
class QTimer;

/** Everything the encoder needs, resolved before anything is spawned. */
struct RecordConfig {
  RecordTarget target;
  int fps = 60;
  bool cursor = true;
  /** Desktop sound. Off unless the user asked for it. */
  bool systemAudio = false;
  bool microphone = false;
  /** In-progress master; promoted to its final name once GSR has exited. */
  QString outputPath;
  /** Unique per session, and short: a unix socket path is capped at 108
   *  bytes, which a project directory under $HOME blows through. */
  QString ipcSocketPath;
};

/**
 * The exact argument vector for `config`. Pure, so the shape of the command
 * is testable, and an argument vector rather than a string so nothing here
 * can ever reach a shell. Empty when the target has no usable geometry.
 */
[[nodiscard]] QStringList recordSessionArguments(const RecordConfig &config);

/**
 * Longest usable control-socket path in bytes. `sockaddr_un` caps it, and
 * going over it fails at bind() inside the encoder with nothing useful said
 * about why.
 */
inline constexpr int kMaxControlSocketPathBytes = 107;

/** Absolute path of the gpu-screen-recorder to run, or empty when it is not
 *  installed. */
[[nodiscard]] QString findRecorderExecutable();

/**
 * One recording. Owns exactly one gpu-screen-recorder child: it is stopped
 * over its own private socket by request id, never by scanning for processes
 * that look like recorders, so a second recorder running on this desktop is
 * left strictly alone.
 *
 * The child also carries a parent-death signal, so killing this process
 * asks the encoder to shut down rather than leaving an invisible orphan
 * recording the screen.
 */
class RecordSession final : public QObject {
  Q_OBJECT
public:
  enum class State { Idle, Starting, Recording, Paused, Stopping, Done, Failed };

  explicit RecordSession(RecordConfig config, QObject *parent = nullptr);
  ~RecordSession() override;

  /** Spawns the encoder. False (with `error`) means nothing was started. */
  [[nodiscard]] bool start(QString &error);
  void setPaused(bool paused);
  /** Asks the encoder to stop and save; `finished` follows, or `failed`. */
  void stop();

  [[nodiscard]] State state() const { return state_; }
  [[nodiscard]] bool paused() const { return state_ == State::Paused; }
  /** Recorded time, excluding anything spent paused. */
  [[nodiscard]] qint64 elapsedMs() const;
  [[nodiscard]] const RecordConfig &config() const { return config_; }

signals:
  /** The encoder is up and its control socket answers. */
  void recording();
  void pausedChanged(bool paused);
  /** `savedPath` is what the encoder reported writing. */
  void finished(const QString &savedPath);
  void failed(const QString &message);

private:
  void pollForReadiness();
  void probeReadiness();
  void sendCommand(const QString &name, const QJsonValue &data,
                   std::function<void(bool ok, const QString &payload)> reply);
  /// Ends the encoder if it is still running `milliseconds` from now and the
  /// session is still stopping. Never blocks the GUI thread waiting for it.
  void armExitWatchdog(int milliseconds);
  void endChildProcess();
  void fail(const QString &message);
  void handleProcessFinished(int exitCode, int exitStatus);

  RecordConfig config_;
  QProcess *process_ = nullptr;
  QTimer *readyTimer_ = nullptr;
  QElapsedTimer clock_;
  qint64 pausedTotalMs_ = 0;
  qint64 pauseStartedMs_ = 0;
  int nextRequestId_ = 1;
  int readyAttempts_ = 0;
  bool probeInFlight_ = false;
  /// One pause request at a time. Without this, rapid clicks put several
  /// idempotent requests in flight and their replies apply out of order.
  bool pauseInFlight_ = false;
  State state_ = State::Idle;
  QString savedPath_;
  QString stderrTail_;
};
