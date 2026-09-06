/** @fileoverview Decode/transport checks using the suite's generated video. */
#include "studio-playback-smoke.hpp"
#include "studio-playback.hpp"
#include "studio-preview.hpp"
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrentRun>

bool runStudioPlaybackChecks(const QString &mediaPath, QString &error) {
  if (QGuiApplication::platformName() != QStringLiteral("offscreen")) {
    StudioVideoSurface surface(nullptr);
    surface.resize(320, 180);
    surface.canvas = surface.drawn = QRectF(0, 0, 320, 180);
    // 120x240 portrait fitted on a 320x180 canonical canvas, then zoomed.
    surface.fit = QRectF(0.359375, 0, 0.28125, 1);
    QImage portrait(120, 240, QImage::Format_RGBA8888);
    portrait.fill(Qt::red);
    surface.setFrame(prepareStudioVideoFrame(QVideoFrame(portrait)));
    surface.show();
    QTest::qWait(50);
    surface.canvas = surface.drawn = surface.rect();
    auto shot = surface.grabFramebuffer();
    const auto colorAt = [](const QImage &image, double x) {
      return image.pixelColor(qRound((image.width() - 1) * x),
                              image.height() / 2);
    };
    if (shot.isNull() || colorAt(shot, 0.5).red() < 245 ||
        colorAt(shot, 0.25).red() > 10) {
      error = QStringLiteral(
          "GPU canonical fit did not preserve portrait letterbox");
      return false;
    }
    surface.source = QRectF(0.25, 0.25, 0.5, 0.5);
    shot = surface.grabFramebuffer();
    if (shot.isNull() || colorAt(shot, 0.25).red() < 245) {
      error =
          QStringLiteral("GPU zoom was applied before canonical source fit");
      return false;
    }
  }
  StudioPreview preview;
  preview.resize(480, 320);
  preview.show();
  StudioPlayback playback(&preview);
  QSignalSpy failures(&playback, &StudioPlayback::errorOccurred);
  QSignalSpy frames(&playback, &StudioPlayback::positionChanged);
  QSignalSpy changes(&playback, &StudioPlayback::activeClipChanged);
  StudioProject project;
  StudioAsset asset;
  asset.id = 1;
  asset.path = mediaPath;
  asset.source.size = {320, 180};
  asset.source.fpsNumerator = 30;
  asset.source.fpsDenominator = 1;
  asset.source.durationMs = 2000;
  project.assets = {asset};
  project.clips = {
      {1, 1, 0, 300, 1.0}, {2, 1, 500, 1100, 2.0}, {3, 1, 1200, 1500, 1.0}};
  project.canvas = {320, 180};
  playback.setProject(project);
  const auto require = [&error, &failures](bool ok, const char *message) {
    if (!ok)
      error = QString::fromLatin1(message) +
              (failures.isEmpty()
                   ? QString{}
                   : QStringLiteral(": ") + failures.last().first().toString());
    return ok;
  };
  if (!require(playback.duration() == 900, "composition duration is wrong"))
    return false;
  playback.play();
  if (!require(QTest::qWaitFor(
                   [&] {
                     return playback.position() == 900 &&
                            playback.playbackState() !=
                                QMediaPlayer::PlayingState;
                   },
                   7000),
               "playback did not cross all hard cuts and stop at project end"))
    return false;
  if (!require(
          failures.isEmpty() && changes.size() >= 3 && frames.size() > 3,
          "composition did not present source frames across decoder slots"))
    return false;
  playback.setPosition(300);
  if (!require(playback.position() == 300 &&
                   changes.last().first().toULongLong() == 2,
               "exact boundary seek did not select incoming scene"))
    return false;
  playback.setPosition(599);
  playback.setPosition(50);
  playback.play();
  if (!require(QTest::qWaitFor([&] { return playback.position() > 70; }, 3000),
               "reverse seek failed to resume source-time playback"))
    return false;
  const auto count = changes.size();
  project.style.padding = 12;
  playback.setProject(project);
  if (!require(playback.playbackState() == QMediaPlayer::PlayingState &&
                   changes.size() == count,
               "styling change interrupted composition playback"))
    return false;
  playback.pause();
  const qint64 stoppedAt = playback.position();
  QTest::qWait(100);
  if (!require(playback.position() == stoppedAt,
               "pause moved project playhead"))
    return false;
  playback.setProject(StudioProject{});
  if (!require(playback.duration() == 0 && playback.position() == 0 &&
                   playback.playbackState() == QMediaPlayer::StoppedState,
               "empty project retained stale media transport"))
    return false;

  QTemporaryDir scratch;
  StudioProject mixed;
  mixed.canvas = {320, 180};
  const QStringList colors{QStringLiteral("red"), QStringLiteral("lime"),
                           QStringLiteral("blue")};
  const QList<QSize> sizes{{160, 90}, {90, 160}, {160, 120}};
  for (int i = 0; i < 3; ++i) {
    const auto path =
        scratch.filePath(QString::number(i) + QStringLiteral(".mp4"));
    const QString color = colors[i];
    const QSize size = sizes[i];
    auto encoded = QtConcurrent::run([path, color, size] {
      QProcess encoder;
      encoder.start(
          QStringLiteral("ffmpeg"),
          {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
           QStringLiteral("lavfi"), QStringLiteral("-i"),
           QStringLiteral("color=c=%1:s=%2x%3:r=30:d=1")
               .arg(color)
               .arg(size.width())
               .arg(size.height()),
           QStringLiteral("-c:v"), QStringLiteral("libx264"),
           QStringLiteral("-preset"), QStringLiteral("ultrafast"),
           QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), path});
      return encoder.waitForFinished(10000) && encoder.exitCode() == 0;
    });
    if (!require(QTest::qWaitFor([&] { return encoded.isFinished(); }, 12000) &&
                     encoded.result(),
                 "could not generate mixed-scene playback fixture"))
      return false;
    StudioAsset item = asset;
    item.id = static_cast<quint64>(i + 1);
    item.path = path;
    item.source.size = sizes[i];
    item.source.durationMs = 1000;
    mixed.assets.append(item);
    mixed.clips.append({item.id, item.id, 0, 600, 1.0});
  }
  preview.setPickable(false);
  playback.setProject(mixed);
  const auto pixelsMatch = [&](int channel, bool bars) {
    const auto shot = preview.grab().toImage();
    if (shot.isNull())
      return false;
    const auto center = shot.pixelColor(shot.width() / 2, shot.height() / 2);
    const int channels[]{center.red(), center.green(), center.blue()};
    if (channels[channel] < 180 || channels[(channel + 1) % 3] > 80 ||
        channels[(channel + 2) % 3] > 80)
      return false;
    if (bars) {
      // Canonical 16:9 canvas is fitted in the preview before source fit.
      const double canvasWidth =
          qMin(static_cast<double>(shot.width()), shot.height() * 16.0 / 9.0);
      const int x =
          qRound((shot.width() - canvasWidth) / 2 + canvasWidth * 0.2);
      const auto bar = shot.pixelColor(x, shot.height() / 2);
      return qMax(bar.red(), qMax(bar.green(), bar.blue())) < 15;
    }
    return true;
  };
  const auto seekAndCheck = [&](qint64 time, int channel, bool bars) {
    playback.setPosition(time);
    const bool ok =
        QTest::qWaitFor([&] { return pixelsMatch(channel, bars); }, 4000);
    if (!ok) {
      const auto shot = preview.grab().toImage();
      qWarning() << "pixel seek" << time << shot.size()
                 << shot.pixelColor(shot.width() / 2, shot.height() / 2)
                 << playback.activePlayer()->position()
                 << playback.activePlayer()->mediaStatus();
      const auto decoded =
          playback.activePlayer()->videoSink()->videoFrame().toImage();
      qWarning() << "decoder" << playback.activePlayer()->source()
                 << decoded.size()
                 << (decoded.isNull()
                         ? QColor{}
                         : decoded.pixelColor(decoded.width() / 2,
                                              decoded.height() / 2));
    }
    return ok;
  };
  if (!require(seekAndCheck(0, 0, false),
               "first scene did not display red pixels") ||
      !require(seekAndCheck(650, 1, true),
               "forward cut seek retained stale frame or lost portrait fit") ||
      !require(seekAndCheck(1250, 2, false),
               "third scene did not display blue pixels") ||
      !require(seekAndCheck(600, 1, true),
               "reverse boundary seek retained outgoing frame") ||
      !require(seekAndCheck(50, 0, false),
               "reverse seek to first source retained stale pixels"))
    return false;
  playback.play();
  if (!require(QTest::qWaitFor([&] { return pixelsMatch(1, true); }, 4000),
               "playing hard cut did not present incoming portrait pixels") ||
      !require(QTest::qWaitFor([&] { return pixelsMatch(2, false); }, 4000),
               "playing second hard cut did not present third-source pixels"))
    return false;
  playback.pause();
  // A cut inside one source must not display a cached frame from the deleted
  // passage, either immediately after the edit or when crossing the new cut.
  const QString stripedPath = scratch.filePath(QStringLiteral("striped.mp4"));
  const QStringList scenePaths{mixed.assets[0].path, mixed.assets[1].path,
                               mixed.assets[2].path};
  auto joined = QtConcurrent::run([scenePaths, stripedPath] {
    QProcess encoder;
    encoder.start(
        QStringLiteral("ffmpeg"),
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-i"),
         scenePaths[0], QStringLiteral("-i"), scenePaths[1],
         QStringLiteral("-i"), scenePaths[2], QStringLiteral("-filter_complex"),
         QStringLiteral("[0:v]scale=320:180,setsar=1[v0];"
                        "[1:v]scale=320:180,setsar=1[v1];"
                        "[2:v]scale=320:180,setsar=1[v2];"
                        "[v0][v1][v2]concat=n=3:v=1:a=0[out]"),
         QStringLiteral("-map"), QStringLiteral("[out]"),
         QStringLiteral("-c:v"), QStringLiteral("libx264"),
         QStringLiteral("-preset"), QStringLiteral("ultrafast"), stripedPath});
    return encoder.waitForFinished(10000) && encoder.exitCode() == 0;
  });
  if (!require(QTest::qWaitFor([&] { return joined.isFinished(); }, 12000) &&
                   joined.result(),
               "could not generate middle-cut playback fixture"))
    return false;
  StudioProject original;
  original.canvas = {320, 180};
  StudioAsset striped = asset;
  striped.path = stripedPath;
  striped.source.durationMs = 3000;
  original.assets = {striped};
  original.clips = {{1, 1, 0, 3000, 1.0}};
  playback.setProject(original);
  if (!require(seekAndCheck(1500, 1, false),
               "uncut source did not show the green middle passage"))
    return false;
  StudioProject cut = original;
  const auto removed = studioDeleteRange(cut, 1000, 2000, 99);
  if (!require(
          removed.changed && studioDuration(cut) == 2000,
          "middle passage deletion did not create the expected composition"))
    return false;
  playback.setProject(cut);
  if (!require(
          !pixelsMatch(1, false),
          "deleted passage remained visible while replacement seek decoded") ||
      !require(QTest::qWaitFor([&] { return pixelsMatch(2, false); }, 4000),
               "middle cut did not show mapped blue source frames") ||
      !require(seekAndCheck(500, 0, false), "cut lost retained red opening") ||
      !require(seekAndCheck(1000, 2, false),
               "new cut boundary displayed deleted green") ||
      !require(seekAndCheck(900, 0, false),
               "reverse cut seek lost retained red"))
    return false;
  bool leaked = false;
  playback.play();
  const bool ended = QTest::qWaitFor(
      [&] {
        leaked = leaked || pixelsMatch(1, false);
        return playback.position() == 2000;
      },
      5000);
  if (!require(ended && !leaked,
               "playing the edited source leaked removed frames or stalled"))
    return false;
  playback.pause();
  playback.setProject(original);
  if (!require(
          seekAndCheck(1500, 1, false),
          "restoring the pre-cut project did not restore the green passage"))
    return false;
  StudioProject nearColorBoundary = original;
  nearColorBoundary.clips = {{1, 1, 0, 300, 1.0}, {99, 1, 970, 1500, 1.0}};
  playback.setProject(nearColorBoundary, 0);
  QTest::qWait(250);
  if (!require(seekAndCheck(300, 0, false),
               "incoming preload skipped its opening red frame"))
    return false;
  QTest::qWait(150);
  if (!require(pixelsMatch(0, false),
               "queued post-pause frames changed the paused cut frame"))
    return false;

  // Repeated composition changes can pause a decoder while its initial source
  // is loading. Qt must finish restoring that pending state before we prime.
  StudioPreview initialPreview;
  initialPreview.resize(480, 320);
  initialPreview.show();
  StudioPlayback initial(&initialPreview);
  StudioProject rapid = original;
  initial.setProject(rapid);
  initial.pause();
  if (!require(studioSplitClip(rapid, 1000, 2), "first rapid split failed"))
    return false;
  initial.setProject(rapid, 1000);
  initial.pause();
  if (!require(studioSplitClip(rapid, 2000, 3), "second rapid split failed"))
    return false;
  initial.setProject(rapid, 1500);
  if (!require(
          QTest::qWaitFor(
              [&] {
                const auto shot = initialPreview.grab().toImage();
                const auto color =
                    shot.pixelColor(shot.width() / 2, shot.height() / 2);
                return color.green() > 180 && color.red() < 80 &&
                       color.blue() < 80;
              },
              4000),
          "rapid initial splits left a paused decoder without a preview frame"))
    return false;
  initialPreview.hide();
  StudioProject imported;
  imported.canvas = mixed.canvas;
  QString sceneError;
  if (!require(studioInsertScenes(imported, mixed.assets, mixed.clips, 0,
                                  sceneError),
               "three-file scene import failed"))
    return false;
  StudioHistory sceneHistory;
  StudioEditState sceneState;
  sceneState.project = imported;
  sceneHistory.reset(sceneState);
  playback.setProject(imported, 650);
  if (!require(QTest::qWaitFor([&] { return pixelsMatch(1, true); }, 4000),
               "imported portrait scene did not display at the requested "
               "project time"))
    return false;
  if (!require(studioMoveClip(imported, 3, 1, sceneError),
               "moving the blue scene before red failed"))
    return false;
  sceneState.project = imported;
  sceneHistory.push(sceneState);
  playback.setProject(imported, 0);
  if (!require(QTest::qWaitFor([&] { return pixelsMatch(2, false); }, 4000),
               "reorder retained the old opening scene") ||
      !require(seekAndCheck(650, 0, false),
               "reorder did not place red second") ||
      !require(seekAndCheck(1250, 1, true),
               "reorder did not place portrait green last"))
    return false;
  if (!require(studioDuplicateClip(imported, 3, 77, sceneError),
               "duplicating the blue source failed"))
    return false;
  sceneState.project = imported;
  sceneHistory.push(sceneState);
  if (!require(studioTrimClip(imported, 77, 200, 450, sceneError),
               "trimming the duplicated blue scene failed"))
    return false;
  sceneState.project = imported;
  sceneHistory.push(sceneState);
  playback.setProject(imported, 650);
  if (!require(
          playback.duration() == 2050 &&
              QTest::qWaitFor([&] { return pixelsMatch(2, false); }, 4000),
          "duplicate edge trim did not map project time onto blue source") ||
      !require(seekAndCheck(1450, 1, true),
               "trimmed sequence green boundary is wrong") ||
      !require(seekAndCheck(850, 0, false),
               "reverse seek retained wrong scene after trim") ||
      !require(seekAndCheck(600, 2, false),
               "duplicate opening source frame was missing"))
    return false;
  if (!require(sceneHistory.undo(), "scene trim undo was unavailable"))
    return false;
  playback.setProject(sceneHistory.current().project, 900);
  if (!require(
          playback.duration() == 2400 &&
              QTest::qWaitFor([&] { return pixelsMatch(2, false); }, 4000),
          "undo did not restore the duplicate's untrimmed duration and pixels"))
    return false;
  if (!require(sceneHistory.redo(), "scene trim redo was unavailable"))
    return false;
  playback.setProject(sceneHistory.current().project, 800);
  playback.play();
  if (!require(QTest::qWaitFor([&] { return pixelsMatch(0, false); }, 4000),
               "playing reordered duplicate/trim sequence did not enter red") ||
      !require(QTest::qWaitFor([&] { return pixelsMatch(1, true); }, 4000),
               "playing reordered sequence did not end with portrait green"))
    return false;
  playback.pause();
  return failures.isEmpty();
}
