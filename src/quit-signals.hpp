/** @fileoverview Declares the SIGINT/SIGTERM bridge onto the GUI thread. */
#pragma once

#include <QObject>

#include <csignal>

#include <functional>

class QSocketNotifier;

/**
 * Turns SIGINT and SIGTERM into something the GUI thread can act on: the
 * handler runs between events, not inside a signal handler, so it may touch
 * Qt objects.
 *
 * By default the first signal quits the event loop. A caller with work to
 * finish first -- a recorder that has a file to close -- installs its own
 * handler for the first signal; a second signal quits regardless, so the
 * process stays interruptible even if that work hangs.
 */
class PosixSignalNotifier final : public QObject {
public:
  explicit PosixSignalNotifier(QObject *parent = nullptr);
  ~PosixSignalNotifier() override;

  /** Runs on the GUI thread for the first signal only. */
  void setFirstSignalHandler(std::function<void()> handler);

private:
  void closeSockets();

  static inline int fds_[2]{-1, -1};
  static inline volatile sig_atomic_t signalFd_ = -1;
  std::function<void()> firstSignalHandler_;
  struct sigaction previousSigint_{};
  struct sigaction previousSigterm_{};
  bool sigintInstalled_ = false;
  bool sigtermInstalled_ = false;
  bool signalled_ = false;
  QSocketNotifier *notifier_ = nullptr;
};
