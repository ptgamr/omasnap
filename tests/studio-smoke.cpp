/** @fileoverview Exercises the Studio without a compositor: the export
 *  command it builds, where it writes, and the trim timeline's arithmetic
 *  and drag behaviour. */
#include "studio-composition-smoke.hpp"
#include "studio-background-ui-smoke.hpp"
#include "studio-cuts-ui-smoke.hpp"
#include "studio-export.hpp"
#include "studio-playback-smoke.hpp"
#include "studio-playback.hpp"
#include "studio-preview.hpp"
#include "studio-project-smoke.hpp"
#include "studio-scenes-ui-smoke.hpp"
#include "studio-theme-smoke.hpp"
#include "studio-transitions-ui-smoke.hpp"
#include "studio.hpp"
#include "zoom-track-smoke.hpp"
#include "zoom-track.hpp"

#include <QApplication>
#include <cstdio>
#include <QAudioOutput>
#include <QComboBox>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSlider>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QtConcurrentRun>

#include <QPainter>
#include <QTransform>

#include <atomic>
#include <cmath>
#include <limits>

/** Renders one frame both ways and compares them. Defined at the end. */
[[nodiscard]] bool runZoomExportGoldenChecks(QString &error);
/** Checks the preview surface offscreen. Defined at the end. */
[[nodiscard]] bool runPreviewChecks(QString &error);
[[nodiscard]] bool runStudioInteractionChecks(QString &error);
[[nodiscard]] bool runGpuPreviewChecks(QString &error);
[[nodiscard]] bool runDirectionalPreviewChecks(QString &error);

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
                   studioTimecode(3723456) == QStringLiteral("01:02:03.456") &&
                   studioTimecode(-10) == QStringLiteral("00:00:00.000"),
               error, QStringLiteral("ffmpeg timecode is wrong")) &&
         check(studioClock(0) == QStringLiteral("0:00") &&
                   studioClock(65999) == QStringLiteral("1:05") &&
                   studioClock(3600000) == QStringLiteral("60:00"),
               error, QStringLiteral("readable clock is wrong"));
}

bool runExportCommandChecks(QString &error) {
  const QStringList arguments = studioExportArguments(
      QStringLiteral("/tmp/rec.mkv"),
      QStringLiteral("/tmp/rec-omasnap-exported.mp4"), 5000, 8500);
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
                 arguments.last() ==
                     QStringLiteral("/tmp/rec-omasnap-exported.mp4"),
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
  return check(
      studioExportArguments(QStringLiteral("/tmp/rec.mkv"),
                            QStringLiteral("/tmp/out.mp4"), 900, 900)
              .isEmpty() &&
          studioExportArguments(QStringLiteral("/tmp/rec.mkv"),
                                QStringLiteral("/tmp/out.mp4"), 900, 100)
              .isEmpty() &&
          studioExportArguments({}, QStringLiteral("/tmp/out.mp4"), 0, 100)
              .isEmpty(),
      error,
      QStringLiteral("an unusable range still built a "
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
                          .filePath(QStringLiteral(
                              "recording-1-omasnap-exported.mp4")),
             error, QStringLiteral("export path is not beside the source")))
    return false;

  // An export never silently replaces an earlier one.
  QFile placeholder(first);
  if (!placeholder.open(QIODevice::WriteOnly)) {
    error = QStringLiteral("could not write the placeholder export");
    return false;
  }
  placeholder.close();
  return check(
      studioExportPath(source) ==
          QDir(directory.path())
              .filePath(QStringLiteral("recording-1-omasnap-exported-2.mp4")),
      error, QStringLiteral("export overwrote an existing file"));
}

bool runTimelineChecks(QString &error) {
  StudioTimeline timeline;
  timeline.setFixedSize(400, 176);
  timeline.show();

  // Before a duration is known there is nothing to drag and nothing to emit.
  QSignalSpy scrubbed(&timeline, &StudioTimeline::scrubbed);
  QTest::mouseClick(&timeline, Qt::LeftButton, {}, QPoint(224, 58));
  if (!check(scrubbed.isEmpty(), error,
             QStringLiteral("scrubbed an empty timeline")))
    return false;

  // A new duration keeps everything: opening a recording is not a trim.
  timeline.setDuration(60000);
  if (!check(timeline.trimIn() == 0 && timeline.trimOut() == 60000, error,
             QStringLiteral("a new duration did not keep the whole "
                            "recording")))
    return false;

  // The label gutter is 64 px and the trailing inset 16 px.
  QTest::mouseClick(&timeline, Qt::LeftButton, {}, QPoint(224, 58));
  if (!check(scrubbed.count() == 1 &&
                 qAbs(scrubbed.at(0).at(0).toLongLong() - 30000) < 500,
             error, QStringLiteral("scrubbing landed at the wrong time")))
    return false;

  // The outer edges scrub now; there are no separate export trim handles.
  QSignalSpy trimmed(&timeline, &StudioTimeline::trimChanged);
  QTest::mousePress(&timeline, Qt::LeftButton, {}, QPoint(64, 58));
  QTest::mouseMove(&timeline, QPoint(600, 58));
  QTest::mouseRelease(&timeline, Qt::LeftButton, {}, QPoint(600, 58));
  if (!check(trimmed.isEmpty(), error,
             QStringLiteral("timeline edge still changed the export range")))
    return false;
  if (!check(timeline.trimIn() < timeline.trimOut(), error,
             QStringLiteral("the trim handles crossed")))
    return false;
  if (!check(timeline.trimOut() == 60000, error,
             QStringLiteral("dragging in moved the out point")))
    return false;

  // Out is clamped the same way from the other side.
  timeline.setTrim(0, 60000);
  QTest::mousePress(&timeline, Qt::LeftButton, {}, QPoint(384, 58));
  QTest::mouseMove(&timeline, QPoint(-200, 58));
  QTest::mouseRelease(&timeline, Qt::LeftButton, {}, QPoint(-200, 58));
  if (!check(timeline.trimOut() > timeline.trimIn() && timeline.trimIn() == 0,
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

  StudioProject project;
  project.assets = {{1,
                     QStringLiteral("recording.mp4"),
                     {{320, 180}, 30, 1, 0, false, 60000, 0}}};
  project.clips = {{1, 1, 0, 30000, 1.0}, {2, 1, 30000, 60000, 1.0}};
  timeline.setProject(&project);
  qint64 sampleTime = 0;
  for (const QColor &color : {QColor(Qt::red), QColor(Qt::green),
                              QColor(Qt::blue), QColor(Qt::yellow)}) {
    QImage thumbnail(160, 90, QImage::Format_RGB32);
    thumbnail.fill(color);
    timeline.cacheThumbnail({QStringLiteral("recording.mp4"), sampleTime, thumbnail});
    sampleTime += 15000;
  }
  QTest::mouseClick(&timeline, Qt::LeftButton, Qt::ControlModifier, QPoint(130, 58));
  const QImage selected = timeline.grab().toImage();
  const qreal dpr = selected.devicePixelRatio();
  const QColor red = selected.pixelColor(qRound(90 * dpr), qRound(75 * dpr));
  const QColor green = selected.pixelColor(qRound(185 * dpr), qRound(75 * dpr));
  if (!check(timeline.selectedClip() == 1 && red.red() > red.green() + 10 &&
                 green.green() > green.red() + 10,
             error, QStringLiteral("selecting a scene hid its thumbnails")))
    return false;
  // Plain dragging starts away from the playhead, updates while held, and
  // crosses scene boundaries without accidentally rearranging the composition.
  QSignalSpy moved(&timeline, &StudioTimeline::sceneMoveRequested);
  scrubbed.clear();
  QTest::mousePress(&timeline, Qt::LeftButton, {}, QPoint(160, 58));
  QTest::mouseMove(&timeline, QPoint(190, 58), 20);
  QTest::mouseMove(&timeline, QPoint(300, 58), 20);
  if (!check(scrubbed.size() >= 3 && scrubbed.last()[0].toLongLong() > 43000 &&
                 moved.isEmpty(),
             error,
             QStringLiteral("dragging the video lane did not scrub live")))
    return false;
  QTest::mouseRelease(&timeline, Qt::LeftButton, {}, QPoint(330, 58));
  if (!check(qAbs(scrubbed.last()[0].toLongLong() - 49875) < 100 &&
                 moved.isEmpty(),
             error,
             QStringLiteral(
                 "scrub release lost its final position or moved a scene")))
    return false;

  // Per-source caches survive composition edits. Fixed-width tiles repeat
  // while finer samples are pending, rather than stretching or going blank.
  StudioTimeline tiled;
  tiled.setFixedSize(400, 176);
  StudioProject clips = project;
  clips.assets[0].path = QStringLiteral("first.mp4");
  auto secondAsset = clips.assets[0];
  secondAsset.id = 2;
  secondAsset.path = QStringLiteral("second.mp4");
  clips.assets.push_back(secondAsset);
  clips.clips[1].assetId = 2;
  tiled.setProject(&clips);
  tiled.setDuration(60000);
  tiled.show();
  QImage stripes(160, 90, QImage::Format_RGB32);
  stripes.fill(Qt::red);
  { QPainter paint(&stripes); paint.fillRect(QRect(80, 0, 80, 90), Qt::green); }
  QImage blue(160, 90, QImage::Format_RGB32);
  blue.fill(Qt::blue);
  const auto initialRequests = tiled.missingThumbnails();
  for (auto request : initialRequests) {
    request.image = request.path == QStringLiteral("first.mp4") ? stripes : blue;
    tiled.cacheThumbnail(request);
  }
  if (!check(!initialRequests.isEmpty() && tiled.missingThumbnails().isEmpty(), error,
             QStringLiteral("thumbnail cache did not satisfy source-time requests"))) return false;
  const auto pixels = [&] { return tiled.grab().toImage(); };
  const auto sample = [](const QImage &shot, int x) {
    return shot.pixelColor(qRound(x * shot.devicePixelRatio()), qRound(65 * shot.devicePixelRatio()));
  };
  const auto beforeEdit = pixels();
  if (!check(sample(beforeEdit, 84).red() > 240 && sample(beforeEdit, 124).green() > 240 &&
                 sample(beforeEdit, 164).red() > 240 && sample(beforeEdit, 250).blue() > 240,
             error, QStringLiteral("per-clip thumbnail tiles stretched or used another asset"))) return false;
  clips.clips[0].inMs = 5000;
  tiled.setDuration(55000);
  if (!check(sample(pixels(), 84).red() > 240, error,
             QStringLiteral("resizing a clip cleared reusable source thumbnails"))) return false;
  tiled.setFixedWidth(800);
  tiled.setThumbnailViewport(QRectF(0, 0, 400, 176));
  const auto finer = tiled.missingThumbnails();
  const auto zoomed = pixels();
  if (!check(!finer.isEmpty() && finer.size() <= 8 && sample(zoomed, 84).red() > 240 &&
                 sample(zoomed, 124).green() > 240 && sample(zoomed, 164).red() > 240,
             error, QStringLiteral("timeline zoom stretched tiles, blanked cache, or failed to request finer samples"))) return false;
  for (const auto &request : finer)
    tiled.cacheThumbnail(request); // Failed samples must not spin in a retry loop.
  if (!check(tiled.missingThumbnails().isEmpty(), error,
             QStringLiteral("failed thumbnail samples are retried indefinitely"))) return false;

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
                {"Quattro theme", runStudioThemeChecks},
                {"project model", runStudioProjectChecks},
                {"composition export", runStudioCompositionChecks},
                {"export command", runExportCommandChecks},
                {"export path", runExportPathChecks},
                {"timeline", runTimelineChecks},
                {"zoom track", runZoomTrackSmoke},
                {"preview", runPreviewChecks},
                {"keyboard and editing", runStudioInteractionChecks},
                {"GPU video pixels", runGpuPreviewChecks},
                {"directional preview pixels", runDirectionalPreviewChecks},
                {"zoom export agreement", runZoomExportGoldenChecks}};
  for (const auto &check : checks) {
    if (!check.run(error)) {
      // Test failures must remain visible even if desktop Qt logging is disabled.
      std::fprintf(stderr, "studio %s smoke failed: %s\n", check.name, qPrintable(error));
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
  const QImage source = rotation == 0
                            ? coded
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
  QString rate; ///< ffmpeg rate spec for the generated source.
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
  if (!runTool(
          ffmpeg,
          {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"),
           QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
           QStringLiteral("testsrc2=size=640x360:rate=%1").arg(scenario.rate),
           QStringLiteral("-t"), QStringLiteral("10"), QStringLiteral("-c:v"),
           QStringLiteral("libx264"), QStringLiteral("-g"), QStringLiteral("1"),
           QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), source})) {
    error =
        QStringLiteral("could not generate the %1 source").arg(scenario.name);
    return -1.0;
  }
  if (scenario.displayRotation != 0) {
    // A display matrix, the way a phone writes one: the pixels stay
    // landscape and the container says which way up they go.
    const QString rotated =
        QDir(scratch).filePath(QStringLiteral("%1-rot.mp4").arg(scenario.name));
    if (!runTool(ffmpeg,
                 {QStringLiteral("-v"), QStringLiteral("error"),
                  QStringLiteral("-y"), QStringLiteral("-display_rotation"),
                  QString::number(scenario.displayRotation),
                  QStringLiteral("-i"), source, QStringLiteral("-c"),
                  QStringLiteral("copy"), rotated})) {
      error =
          QStringLiteral("could not rotate the %1 source").arg(scenario.name);
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
  const QStringList arguments = studioExportArguments(
      source, exported, scenario.inPointMs, 10000, scenario.track, media);
  if (arguments.isEmpty() || !runTool(ffmpeg, arguments)) {
    error = QStringLiteral("the %1 export failed").arg(scenario.name);
    return -1.0;
  }

  // One frame's duration, from the rate the file actually states.
  const double frameSeconds =
      static_cast<double>(media.fpsDenominator) / media.fpsNumerator;
  // Which output frame the export's first one is, so a trimmed export's
  // frames can be addressed in source terms.
  const int inPointFrames = static_cast<int>(
      std::llround(scenario.inPointMs / 1000.0 / frameSeconds));

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
    const auto timeMs =
        static_cast<qint64>(std::llround(frame * frameSeconds * 1000.0));
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

  // A full track, handed to ffmpeg. The limit that bites is ffmpeg's own
  // expression complexity, not the length of the argument, so counting
  // characters proves nothing about whether an export will run.
  {
    ZoomTrack full;
    for (int index = 0; index < kMaxZoomCues + 5; ++index)
      static_cast<void>(
          addZoomCue(full, index * 200, {0.4, 0.6}, 2.0, 150, 1000000));
    if (full.cues.size() != kMaxZoomCues) {
      error = QStringLiteral("expected a full track of %1 cues, got %2")
                  .arg(kMaxZoomCues)
                  .arg(full.cues.size());
      return false;
    }
    GoldenCase capped{"capped", QStringLiteral("30"), full, 0, {30}, 0};
    if (compareCase(capped, ffmpeg, scratch.path(), error) < 0.0) {
      error = QStringLiteral("a full %1-cue track does not export: %2")
                  .arg(kMaxZoomCues)
                  .arg(error);
      return false;
    }
  }

  double worst = 0.0;
  for (const GoldenCase &scenario : cases) {
    const double difference =
        compareCase(scenario, ffmpeg, scratch.path(), error);
    if (difference < 0.0)
      return false;
    worst = qMax(worst, difference);
  }

  // The comparison has to be capable of failing: framing the same frame a
  // second off should score far worse than the agreement above.
  const GoldenCase &first = cases.first();
  const QString source =
      QDir(scratch.path())
          .filePath(QStringLiteral("%1-src.mp4").arg(first.name));
  const QString exported =
      QDir(scratch.path())
          .filePath(QStringLiteral("%1-out.mp4").arg(first.name));
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
        "scenarios",
        worst, static_cast<long long>(cases.size()));
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
  painter.fillRect(QRect(0, 0, 320, 180), QColor(220, 40, 40));   // top-left
  painter.fillRect(QRect(320, 0, 320, 180), QColor(40, 200, 40)); // top-right
  painter.fillRect(QRect(0, 180, 320, 180), QColor(40, 60, 220)); // bottom-left
  painter.fillRect(QRect(320, 180, 320, 180),
                   QColor(230, 200, 40)); // bottom-right
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

bool runGpuPreviewChecks(QString &error) {
  // The regular smoke stays headless. The same binary on Wayland verifies
  // actual shader output; successful initialization or a high fps alone
  // cannot establish that the correct pixels reached the framebuffer.
  if (QGuiApplication::platformName() != QStringLiteral("wayland"))
    return true;
  StudioVideoSurface surface(nullptr);
  surface.resize(640, 360);
  QString gpuError;
  surface.failed = [&gpuError](const QString &message) { gpuError = message; };
  surface.show();
  if (!check(
          QTest::qWaitFor(
              [&] { return surface.isValid() || !gpuError.isEmpty(); }, 5000) &&
              gpuError.isEmpty(),
          error,
          QStringLiteral("GPU preview could not initialize: %1").arg(gpuError)))
    return false;
  surface.drawn = surface.rect();
  surface.canvas = surface.rect();
  bool overlayCalled = false;
  bool overlayAlignmentValid = true;
  surface.overlay = [&](QPainter &painter) {
    GLint alignment = 0;
    QOpenGLContext::currentContext()->functions()->glGetIntegerv(
        GL_UNPACK_ALIGNMENT, &alignment);
    overlayCalled = true;
    overlayAlignmentValid = overlayAlignmentValid && alignment == 4;
    painter.setPen(Qt::white);
    painter.drawText(QRect(10, 10, 300, 30), Qt::AlignLeft,
                     QStringLiteral("click to aim the zoom"));
  };
  for (const auto pixelFormat :
       {QVideoFrameFormat::Format_YUV420P, QVideoFrameFormat::Format_NV12}) {
    QVideoFrameFormat format(QSize(318, 180), pixelFormat);
    format.setColorSpace(QVideoFrameFormat::ColorSpace_BT601);
    format.setColorRange(QVideoFrameFormat::ColorRange_Video);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly))
      return false;
    for (int plane = 0; plane < frame.planeCount(); ++plane) {
      for (int byte = 0; byte < frame.mappedBytes(plane); ++byte) {
        frame.bits(plane)[byte] =
            static_cast<uchar>(plane == 0 ? 81
                               : pixelFormat == QVideoFrameFormat::Format_NV12
                                   ? (byte % 2 ? 240 : 90)
                               : plane == 1 ? 90
                                            : 240);
      }
    }
    frame.unmap();
    surface.setFrame(prepareStudioVideoFrame(frame));
    const QImage shot = surface.grabFramebuffer();
    if (!check(overlayCalled && overlayAlignmentValid, error,
               QStringLiteral("Video upload leaked row alignment into the text overlay")))
      return false;
    const QColor center = shot.pixelColor(shot.width() / 2, shot.height() / 2);
    if (!check(center.red() > 245 && center.green() < 8 && center.blue() < 8,
               error,
               QStringLiteral("GPU YUV conversion did not produce red pixels")))
      return false;
  }
  surface.setFrame(prepareStudioVideoFrame(QVideoFrame(quadrantFrame())));
  surface.rotation = 90;
  const QImage rotated = surface.grabFramebuffer();
  if (!check(nearestQuadrant(rotated.pixelColor(rotated.width() / 4,
                                                rotated.height() / 4)) ==
                 QStringLiteral("bottom-left"),
             error,
             QStringLiteral(
                 "GPU rotation differs from the source coordinate model")))
    return false;
  surface.rotation = 0;
  surface.source = QRectF(0.5, 0, 0.5, 0.5);
  const QImage crop = surface.grabFramebuffer();
  return check(
      nearestQuadrant(crop.pixelColor(crop.width() / 2, crop.height() / 2)) ==
          QStringLiteral("top-right"),
      error, QStringLiteral("GPU zoom sampled the wrong source quadrant"));
}

bool runDirectionalPreviewChecks(QString &error) {
  StudioPreview preview;
  preview.setFixedSize(320, 320);
  preview.setCanvasSize({320, 320});
  StudioStyle style;
  style.padding = 10;
  style.radius = 24;
  preview.setStyle(style);
  preview.setPickable(false);
  preview.show();
  QImage outgoing(320, 320, QImage::Format_RGBA8888);
  QImage incoming(160, 320, QImage::Format_RGBA8888);
  for (int y = 0; y < 320; ++y)
    for (int x = 0; x < 320; ++x)
      outgoing.setPixelColor(x, y, QColor(60 + x / 3, 20 + y / 3, 10));
  for (int y = 0; y < incoming.height(); ++y)
    for (int x = 0; x < incoming.width(); ++x)
      incoming.setPixelColor(x, y, QColor(10, 60 + x / 3, 100 + y / 3));
  preview.setVideoFrame(0, QVideoFrame(outgoing), 0);
  preview.setVideoFrame(1, QVideoFrame(incoming), 90);
  incoming = incoming.transformed(QTransform().rotate(90));
  if (!check(QTest::qWaitFor(
                 [&] {
                   return preview.videoSlotReady(0) &&
                          preview.videoSlotReady(1);
                 },
                 5000),
             error, QStringLiteral("Directional preview frames unavailable")))
    return false;
  ZoomTrack track;
  preview.setTrack(&track);
  const StudioTransitionKind kinds[] = {
      StudioTransitionKind::WipeLeft,  StudioTransitionKind::WipeRight,
      StudioTransitionKind::WipeUp,    StudioTransitionKind::WipeDown,
      StudioTransitionKind::SlideLeft, StudioTransitionKind::SlideRight,
      StudioTransitionKind::SlideUp,   StudioTransitionKind::SlideDown};
  for (bool zoomed : {false, true}) {
    if (zoomed) {
      ZoomCue cue;
      cue.id = 1;
      cue.startMs = 0;
      cue.endMs = 4000;
      cue.easeInMs = cue.easeOutMs = 100;
      cue.target = {0.5, 0.5};
      cue.scale = 2;
      track.cues = {cue};
    }
    for (int kind = 0; kind < 8; ++kind)
      for (double progress : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        preview.setComposition(0, 1, kinds[kind], progress, 2000);
        const QImage shot = preview.grab().toImage();
        const qreal dpr = shot.devicePixelRatio();
        const auto at = [&](double x, double y) {
          return shot.pixelColor(qFloor(x * dpr), qFloor(y * dpr));
        };
        if (!check(
                at(16, 160) == style.color(), error,
                QStringLiteral("Directional preview modified canvas padding")))
          return false;
        for (double x : {0.12, 0.37, 0.62, 0.87})
          for (double y : {0.12, 0.37, 0.62, 0.87}) {
            const int px = qFloor(32 + x * 256), py = qFloor(32 + y * 256);
            QPointF source((px + 0.5 - 32) / 256, (py + 0.5 - 32) / 256);
            if (zoomed)
              source = QPointF(0.25, 0.25) + source * 0.5;
            const bool vertical = kind % 4 >= 2;
            const bool negative = kind % 2 == 0;
            const double axis = vertical ? source.y() : source.x();
            const bool isIncoming =
                negative ? axis >= 1 - progress : axis < progress;
            if (kind >= 4) {
              const double offset =
                  (negative ? -1 : 1) * (isIncoming ? progress - 1 : progress);
              if (vertical)
                source.ry() -= offset;
              else
                source.rx() -= offset;
            }
            const QImage &image = isIncoming ? incoming : outgoing;
            // Fit the rotated portrait into the square canonical canvas
            // before movement and global zoom: its letterbox remains black.
            if (isIncoming)
              source.setY((source.y() - 0.25) * 2);
            const QColor expected =
                source.y() < 0 || source.y() >= 1
                    ? QColor(Qt::black)
                    : image.pixelColor(
                          qBound(0, qFloor(source.x() * image.width()),
                                 image.width() - 1),
                          qBound(0, qFloor(source.y() * image.height()),
                                 image.height() - 1));
            const QColor actual = at(px, py);
            if (!check(qAbs(actual.red() - expected.red()) <= 3 &&
                           qAbs(actual.green() - expected.green()) <= 3 &&
                           qAbs(actual.blue() - expected.blue()) <= 3,
                       error,
                       QStringLiteral("%1 preview at %2,%3 progress %4 zoom "
                                      "%5: got %6 expected %7")
                           .arg(studioTransitionName(kinds[kind]))
                           .arg(px)
                           .arg(py)
                           .arg(progress)
                           .arg(zoomed)
                           .arg(actual.name())
                           .arg(expected.name())))
              return false;
          }
      }
  }
  return true;
}

bool runStudioExportUiChecks(const QString &fixture, QString &error) {
  QTemporaryDir scratch;
  const QString source =
      scratch.filePath(QStringLiteral("recording with spaces.mp4"));
  if (!QFile::copy(fixture, source))
    return check(false, error, QStringLiteral("could not copy export fixture"));
  StudioProject project;
  project.assets = {{1, source, probeStudioSource(source)}};
  project.clips = {{1, 1, 0, 6000, 1.0}};
  project.canvas = {320, 180};
  project.trimInMs = 1000;
  project.trimOutMs = 3000;
  const StudioProject original = project;
  QWidget parent;
  parent.setStyleSheet(StudioChrome{}.styleSheet());
  parent.show();
  const QString destination = studioExportPath(source);
  // Success is persistent, accurate, and offers explicit open actions.
  {
    StudioExportDialog dialog(project, source, &parent);
    dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    QSignalSpy finished(&dialog, &StudioExportDialog::exportFinished);
    auto *progress = dialog.findChild<QProgressBar *>("exportProgress");
    QSignalSpy values(progress, &QProgressBar::valueChanged);
    dialog.open();
    if (!check(dialog.windowModality() == Qt::WindowModal &&
                   QTest::qWaitFor([&] { return !finished.isEmpty(); }, 30000),
               error, QStringLiteral("export modal did not finish")))
      return false;
    bool actualProgress = false;
    for (const auto &value : values)
      actualProgress |= value[0].toInt() > 0 && value[0].toInt() < 100;
    if (!check(
            !finished[0][1].toBool() && dialog.isVisible() &&
                progress->value() == 100 && actualProgress &&
                dialog.findChild<QLabel *>("exportPath")->text() ==
                    destination &&
                dialog.findChild<QPushButton *>("exportOpen")->isVisible() &&
                dialog.findChild<QPushButton *>("exportFolder")->isVisible() &&
                QFileInfo(destination).size() > 0 &&
                qAbs(probeStudioSource(destination).durationMs - 2000) < 100,
            error,
            QStringLiteral("export completion/progress/path/range is wrong: %1")
                .arg(dialog.findChild<QLabel *>("exportDetail")->text())))
      return false;
    // Exercise both launch actions without opening a real desktop application.
    QFile opener(scratch.filePath(QStringLiteral("xdg-open")));
    const QByteArray script(
        "#!/bin/sh\nprintf '%s\\n' \"$1\" >> \"$OMASNAP_EXPORT_OPEN_TEST\"\n");
    if (!opener.open(QIODevice::WriteOnly) ||
        opener.write(script) != script.size())
      return check(false, error,
                   QStringLiteral("could not create test opener"));
    opener.close();
    if (!opener.setPermissions(QFileDevice::ReadOwner |
                               QFileDevice::WriteOwner | QFileDevice::ExeOwner))
      return false;
    const QByteArray oldPath = qgetenv("PATH");
    const QByteArray oldCapture = qgetenv("OMASNAP_EXPORT_OPEN_TEST");
    const auto restoreEnvironment = qScopeGuard([&] {
      if (oldPath.isNull())
        qunsetenv("PATH");
      else
        qputenv("PATH", oldPath);
      if (oldCapture.isNull())
        qunsetenv("OMASNAP_EXPORT_OPEN_TEST");
      else
        qputenv("OMASNAP_EXPORT_OPEN_TEST", oldCapture);
    });
    const QString capture = scratch.filePath(QStringLiteral("opened-paths"));
    qputenv("PATH", scratch.path().toLocal8Bit() + ':' + oldPath);
    qputenv("OMASNAP_EXPORT_OPEN_TEST", capture.toLocal8Bit());
    QByteArray expected;
    for (const auto &action : {qMakePair("exportOpen", destination),
                               qMakePair("exportFolder", scratch.path())}) {
      auto *button = dialog.findChild<QPushButton *>(action.first);
      button->click();
      expected += action.second.toUtf8() + '\n';
      if (!check(QTest::qWaitFor(
                     [&] {
                       QFile captured(capture);
                       return button->isEnabled() &&
                              captured.open(QIODevice::ReadOnly) &&
                              captured.readAll() == expected;
                     },
                     5000),
                 error,
                 QStringLiteral(
                     "Open action did not launch the exact saved path")))
        return false;
    }
    QTest::keyClick(&dialog, Qt::Key_Escape);
    if (!check(!dialog.isVisible(), error,
               QStringLiteral("Escape did not close finished export")))
      return false;
  }
  const qint64 savedSize = QFileInfo(destination).size();
  // Escape requests cancellation, waits for cleanup, and preserves prior
  // output.
  {
    StudioExportDialog dialog(project, source, &parent);
    dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    QSignalSpy finished(&dialog, &StudioExportDialog::exportFinished);
    dialog.open();
    QTest::keyClick(&dialog, Qt::Key_Escape);
    if (!check(
            dialog.isVisible() &&
                QTest::qWaitFor([&] { return !finished.isEmpty(); }, 10000) &&
                dialog.findChild<QLabel *>("exportHeading")->text() ==
                    "Export cancelled" &&
                !dialog.findChild<QPushButton *>("exportOpen")->isVisible(),
            error,
            QStringLiteral(
                "export cancellation did not retain its result modal")))
      return false;
  }
  // Invalid media reports failure rather than a misleading completion or Open.
  project.assets[0].path = scratch.filePath(QStringLiteral("missing.mp4"));
  {
    StudioExportDialog dialog(project, source, &parent);
    dialog.setAttribute(Qt::WA_DeleteOnClose, false);
    QSignalSpy finished(&dialog, &StudioExportDialog::exportFinished);
    dialog.open();
    if (!check(
            QTest::qWaitFor([&] { return !finished.isEmpty(); }, 10000) &&
                finished[0][1].toBool() && dialog.isVisible() &&
                dialog.findChild<QLabel *>("exportHeading")->text() ==
                    "Export failed" &&
                !dialog.findChild<QPushButton *>("exportOpen")->isVisible() &&
                !dialog.findChild<QLabel *>("exportDetail")->text().isEmpty(),
            error, QStringLiteral("failed export did not explain the failure")))
      return false;
  }
  return check(
      QFileInfo(destination).size() == savedSize &&
          !QFileInfo::exists(studioExportPath(source)) &&
          QDir(scratch.path())
              .entryList({".omasnap-export-*"}, QDir::Files | QDir::Hidden)
              .isEmpty() &&
          original.assets[0].path == source &&
          QFileInfo(source).size() == QFileInfo(fixture).size(),
      error,
      QStringLiteral("export left partial files or modified existing media"));
}

bool runMultiClipReadinessChecks(const QString &source, QString &error) {
  StudioProject project;
  project.assets = {{1, source, probeStudioSource(source)}};
  project.canvas = {320, 180};
  project.clips = {{1, 1, 0, 3000, 1}, {2, 1, 3000, 6000, 1}};
  project.transitions = {{1, 2, StudioTransitionKind::Crossfade, 500}};
  StudioPreview preview;
  preview.setFixedSize(480, 320);
  preview.show();
  StudioPlayback player(&preview);
  player.setProject(project, 0);
  if (!check(QTest::qWaitFor(
                 [&] {
                   return !player.seekPending() && preview.videoSlotReady(0) &&
                          preview.videoSlotReady(1);
                 },
                 10000),
             error, QStringLiteral("multi-clip sources did not load")))
    return false;
  auto *pool = QThreadPool::globalInstance();
  const int threads = pool->maxThreadCount();
  pool->setMaxThreadCount(1);
  QSemaphore gate;
  std::atomic_bool blocked = false;
  auto blocker = QtConcurrent::run([&] {
    blocked.store(true);
    gate.acquire();
  });
  const auto restore = qScopeGuard([&] {
    gate.release();
    blocker.waitForFinished();
    pool->setMaxThreadCount(threads);
  });
  if (!check(QTest::qWaitFor([&] { return blocked.load(); }, 5000), error,
             QStringLiteral("could not delay multi-clip preparations")))
    return false;
  // Reuse already-loaded sources: initial media loading itself also uses Qt's
  // pool, so blocking it before loading would test the loader, not preparation.
  project.clips[0].id = 11;
  project.clips[1].id = 22;
  project.transitions = {{11, 22, StudioTransitionKind::Crossfade, 500}};
  player.setProject(project, 0);
  const auto decoders = player.findChildren<QMediaPlayer *>();
  if (!check(QTest::qWaitFor(
                 [&] {
                   for (const auto *decoder : decoders)
                     if (!decoder->videoSink()->videoFrame().isValid() ||
                         decoder->playbackState() == QMediaPlayer::PlayingState)
                       return false;
                   return true;
                 },
                 5000),
             error,
             QStringLiteral(
                 "multi-clip decoders did not accept their prime frames")))
    return false;
  // The incoming decoded frame is waiting on the worker. A seek on the
  // outgoing clip must not invalidate that unrelated preload's generation.
  player.setPosition(500);
  gate.release();
  blocker.waitForFinished();
  pool->setMaxThreadCount(threads);
  if (!check(QTest::qWaitFor(
                 [&] {
                   return !player.seekPending() && preview.videoSlotReady(0) &&
                          preview.videoSlotReady(1);
                 },
                 5000),
             error,
             QStringLiteral("outgoing seek stranded the incoming preparation")))
    return false;
  // Appending a scene leaves the active clip/range/asset unchanged.
  project.clips.push_back({3, 1, 0, 1000, 1});
  player.setProject(project, 500);
  const auto shot = preview.grab().toImage();
  if (!check(!shot.isNull() &&
                 shot.pixelColor(shot.width() / 2, shot.height() / 2).red() >
                     180,
             error,
             QStringLiteral("appending a clip blanked an unchanged preview")))
    return false;
  player.play();
  if (!check(QTest::qWaitFor([&] { return player.position() > 3200; }, 5000),
             error,
             QStringLiteral("playback froze at the preloaded transition")))
    return false;
  player.pause();
  project.clips = {{2, 1, 3000, 6000, 1}};
  project.transitions.clear();
  player.setProject(project, 500);
  return check(
      QTest::qWaitFor(
          [&] {
            if (player.seekPending())
              return false;
            for (const auto *decoder : decoders)
              if (decoder->playbackState() == QMediaPlayer::PlayingState)
                return false;
            return true;
          },
          5000),
      error,
      QStringLiteral("an unused decoder kept running after a project edit"));
}

bool runResponsiveScrubChecks(const QString &source, QString &error) {
  StudioWindow window(source);
  window.show();
  auto *timeline = window.findChild<StudioTimeline *>();
  auto *player = window.findChild<StudioPlayback *>();
  auto *preview = window.findChild<StudioPreview *>();
  if (!check(QTest::qWaitFor(
                 [&] {
                   return player->duration() == 6000 && !player->seekPending();
                 },
                 10000),
             error, QStringLiteral("scrub fixture did not prepare")))
    return false;
  QSignalSpy requests(player, &StudioPlayback::positionChanged);
  QSignalSpy prepared(preview, &StudioPreview::videoFrameReady);
  // Deliberately delay frame preparation beyond the 35 ms mouse timer.
  // New pointer positions must not invalidate the one in-flight frame.
  auto *pool = QThreadPool::globalInstance();
  const int originalThreads = pool->maxThreadCount();
  pool->setMaxThreadCount(1);
  QSemaphore gate;
  std::atomic_bool blocked = false;
  auto blocker = QtConcurrent::run([&] {
    blocked.store(true);
    gate.acquire();
  });
  const auto restore = qScopeGuard([&] {
    gate.release();
    blocker.waitForFinished();
    pool->setMaxThreadCount(originalThreads);
  });
  if (!check(QTest::qWaitFor([&] { return blocked.load(); }, 5000), error,
             QStringLiteral("could not delay scrub preparation")))
    return false;
  const auto point = [&](qint64 ms) {
    return QPoint(64 + qRound((timeline->width() - 80) * ms / 6000.0), 60);
  };
  QTest::mousePress(timeline, Qt::LeftButton, {}, point(1000));
  for (int i = 0; i < 12; ++i) {
    QTest::mouseMove(timeline, point(1100 + i * 100));
    QTest::qWait(20);
  }
  if (!check(requests.size() == 1 && prepared.isEmpty() &&
                 (preview->videoSlotReady(0) || preview->videoSlotReady(1)),
             error,
             QStringLiteral(
                 "dragging restarted pending decoding or blanked the preview")))
    return false;
  if (!check(QTest::qWaitFor([&] { return requests.size() >= 2; }, 1800), error,
             QStringLiteral(
                 "a stalled seek permanently blocked the latest drag target")))
    return false;
  gate.release();
  blocker.waitForFinished();
  pool->setMaxThreadCount(originalThreads);
  // Continue moving with the button held, in both directions: prepared video
  // must advance before release, not just the timeline's seek signals.
  for (int i = 0; i < 60; ++i) {
    QTest::mouseMove(timeline, point(1000 + (i % 20) * 150));
    QTest::qWait(20);
  }
  if (!check(prepared.size() >= 3, error,
             QStringLiteral("video frames did not update while dragging")))
    return false;
  QTest::mouseRelease(timeline, Qt::LeftButton, {}, point(4500));
  return check(
      QTest::qWaitFor(
          [&] {
            return qAbs(player->position() - 4500) < 30 &&
                   !player->seekPending();
          },
          10000),
      error,
      QStringLiteral("scrubbing did not settle on the release position"));
}

bool runStudioInteractionChecks(QString &error) {
  const QString ffmpeg =
      QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (ffmpeg.isEmpty())
    return true;
  QTemporaryDir scratch;
  const QString source = scratch.filePath(QStringLiteral("keyboard.mp4"));
  if (!runTool(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"),
                        QStringLiteral("-f"), QStringLiteral("lavfi"),
                        QStringLiteral("-i"),
                        QStringLiteral("color=red:s=320x180:r=30:d=6"),
                        QStringLiteral("-c:v"), QStringLiteral("libx264"),
                        QStringLiteral("-g"), QStringLiteral("1"), source})) {
    error = QStringLiteral("could not generate keyboard fixture");
    return false;
  }
  const QString palettePath = scratch.filePath(QStringLiteral("colors.toml"));
  if (!runMultiClipReadinessChecks(source, error))
    return false;
  if (!runResponsiveScrubChecks(source, error))
    return false;
  if (!runStudioExportUiChecks(source, error))
    return false;
  if (!runStudioPlaybackChecks(source, error))
    return false;
  if (!runStudioCutsUiChecks(source, error))
    return false;
  if (!runStudioBackgroundUiChecks(source, error))
    return false;
  if (!runStudioScenesUiChecks(source, error))
    return false;
  if (!runStudioTransitionsUiChecks(source, error))
    return false;
  StudioWindow window(source, nullptr, palettePath);
  window.show();
  auto *player = window.findChild<StudioPlayback *>();
  auto *slider = window.findChild<QSlider *>(QStringLiteral("zoomScale"));
  auto *padding = window.findChild<QSlider *>(QStringLiteral("canvasPadding"));
  auto *exportButton =
      window.findChild<QPushButton *>(QStringLiteral("primary"));
  if (!check(
          QTest::qWaitFor(
              [&] {
                return player->duration() == 6000 && exportButton->isEnabled();
              },
              10000),
          error, QStringLiteral("Studio did not finish asynchronous loading")))
    return false;
  QTest::qWait(150);
  QTest::keyClick(&window, Qt::Key_Z);
  if (!check(slider->isEnabled(), error,
             QStringLiteral("Z did not create a zoom")))
    return false;
  // Theme swaps must not pause transport or mutate the editing history.
  QTest::keyClick(&window, Qt::Key_Space);
  const int initialScale = slider->value();
  QSaveFile palette(palettePath);
  const QByteArray paletteData(
      "background = '#fafafa'\nforeground = '#202020'\naccent = '#3256a0'\n");
  if (!palette.open(QIODevice::WriteOnly) ||
      palette.write(paletteData) != paletteData.size() || !palette.commit())
    return false;
  auto *theme = window.findChild<StudioTheme *>();
  theme->reload();
  if (!check(
          QTest::qWaitFor(
              [&] { return theme->chrome().background == QColor("#fafafa"); },
              3000) &&
              player->playbackState() == QMediaPlayer::PlayingState &&
              slider->value() == initialScale && padding->value() == 0 &&
              window.styleSheet().contains(QStringLiteral("#3256a0")),
          error, QStringLiteral("live theme reload changed playback or edits")))
    return false;
  player->pause();
  for (QWidget *focus :
       {static_cast<QWidget *>(slider), static_cast<QWidget *>(exportButton),
        static_cast<QWidget *>(padding)}) {
    focus->setFocus();
    const bool playing = player->playbackState() == QMediaPlayer::PlayingState;
    QSignalSpy states(player, &StudioPlayback::playbackStateChanged);
    QTest::keyClick(focus, Qt::Key_Space);
    if (!check((player->playbackState() == QMediaPlayer::PlayingState) !=
                       playing &&
                   states.size() == 1,
               error,
               QStringLiteral(
                   "Space did not toggle exactly once from a child control")))
      return false;
    QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier,
                     QStringLiteral(" "), true);
    QApplication::sendEvent(focus, &repeat);
    if (!check(states.size() == 1, error,
               QStringLiteral("held Space repeatedly toggled playback")))
      return false;
  }
  player->pause();
  QTest::keyClick(slider, Qt::Key_Home);
  QTest::keyClick(slider, Qt::Key_Right);
  if (!check(player->position() >= 30 && player->position() <= 35, error,
             QStringLiteral("Right did not step one frame from a slider")))
    return false;
  QTest::keyClick(slider, Qt::Key_Right, Qt::ShiftModifier);
  if (!check(player->position() >= 5030 && player->position() <= 5035, error,
             QStringLiteral("Shift+Right did not seek five seconds")))
    return false;
  QTest::keyClick(slider, Qt::Key_Home);
  // A whole drag is one undo operation, with redo restoring its final value.
  const int scale = slider->value();
  QMetaObject::invokeMethod(slider, "sliderPressed");
  slider->setValue(scale + 1);
  slider->setValue(scale + 2);
  slider->setValue(scale + 3);
  QMetaObject::invokeMethod(slider, "sliderReleased");
  QTest::keyClick(slider, Qt::Key_Z, Qt::ControlModifier);
  if (!check(slider->value() == scale, error,
             QStringLiteral("Undo did not group a zoom drag")))
    return false;
  QTest::keyClick(slider, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  if (!check(slider->value() == scale + 3, error,
             QStringLiteral("Redo did not restore the zoom")))
    return false;
  padding->setValue(8);
  QTest::keyClick(padding, Qt::Key_Z, Qt::ControlModifier);
  if (!check(padding->value() == 0, error,
             QStringLiteral("Canvas edit was not undoable")))
    return false;
  QTest::keyClick(padding, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  QTest::keyClick(&window, Qt::Key_Right, Qt::ShiftModifier);
  // Closing while a debounced write is pending waits asynchronously for the
  // final edit to be committed. Reopening restores trim, zoom, and canvas.
  window.close();
  if (!check(QTest::qWaitFor([&] { return !window.isVisible(); }, 5000), error,
             QStringLiteral("Studio did not finish saving before close")))
    return false;
  const auto saved =
      loadStudioProject(source + QStringLiteral(".omasnap.json"));
  if (!check(saved.error.isEmpty() && saved.project.zoom.cues.size() == 1 &&
                 saved.project.style.padding == 8 &&
                 saved.project.trimOutMs == -1,
             error,
             QStringLiteral(
                 "saved project does not match the last visible state")))
    return false;
  {
    StudioWindow reopened(source + QStringLiteral(".omasnap.json"), nullptr,
                          palettePath);
    reopened.show();
    auto *transport = reopened.findChild<StudioPlayback *>();
    auto *canvas =
        reopened.findChild<QSlider *>(QStringLiteral("canvasPadding"));
    if (!check(QTest::qWaitFor(
                   [&] {
                     return transport->duration() == 6000 &&
                            canvas->value() == 8 &&
                            reopened.findChild<StudioTimeline *>()->trimOut() ==
                                6000;
                   },
                   5000),
               error,
               QStringLiteral("Opening the project did not restore its state")))
      return false;
    reopened.close();
  }
  {
    const QString emptyPath =
        scratch.filePath(QStringLiteral("empty.omasnap.json"));
    if (!saveStudioProject(emptyPath, {}).isEmpty())
      return false;
    StudioWindow empty(emptyPath, nullptr, palettePath);
    empty.show();
    QTest::qWait(200);
    if (!check(!empty.findChild<QPushButton *>(QStringLiteral("primary"))
                    ->isEnabled(),
               error, QStringLiteral("Empty project enabled export")))
      return false;
    QTest::keyClick(&empty, Qt::Key_S, Qt::ControlModifier);
    empty.close();
    if (!check(QTest::qWaitFor([&] { return !empty.isVisible(); }, 5000), error,
               QStringLiteral("Empty project did not save and close")))
      return false;
    StudioProject missing = saved.project;
    missing.assets[0].path = scratch.filePath(QStringLiteral("missing.mp4"));
    const QString missingPath =
        scratch.filePath(QStringLiteral("missing.omasnap.json"));
    if (!saveStudioProject(missingPath, missing).isEmpty())
      return false;
    StudioWindow absent(missingPath, nullptr, palettePath);
    absent.show();
    if (!check(QTest::qWaitFor(
                   [&] {
                     for (auto *button : absent.findChildren<QPushButton *>())
                       if (button->text() == QStringLiteral("Relink media") &&
                           button->isVisible())
                         return true;
                     return false;
                   },
                   5000),
               error, QStringLiteral("Missing media did not expose relink")))
      return false;
    absent.close();
  }
  // Exercise the actual canvas filter, not just its argument spelling.
  const QString output = scratch.filePath(QStringLiteral("canvas.mp4"));
  const StudioStyle style{1, 10, 64};
  if (!check(runTool(ffmpeg,
                     studioExportArguments(source, output, 0, 500, {},
                                           probeStudioSource(source), style)),
             error, QStringLiteral("styled export failed")))
    return false;
  const QImage frame = frameAt(ffmpeg, output, 0.2, scratch.path());
  if (!check(!frame.isNull() && frame.size() == QSize(320, 180), error,
             QStringLiteral("styled export changed canvas dimensions")))
    return false;
  const QColor corner = frame.pixelColor(4, 4);
  const QColor expected = style.color();
  if (!check(std::abs(corner.red() - expected.red()) < 8 &&
                 std::abs(corner.green() - expected.green()) < 8 &&
                 std::abs(corner.blue() - expected.blue()) < 8 &&
                 frame.pixelColor(160, 90).red() > 220,
             error,
             QStringLiteral("exported canvas or video colors are wrong")))
    return false;
  return true;
}

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
