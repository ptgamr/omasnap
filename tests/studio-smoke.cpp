/** @fileoverview Exercises the Studio without a compositor: the export
 *  command it builds, where it writes, and the trim timeline's arithmetic
 *  and drag behaviour. */
#include "studio.hpp"
#include "studio-preview.hpp"
#include "zoom-track.hpp"
#include "zoom-track-smoke.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <QPainter>
#include <QTransform>

#include <cmath>
#include <limits>

/** Renders one frame both ways and compares them. Defined at the end. */
[[nodiscard]] bool runZoomExportGoldenChecks(QString &error);
/** Checks the preview surface offscreen. Defined at the end. */
[[nodiscard]] bool runPreviewChecks(QString &error);

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
  // As omasnap-studio does: no desktop theme plugin, so an offscreen run
  // needs no display.
  qputenv("QT_QPA_PLATFORMTHEME", "generic");
  QApplication application(argc, argv);
  QString error;
  const struct {
    const char *name;
    bool (*run)(QString &);
  } checks[] = {{"timecode", runTimecodeChecks},
                {"export command", runExportCommandChecks},
                {"export path", runExportPathChecks},
                {"timeline", runTimelineChecks},
                {"zoom track", runZoomTrackSmoke},
                {"preview", runPreviewChecks},
                {"zoom export agreement", runZoomExportGoldenChecks}};
  for (const auto &check : checks) {
    if (!check.run(error)) {
      qWarning().noquote() << QStringLiteral("studio %1 smoke failed: %2")
                                  .arg(QString::fromLatin1(check.name), error);
      return EXIT_FAILURE;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Preview/export agreement.
//
// The whole reason the zoom lives in one model is that the picture the person
// framed has to be the picture that comes out. This renders the same frame
// both ways -- through zoomSourceRect() the way the preview does, and through
// the ffmpeg command the export builds -- and compares them.
// ---------------------------------------------------------------------------

namespace {

/// Runs a tool to completion; false on anything other than a clean exit.
bool runTool(const QString &program, const QStringList &arguments) {
  QProcess process;
  process.start(program, arguments);
  if (!process.waitForFinished(120000)) {
    process.kill();
    process.waitForFinished(2000);
    return false;
  }
  return process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
}

/// One frame at `seconds`, decoded to an image.
QImage frameAt(const QString &ffmpeg, const QString &path, double seconds,
               const QString &scratch, bool autorotate = true) {
  const QString png =
      QStringLiteral("%1/frame-%2-%3.png")
          .arg(scratch, QString::number(qRound(seconds * 1000)),
               autorotate ? QStringLiteral("r") : QStringLiteral("n"));
  QFile::remove(png);
  QStringList arguments{QStringLiteral("-v"), QStringLiteral("error")};
  if (!autorotate)
    arguments << QStringLiteral("-noautorotate");
  arguments << QStringLiteral("-ss") << QString::number(seconds, 'f', 3)
            << QStringLiteral("-i") << path << QStringLiteral("-frames:v")
            << QStringLiteral("1") << QStringLiteral("-y") << png;
  if (!runTool(ffmpeg, arguments))
    return {};
  return QImage(png);
}

/// The preview's answer: the model's window of the source frame, filled out
/// to the output size. This is the same operation the preview widget paints.
QImage renderThroughModel(const QImage &coded, const ZoomTrack &track,
                          qint64 timeMs, int rotation = 0) {
  // Exactly what StudioPreview does: turn the coded frame the way the
  // container says, then take the model's window of it. Starting from the
  // coded frame rather than an autorotated one is what makes this cover the
  // preview's rotation and not just the export's.
  const QImage source =
      rotation == 0 ? coded
                    : coded.transformed(QTransform().rotate(rotation),
                                        Qt::SmoothTransformation);
  const QRectF window = zoomSourceRect(track, timeMs);
  const QRect pixels(qRound(window.x() * source.width()),
                     qRound(window.y() * source.height()),
                     qRound(window.width() * source.width()),
                     qRound(window.height() * source.height()));
  return source.copy(pixels).scaled(source.size(), Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation);
}

/// Mean absolute per-channel difference, 0..255. Encoding and two different
/// scalers put a floor under this; framing errors put it far above.
double meanDifference(const QImage &a, const QImage &b) {
  if (a.isNull() || b.isNull() || a.size() != b.size())
    return 255.0;
  const QImage left = a.convertToFormat(QImage::Format_RGB888);
  const QImage right = b.convertToFormat(QImage::Format_RGB888);
  double total = 0.0;
  for (int y = 0; y < left.height(); ++y) {
    const uchar *l = left.constScanLine(y);
    const uchar *r = right.constScanLine(y);
    for (int x = 0; x < left.width() * 3; ++x)
      total += std::abs(static_cast<int>(l[x]) - static_cast<int>(r[x]));
  }
  return total / (left.width() * left.height() * 3.0);
}

} // namespace

namespace {

/// One agreement scenario: media generated at `rate`, a track, and where the
/// export starts.
struct GoldenCase {
  const char *name;
  QString rate;          ///< ffmpeg rate spec for the generated source.
  ZoomTrack track;
  qint64 inPointMs;
  /// Source frame numbers to compare at. Frames, not seconds, because the
  /// export moves the camera once per output frame: comparing at a wall-clock
  /// time leaves a one-frame ambiguity, which during a fast ramp is a large
  /// difference that says nothing about whether the two agree.
  QVector<int> frames;
  /// Counter-clockwise display rotation to stamp on the generated file, so
  /// the portrait-phone path is exercised rather than assumed.
  int displayRotation = 0;
};

ZoomCue makeCue(quint64 id, qint64 startMs, qint64 endMs, QPointF target,
                qreal scale, qint64 ease = 400) {
  ZoomCue cue;
  cue.id = id;
  cue.startMs = startMs;
  cue.endMs = endMs;
  cue.easeInMs = ease;
  cue.easeOutMs = ease;
  cue.target = target;
  cue.scale = scale;
  return cue;
}

/// Renders one case both ways and returns the worst disagreement, or -1 on a
/// setup failure with `error` set.
double compareCase(const GoldenCase &scenario, const QString &ffmpeg,
                   const QString &scratch, QString &error) {
  QString source =
      QDir(scratch).filePath(QStringLiteral("%1-src.mp4").arg(scenario.name));
  // Every frame a keyframe, so sampling a time lands on the frame the model
  // was asked about rather than the nearest keyframe before it.
  if (!runTool(ffmpeg,
               {QStringLiteral("-v"), QStringLiteral("error"),
                QStringLiteral("-y"), QStringLiteral("-f"),
                QStringLiteral("lavfi"), QStringLiteral("-i"),
                QStringLiteral("testsrc2=size=640x360:rate=%1").arg(scenario.rate),
                QStringLiteral("-t"), QStringLiteral("10"),
                QStringLiteral("-c:v"), QStringLiteral("libx264"),
                QStringLiteral("-g"), QStringLiteral("1"),
                QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), source})) {
    error = QStringLiteral("could not generate the %1 source").arg(scenario.name);
    return -1.0;
  }
  if (scenario.displayRotation != 0) {
    // A display matrix, the way a phone writes one: the pixels stay
    // landscape and the container says which way up they go.
    const QString rotated =
        QDir(scratch).filePath(QStringLiteral("%1-rot.mp4").arg(scenario.name));
    if (!runTool(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
                          QStringLiteral("-y"),
                          QStringLiteral("-display_rotation"),
                          QString::number(scenario.displayRotation),
                          QStringLiteral("-i"), source, QStringLiteral("-c"),
                          QStringLiteral("copy"), rotated})) {
      error = QStringLiteral("could not rotate the %1 source")
                  .arg(scenario.name);
      return -1.0;
    }
    source = rotated;
  }
  const StudioSource media = probeStudioSource(source);
  if (!media.usable()) {
    error = QStringLiteral("could not probe the %1 source").arg(scenario.name);
    return -1.0;
  }
  const QString exported =
      QDir(scratch).filePath(QStringLiteral("%1-out.mp4").arg(scenario.name));
  const QStringList arguments =
      studioExportArguments(source, exported, scenario.inPointMs, 10000,
                            scenario.track, media);
  if (arguments.isEmpty() || !runTool(ffmpeg, arguments)) {
    error = QStringLiteral("the %1 export failed").arg(scenario.name);
    return -1.0;
  }

  // One frame's duration, from the rate the file actually states.
  const double frameSeconds = static_cast<double>(media.fpsDenominator) /
                              media.fpsNumerator;
  // Which output frame the export's first one is, so a trimmed export's
  // frames can be addressed in source terms.
  const int inPointFrames =
      static_cast<int>(std::llround(scenario.inPointMs / 1000.0 / frameSeconds));

  double worst = 0.0;
  for (const int frame : scenario.frames) {
    // Half a frame *before* its timestamp: -ss seeks to the first frame at
    // or after the time asked for, so landing squarely on frame k means
    // asking for something between frame k-1 and k. The model is then asked
    // about frame k's own presentation time, which is what the export's
    // `on/fps` evaluates to for that frame.
    const double sourceAt = qMax(0.0, (frame - 0.5) * frameSeconds);
    const double exportAt =
        qMax(0.0, (frame - inPointFrames - 0.5) * frameSeconds);
    const auto timeMs = static_cast<qint64>(std::llround(frame * frameSeconds *
                                                         1000.0));
    const QImage sourceFrame =
        frameAt(ffmpeg, source, sourceAt, scratch, /*autorotate=*/false);
    const QImage exportFrame = frameAt(ffmpeg, exported, exportAt, scratch);
    if (sourceFrame.isNull() || exportFrame.isNull()) {
      error = QStringLiteral("could not decode %1 at frame %2")
                  .arg(QString::fromLatin1(scenario.name))
                  .arg(frame);
      return -1.0;
    }
    const double difference = meanDifference(
        renderThroughModel(sourceFrame, scenario.track, timeMs, media.rotation),
        exportFrame);
    if (difference > worst)
      worst = difference;
    if (difference > 14.0) {
      error = QStringLiteral("%1: preview and export disagree at frame %2 "
                             "(%3 s, mean difference %4/255)")
                  .arg(QString::fromLatin1(scenario.name))
                  .arg(frame)
                  .arg(frame * frameSeconds, 0, 'f', 3)
                  .arg(difference, 0, 'f', 1);
      return -1.0;
    }
  }
  return worst;
}

} // namespace

bool runZoomExportGoldenChecks(QString &error) {
  const QString ffmpeg =
      QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  const QString ffprobe =
      QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
  if (ffmpeg.isEmpty() || ffprobe.isEmpty()) {
    qInfo("studio smoke: ffmpeg/ffprobe absent; skipping export agreement");
    return true;
  }
  QTemporaryDir scratch;
  if (!scratch.isValid()) {
    error = QStringLiteral("could not create a temporary directory");
    return false;
  }

  QVector<GoldenCase> cases;
  // The plain case: one cue, integer rate, no trim.
  cases.push_back({"single",
                   QStringLiteral("30"),
                   ZoomTrack{{makeCue(1, 1000, 3000, {0.25, 0.75}, 2.0)}},
                   0,
                   {15, 36, 60, 87, 105}});
  // A cue starting at zero, and another beginning exactly where it ends.
  cases.push_back({"adjacent",
                   QStringLiteral("30"),
                   ZoomTrack{{makeCue(1, 0, 2000, {0.2, 0.2}, 2.0),
                              makeCue(2, 2000, 4000, {0.8, 0.8}, 3.0)}},
                   0,
                   {3, 30, 60, 90, 135}});
  // Overlapping cues, which sortedCues() resolves before either side reads
  // them. Preview and export used to pick different centres here.
  cases.push_back({"overlap",
                   QStringLiteral("30"),
                   ZoomTrack{{makeCue(1, 500, 4000, {0.25, 0.25}, 3.0),
                              makeCue(2, 2000, 5000, {0.75, 0.75}, 2.0)}},
                   0,
                   {45, 75, 105, 135}});
  // A cue shorter than its own ramps, where the two used to round the split
  // differently.
  cases.push_back({"tight",
                   QStringLiteral("30"),
                   ZoomTrack{{makeCue(1, 1000, 1300, {0.3, 0.6}, 2.5, 400)}},
                   0,
                   {32, 35, 38}});
  // A rate that is not an integer. Rounding it to 30 drifts the camera
  // against the picture, so the samples reach well into the clip.
  cases.push_back({"ntsc",
                   QStringLiteral("30000/1001"),
                   ZoomTrack{{makeCue(1, 1000, 9000, {0.7, 0.3}, 2.0)}},
                   0,
                   {45, 120, 240, 264}});
  // A trimmed export: cue times are absolute, so the offset has to reach the
  // expressions.
  cases.push_back({"trimmed",
                   QStringLiteral("30"),
                   ZoomTrack{{makeCue(1, 4000, 7000, {0.25, 0.75}, 2.5)}},
                   3000,
                   {135, 165, 195}});
  // A portrait recording: the pixels are landscape and a display matrix says
  // which way up. ffmpeg rotates before the zoom filter, so the preview has
  // to rotate too or a normalized target lands on a different axis.
  cases.push_back({"rotated",
                   QStringLiteral("30"),
                   ZoomTrack{{makeCue(1, 1000, 5000, {0.3, 0.8}, 2.0)}},
                   0,
                   {45, 90, 135},
                   90});

  double worst = 0.0;
  for (const GoldenCase &scenario : cases) {
    const double difference = compareCase(scenario, ffmpeg, scratch.path(), error);
    if (difference < 0.0)
      return false;
    worst = qMax(worst, difference);
  }

  // The comparison has to be capable of failing: framing the same frame a
  // second off should score far worse than the agreement above.
  const GoldenCase &first = cases.first();
  const QString source = QDir(scratch.path()).filePath(
      QStringLiteral("%1-src.mp4").arg(first.name));
  const QString exported = QDir(scratch.path()).filePath(
      QStringLiteral("%1-out.mp4").arg(first.name));
  const QImage sourceFrame =
      frameAt(ffmpeg, source, 2.0167, scratch.path(), /*autorotate=*/false);
  const QImage exportFrame = frameAt(ffmpeg, exported, 2.0167, scratch.path());
  const double wrong = meanDifference(
      renderThroughModel(sourceFrame, first.track, 500), exportFrame);
  if (!check(wrong > worst * 3.0, error,
             QStringLiteral("the frame comparison cannot tell a wrong "
                            "framing (%1) from a right one (%2)")
                 .arg(wrong, 0, 'f', 1)
                 .arg(worst, 0, 'f', 1)))
    return false;
  qInfo("studio smoke: preview and export agree within %.1f/255 across %lld "
        "scenarios", worst, static_cast<long long>(cases.size()));
  return true;
}


// ---------------------------------------------------------------------------
// The preview surface: what it shows, and what a click on it means.
// ---------------------------------------------------------------------------

namespace {

/// Four flat quadrants, so "which part of the source is on camera" is a
/// question about colour rather than about pixel arithmetic.
QImage quadrantFrame() {
  QImage frame(640, 360, QImage::Format_RGB32);
  QPainter painter(&frame);
  painter.fillRect(QRect(0, 0, 320, 180), QColor(220, 40, 40));    // top-left
  painter.fillRect(QRect(320, 0, 320, 180), QColor(40, 200, 40));  // top-right
  painter.fillRect(QRect(0, 180, 320, 180), QColor(40, 60, 220));  // bottom-left
  painter.fillRect(QRect(320, 180, 320, 180), QColor(230, 200, 40)); // bottom-right
  return frame;
}

/// The quadrant colour nearest `sample`, as a name, for readable failures.
QString nearestQuadrant(const QColor &sample) {
  const QVector<QPair<QString, QColor>> named{
      {QStringLiteral("top-left"), QColor(220, 40, 40)},
      {QStringLiteral("top-right"), QColor(40, 200, 40)},
      {QStringLiteral("bottom-left"), QColor(40, 60, 220)},
      {QStringLiteral("bottom-right"), QColor(230, 200, 40)}};
  QString best;
  int bestDistance = std::numeric_limits<int>::max();
  for (const auto &[name, colour] : named) {
    const int distance = std::abs(sample.red() - colour.red()) +
                         std::abs(sample.green() - colour.green()) +
                         std::abs(sample.blue() - colour.blue());
    if (distance < bestDistance) {
      bestDistance = distance;
      best = name;
    }
  }
  return best;
}

} // namespace

bool runPreviewChecks(QString &error) {
  StudioPreview preview;
  // 16:9, so the frame fills the widget and there is no letterbox to dodge.
  preview.resize(320, 180);
  preview.setFrame(quadrantFrame());
  preview.show();

  ZoomTrack track;
  preview.setTrack(&track);
  preview.setPosition(0);

  // Resting: the whole frame is on camera, so each corner of the widget
  // shows its own quadrant.
  {
    const QImage shot = preview.grab().toImage();
    const QVector<QPair<QPoint, QString>> corners{
        {{40, 30}, QStringLiteral("top-left")},
        {{280, 30}, QStringLiteral("top-right")},
        {{40, 150}, QStringLiteral("bottom-left")},
        {{280, 150}, QStringLiteral("bottom-right")}};
    for (const auto &[point, expected] : corners) {
      const QString seen = nearestQuadrant(shot.pixelColor(point));
      if (seen != expected) {
        error = QStringLiteral("resting preview shows %1 where %2 belongs")
                    .arg(seen, expected);
        return false;
      }
    }
  }

  // Zoomed into the bottom-left quadrant: the whole widget is that colour,
  // which is the model's window being honoured rather than the frame being
  // drawn whole.
  ZoomCue cue;
  cue.id = 1;
  cue.startMs = 0;
  cue.endMs = 4000;
  cue.easeInMs = 100;
  cue.easeOutMs = 100;
  cue.target = {0.25, 0.75};
  cue.scale = 2.0;
  track.cues = {cue};
  preview.setTrack(&track);
  preview.setPosition(2000);
  {
    const QImage shot = preview.grab().toImage();
    for (const QPoint point : {QPoint(20, 20), QPoint(300, 20), QPoint(20, 160),
                               QPoint(300, 160), QPoint(160, 90)}) {
      const QString seen = nearestQuadrant(shot.pixelColor(point));
      if (seen != QStringLiteral("bottom-left")) {
        error = QStringLiteral("zoomed preview shows %1 at %2,%3; the whole "
                               "view should be the targeted quadrant")
                    .arg(seen)
                    .arg(point.x())
                    .arg(point.y());
        return false;
      }
    }
  }

  // A click reports a point on the *source*, not on the visible window: while
  // zoomed into the bottom-left quadrant, the middle of the widget is the
  // middle of that quadrant, not the middle of the frame.
  {
    QSignalSpy picked(&preview, &StudioPreview::targetPicked);
    QTest::mouseClick(&preview, Qt::LeftButton, {}, QPoint(160, 90));
    if (picked.count() != 1) {
      error = QStringLiteral("clicking the preview reported no target");
      return false;
    }
    const QPointF target = picked.at(0).at(0).toPointF();
    if (std::abs(target.x() - 0.25) > 0.03 ||
        std::abs(target.y() - 0.75) > 0.03) {
      error = QStringLiteral("a click while zoomed reported %1,%2 instead of "
                             "the point under the pointer (0.25,0.75)")
                  .arg(target.x(), 0, 'f', 3)
                  .arg(target.y(), 0, 'f', 3);
      return false;
    }
  }

  // A display rotation is applied before anything else, because ffmpeg
  // rotates a phone recording before the zoom filter sees it. The widget's
  // angle is clockwise, so 90 carries the original top-left quadrant to the
  // top-right.
  {
    track.cues.clear();
    StudioPreview portrait;
    portrait.setRotation(90);
    portrait.setFrame(quadrantFrame());
    portrait.setTrack(&track);
    // The rotated frame's shape, and above the widget's minimum width: a
    // narrower widget is clamped, and the frame then letterboxes inside it.
    portrait.resize(360, 640);
    portrait.show();
    const QImage shot = portrait.grab().toImage();
    const QVector<QPair<QPoint, QString>> corners{
        {{60, 60}, QStringLiteral("bottom-left")},
        {{300, 60}, QStringLiteral("top-left")},
        {{60, 580}, QStringLiteral("bottom-right")},
        {{300, 580}, QStringLiteral("top-right")}};
    for (const auto &[point, expected] : corners) {
      const QString seen = nearestQuadrant(shot.pixelColor(point));
      if (seen != expected) {
        error = QStringLiteral("a 90-degree preview shows %1 at %2,%3 where "
                               "%4 belongs")
                    .arg(seen)
                    .arg(point.x())
                    .arg(point.y())
                    .arg(expected);
        return false;
      }
    }
  }

  // And unzoomed, a click maps straight through to the frame.
  {
    track.cues.clear();
    preview.setTrack(&track);
    preview.setPosition(0);
    QSignalSpy picked(&preview, &StudioPreview::targetPicked);
    QTest::mouseClick(&preview, Qt::LeftButton, {}, QPoint(240, 45));
    if (picked.count() != 1) {
      error = QStringLiteral("clicking the resting preview reported no target");
      return false;
    }
    const QPointF target = picked.at(0).at(0).toPointF();
    if (std::abs(target.x() - 0.75) > 0.02 ||
        std::abs(target.y() - 0.25) > 0.02) {
      error = QStringLiteral("a click at rest reported %1,%2 instead of "
                             "0.75,0.25")
                  .arg(target.x(), 0, 'f', 3)
                  .arg(target.y(), 0, 'f', 3);
      return false;
    }
  }
  return true;
}
