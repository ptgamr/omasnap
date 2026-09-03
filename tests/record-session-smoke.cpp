/** @fileoverview Tests the gpu-screen-recorder command shape: an argument
 *  vector with no shell fragments in it, the right source for each target
 *  kind, audio only when it was asked for, and nothing at all for a config
 *  that could not produce a recording. */
#include "record-session-smoke.hpp"

#include "capture.hpp"
#include "record-session.hpp"

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
