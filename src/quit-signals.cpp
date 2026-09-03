/** @fileoverview The SIGINT/SIGTERM bridge onto the GUI thread. */
#include "quit-signals.hpp"

#include <QCoreApplication>
#include <QSocketNotifier>

#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

PosixSignalNotifier::PosixSignalNotifier(QObject *parent) : QObject(parent) {
  if (::socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                   fds_) != 0)
    return; // Default signal disposition stays in effect.
  signalFd_ = fds_[0];

  struct sigaction sa{};
  sa.sa_handler = [](int) {
    const int savedErrno = errno;
    const char byte = 1;
    const int fd = signalFd_;
    if (fd >= 0)
      static_cast<void>(::write(fd, &byte, sizeof(byte)));
    errno = savedErrno;
  };
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  sigintInstalled_ = ::sigaction(SIGINT, &sa, &previousSigint_) == 0;
  sigtermInstalled_ = ::sigaction(SIGTERM, &sa, &previousSigterm_) == 0;
  if (!sigintInstalled_ && !sigtermInstalled_) {
    closeSockets();
    return;
  }

  notifier_ = new QSocketNotifier(fds_[1], QSocketNotifier::Read, this);
  connect(notifier_, &QSocketNotifier::activated, this, [this] {
    char bytes[32];
    while (::read(fds_[1], bytes, sizeof(bytes)) > 0) {
    }
    if (!signalled_ && firstSignalHandler_) {
      // Stays armed: a second signal still quits, so a handler that hangs
      // cannot make the process unkillable by anything short of SIGKILL.
      signalled_ = true;
      firstSignalHandler_();
      return;
    }
    notifier_->setEnabled(false);
    QCoreApplication::quit();
  });
}

PosixSignalNotifier::~PosixSignalNotifier() {
  if (sigintInstalled_)
    ::sigaction(SIGINT, &previousSigint_, nullptr);
  if (sigtermInstalled_)
    ::sigaction(SIGTERM, &previousSigterm_, nullptr);
  closeSockets();
}

void PosixSignalNotifier::setFirstSignalHandler(std::function<void()> handler) {
  firstSignalHandler_ = std::move(handler);
}

void PosixSignalNotifier::closeSockets() {
  signalFd_ = -1;
  for (int &fd : fds_) {
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }
}
