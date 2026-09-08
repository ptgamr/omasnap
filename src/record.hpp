/** @fileoverview Declares the recording entry points: handing a picked target
 *  off to a recorder process, and being that process. */
#pragma once

#include <QString>

class PosixSignalNotifier;
struct RecordTarget;

/** What the user asked for beyond the rectangle itself. */
struct RecordOptions {
  bool systemAudio = false;
  bool microphone = false;
  int fps = 60;
};

/**
 * Writes `target` to a private file and starts a detached recorder on it.
 *
 * The selector cannot become the recorder in place: it holds the screenshot
 * instance lock, and the next screenshot would then end the recording. So the
 * handoff is a new process, which takes a recording lock of its own and lets
 * screenshots carry on as usual alongside.
 */
[[nodiscard]] bool handOffToRecorder(const RecordTarget &target,
                                     const RecordOptions &options,
                                     QString &error);

/**
 * Runs the recorder: reads (and removes) the target file, takes the recording
 * lock, owns the encoder, shows the indicator, and on stop promotes the
 * master and notifies with a link into the Studio. Returns a process exit
 * code.
 *
 * `quitSignals`, when given, is redirected so the first SIGINT/SIGTERM finalizes
 * the recording instead of dropping it: logging out or `omasnap --record
 * --stop` then leaves a finished file, not a half-written one.
 */
[[nodiscard]] int runRecorder(const QString &targetPath,
                              const RecordOptions &options,
                              PosixSignalNotifier *quitSignals = nullptr);

/**
 * Asks the running recorder to stop and save. False (with `error`) means
 * there was nothing recording.
 */
[[nodiscard]] bool stopActiveRecording(QString &error);

/** Directory finished recordings are moved into (`~/Videos/Recordings`). */
[[nodiscard]] QString recordingsDirectory();
