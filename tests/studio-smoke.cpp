/** @fileoverview Exercises the Studio without a compositor: the export
 *  command it builds, where it writes, and the trim timeline's arithmetic
 *  and drag behaviour. */
#include "studio.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

bool check(bool condition, QString &error, const QString &message) {
  if (condition)
    return true;
  error = message;
  return false;
}

QString valueAfter(const QStringList &arguments, const QString &flag) {
  const qsizetype index = arguments.indexOf(flag);
  if (index < 0 || index + 1 >= arguments.size())
    return {};
  return arguments.at(index + 1);
}

bool runTimecodeChecks(QString &error) {
  return check(studioTimecode(0) == QStringLiteral("00:00:00.000") &&
                   studioTimecode(1500) == QStringLiteral("00:00:01.500") &&
                   studioTimecode(3723456) ==
                       QStringLiteral("01:02:03.456") &&
                   studioTimecode(-10) == QStringLiteral("00:00:00.000"),
               error, QStringLiteral("ffmpeg timecode is wrong")) &&
         check(studioClock(0) == QStringLiteral("0:00") &&
                   studioClock(65999) == QStringLiteral("1:05") &&
                   studioClock(3600000) == QStringLiteral("60:00"),
               error, QStringLiteral("readable clock is wrong"));
}

bool runExportCommandChecks(QString &error) {
  const QStringList arguments = studioExportArguments(
      QStringLiteral("/tmp/rec.mkv"), QStringLiteral("/tmp/rec-trim.mp4"),
      5000, 8500);
  // -ss before -i so ffmpeg seeks instead of decoding the head, and a
  // duration rather than an end time so it cannot be read against the wrong
  // timeline.
  if (!check(arguments.indexOf(QStringLiteral("-ss")) <
                 arguments.indexOf(QStringLiteral("-i")),
             error, QStringLiteral("-ss is not an input option")))
    return false;
  if (!check(valueAfter(arguments, QStringLiteral("-ss")) ==
                     QStringLiteral("00:00:05.000") &&
                 valueAfter(arguments, QStringLiteral("-t")) ==
                     QStringLiteral("00:00:03.500"),
             error, QStringLiteral("trim range is wrong")))
    return false;
  if (!check(valueAfter(arguments, QStringLiteral("-i")) ==
                     QStringLiteral("/tmp/rec.mkv") &&
                 arguments.last() == QStringLiteral("/tmp/rec-trim.mp4"),
             error, QStringLiteral("source or destination is misplaced")))
    return false;
  if (!check(valueAfter(arguments, QStringLiteral("-map_metadata")) ==
                 QStringLiteral("-1"),
             error, QStringLiteral("export keeps the source metadata")))
    return false;
  // Every audio stream, not ffmpeg's default pick of one: a recording made
  // with --audio and --mic has two, and the export must not drop the mic.
  if (!check(arguments.contains(QStringLiteral("0:a?")) &&
                 arguments.contains(QStringLiteral("0:v:0")),
             error, QStringLiteral("export does not map every stream")))
    return false;
  for (const QString &argument : arguments) {
    if (argument.contains(QLatin1Char(';')) ||
        argument.contains(QLatin1Char('|')) ||
        argument.contains(QLatin1Char('`')) ||
        argument.contains(QLatin1Char('$'))) {
      error = QStringLiteral("export argument looks like a shell fragment: %1")
                  .arg(argument);
      return false;
    }
  }
  // An empty or inverted range exports nothing rather than a broken file.
  return check(studioExportArguments(QStringLiteral("/tmp/rec.mkv"),
                                     QStringLiteral("/tmp/out.mp4"), 900, 900)
                       .isEmpty() &&
                   studioExportArguments(QStringLiteral("/tmp/rec.mkv"),
                                         QStringLiteral("/tmp/out.mp4"), 900,
                                         100)
                       .isEmpty() &&
                   studioExportArguments({}, QStringLiteral("/tmp/out.mp4"), 0,
                                         100)
                       .isEmpty(),
               error, QStringLiteral("an unusable range still built a "
                                     "command"));
}

bool runExportPathChecks(QString &error) {
  QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("could not create a temporary directory");
    return false;
  }
  const QString source =
      QDir(directory.path()).filePath(QStringLiteral("recording-1.mkv"));
  const QString first = studioExportPath(source);
  if (!check(first == QDir(directory.path())
                          .filePath(QStringLiteral("recording-1-trim.mp4")),
             error, QStringLiteral("export path is not beside the source")))
    return false;

  // An export never silently replaces an earlier one.
  QFile placeholder(first);
  if (!placeholder.open(QIODevice::WriteOnly)) {
    error = QStringLiteral("could not write the placeholder export");
    return false;
  }
  placeholder.close();
  return check(studioExportPath(source) ==
                   QDir(directory.path())
                       .filePath(QStringLiteral("recording-1-trim-2.mp4")),
               error, QStringLiteral("export overwrote an existing file"));
}

bool runTimelineChecks(QString &error) {
  StudioTimeline timeline;
  timeline.resize(400, 46);
  timeline.show();

  // Before a duration is known there is nothing to drag and nothing to emit.
  QSignalSpy scrubbed(&timeline, &StudioTimeline::scrubbed);
  QTest::mouseClick(&timeline, Qt::LeftButton, {}, QPoint(200, 23));
  if (!check(scrubbed.isEmpty(), error,
             QStringLiteral("scrubbed an empty timeline")))
    return false;

  // A new duration keeps everything: opening a recording is not a trim.
  timeline.setDuration(60000);
  if (!check(timeline.trimIn() == 0 && timeline.trimOut() == 60000, error,
             QStringLiteral("a new duration did not keep the whole "
                            "recording")))
    return false;

  // The track is inset by half a handle at each end, so the midpoint of the
  // widget is the midpoint of the recording.
  QTest::mouseClick(&timeline, Qt::LeftButton, {}, QPoint(200, 23));
  if (!check(scrubbed.count() == 1 &&
                 qAbs(scrubbed.at(0).at(0).toLongLong() - 30000) < 500,
             error, QStringLiteral("scrubbing landed at the wrong time")))
    return false;

  // Dragging the in handle past the out handle would invert the range; it
  // stops short instead.
  QSignalSpy trimmed(&timeline, &StudioTimeline::trimChanged);
  QTest::mousePress(&timeline, Qt::LeftButton, {}, QPoint(8, 23));
  QTest::mouseMove(&timeline, QPoint(600, 23));
  QTest::mouseRelease(&timeline, Qt::LeftButton, {}, QPoint(600, 23));
  if (!check(!trimmed.isEmpty(), error,
             QStringLiteral("dragging the in handle changed nothing")))
    return false;
  if (!check(timeline.trimIn() < timeline.trimOut(), error,
             QStringLiteral("the trim handles crossed")))
    return false;
  if (!check(timeline.trimOut() == 60000, error,
             QStringLiteral("dragging in moved the out point")))
    return false;

  // Out is clamped the same way from the other side.
  timeline.setTrim(0, 60000);
  QTest::mousePress(&timeline, Qt::LeftButton, {}, QPoint(392, 23));
  QTest::mouseMove(&timeline, QPoint(-200, 23));
  QTest::mouseRelease(&timeline, Qt::LeftButton, {}, QPoint(-200, 23));
  if (!check(timeline.trimOut() > timeline.trimIn() &&
                 timeline.trimIn() == 0,
             error, QStringLiteral("dragging out crossed or moved in")))
    return false;

  // setTrim itself normalises rather than trusting its caller.
  timeline.setTrim(40000, 10000);
  if (!check(timeline.trimIn() == 10000 && timeline.trimOut() == 40000, error,
             QStringLiteral("an inverted trim was not normalised")))
    return false;
  timeline.setTrim(-5000, 900000);
  if (!check(timeline.trimIn() == 0 && timeline.trimOut() == 60000, error,
             QStringLiteral("a trim outside the recording was not clamped")))
    return false;

  return true;
}

} // namespace

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  QString error;
  const struct {
    const char *name;
    bool (*run)(QString &);
  } checks[] = {{"timecode", runTimecodeChecks},
                {"export command", runExportCommandChecks},
                {"export path", runExportPathChecks},
                {"timeline", runTimelineChecks}};
  for (const auto &check : checks) {
    if (!check.run(error)) {
      qWarning().noquote() << QStringLiteral("studio %1 smoke failed: %2")
                                  .arg(QString::fromLatin1(check.name), error);
      return EXIT_FAILURE;
    }
  }
  return 0;
}
