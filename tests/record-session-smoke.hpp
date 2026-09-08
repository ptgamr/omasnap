/** @fileoverview Declares the recorder command-shape smoke test. */
#pragma once

#include <QString>

/** Environment variable that turns this executable into a fake encoder. Its
 *  value picks the behaviour: `normal`, `slow-pause`, or `no-socket`. */
inline constexpr char kFakeRecorderVariable[] = "OMASNAP_SMOKE_FAKE_RECORDER";

/** Checks the command shape RecordSession builds. */
[[nodiscard]] bool runRecordSessionSmoke(QString &error);
/** Drives a real RecordSession against the fake encoder below. */
[[nodiscard]] bool runRecordSessionLifecycleSmoke(QString &error);
/** Speaks gpu-screen-recorder's control protocol, badly and on purpose. */
[[nodiscard]] int runFakeRecorder(int argc, char **argv);
