/** @fileoverview Decode/transport checks using the suite's generated video. */
#include "studio-playback-smoke.hpp"
#include "studio-playback.hpp"
#include "studio-preview.hpp"
#include <QGuiApplication>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
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
    QImage blue(160, 90, QImage::Format_RGBA8888);
    blue.fill(Qt::blue);
    surface.setFrame(0, prepareStudioVideoFrame(QVideoFrame(portrait)));
    surface.setFrame(1, prepareStudioVideoFrame(QVideoFrame(blue)));
    surface.primary = 0;
    surface.secondary = 1;
    surface.primaryOpacity = 0.25;
    surface.secondaryOpacity = 0.75;
    surface.fits[0] = QRectF(0.359375, 0, 0.28125, 1);
    surface.fits[1] = QRectF(0, 0, 1, 1);
    surface.source = QRectF(0, 0, 1, 1);
    surface.drawn = surface.canvas.adjusted(32, 18, -32, -18);
    surface.background = Qt::green;
    surface.radius = 16;
    shot = surface.grabFramebuffer();
    const QColor blended = colorAt(shot, 0.5);
    const QColor bar = colorAt(shot, 0.25);
    if (qAbs(blended.red() - 64) > 3 || qAbs(blended.blue() - 191) > 3 ||
        blended.green() > 3 || bar.red() > 3 || qAbs(bar.blue() - 191) > 3 ||
        shot.pixelColor(shot.width() / 2, 2).green() < 250) {
      error = QStringLiteral(
          "GPU paired RGB blend did not preserve canonical fit/style once");
      return false;
    }
    surface.primaryOpacity = surface.secondaryOpacity = 0;
    shot = surface.grabFramebuffer();
    if (colorAt(shot, 0.5) != QColor(Qt::black) ||
        shot.pixelColor(shot.width() / 2, 2).green() < 250) {
      error = QStringLiteral("GPU black fade changed the style background or "
                             "missed black midpoint");
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

  // Audio lane follower: plays the span under the clock, silent in gaps,
  // and re-resolves when the window pushes new clips.
  const QString ffmpeg =
      QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (!ffmpeg.isEmpty()) {
    QTemporaryDir songScratch;
    if (!require(songScratch.isValid(), "could not create follower scratch"))
      return false;
    const QString song = songScratch.filePath(QStringLiteral("song.m4a"));
    auto encodedSong = QtConcurrent::run([song] {
      QProcess encoder;
      encoder.start(
          QStringLiteral("ffmpeg"),
          {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-f"),
           QStringLiteral("lavfi"), QStringLiteral("-i"),
           QStringLiteral("sine=frequency=440:sample_rate=48000"),
           QStringLiteral("-t"), QStringLiteral("3"), QStringLiteral("-c:a"),
           QStringLiteral("aac"), song});
      return encoder.waitForFinished(10000) && encoder.exitCode() == 0;
    });
    if (!require(QTest::qWaitFor([&] { return encodedSong.isFinished(); },
                                 12000) &&
                     encodedSong.result(),
                 "could not generate follower fixture"))
      return false;
    StudioSource songSource;
    songSource.durationMs = 3000;
    songSource.audioStreams = 1;
    StudioProject scored;
    scored.assets = {asset, {2, song, songSource}};
    scored.canvas = {320, 180};
    scored.clips = {{1, 1, 0, 2000, 1.0}};
    scored.audioClips = {{11, 2, 0, 1000, 1.0, 100}};
    playback.setProject(scored);
    auto *follower = playback.audioPlayerForTest();
    playback.setPosition(900);
    playback.play();
    if (!require(QTest::qWaitFor(
                     [&] {
                       return follower->source().toLocalFile() == song &&
                              follower->playbackState() ==
                                  QMediaPlayer::PlayingState &&
                              qAbs(follower->position() - 900) < 700;
                     },
                     5000),
                 "follower did not play the lane span"))
      return false;
    // Seeking under playback re-anchors: backward, so the drift is always
    // past the correction threshold rather than racing it.
    playback.setPosition(100);
    if (!require(QTest::qWaitFor(
                     [&] { return qAbs(follower->position() - 100) < 700; },
                     5000),
                 "playing seek left the follower behind"))
      return false;
    playback.setPosition(1500);
    if (!require(QTest::qWaitFor(
                     [&] {
                       return follower->playbackState() !=
                              QMediaPlayer::PlayingState;
                     },
                     5000),
                 "follower played through a lane gap"))
      return false;
    playback.setAudioClips({});
    playback.setPosition(500);
    if (!require(follower->playbackState() != QMediaPlayer::PlayingState,
                 "cleared lane kept playing"))
      return false;
    playback.setAudioClips(scored.audioClips);
    if (!require(QTest::qWaitFor(
                     [&] {
                       return follower->playbackState() ==
                              QMediaPlayer::PlayingState;
                     },
                     5000),
                 "pushed lane did not resume"))
      return false;
    playback.pause();
    playback.setProject(StudioProject{});
  }

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
  // A parked decoder may have played beyond its original preload/seek target.
  // Revisit that target after another clip: compare pixels, not just the clock.
  StudioProject duplicates = original;
  duplicates.clips = {
      {1, 1, 0, 3000, 1.0}, {2, 1, 0, 3000, 1.0}, {3, 1, 0, 3000, 1.0}};
  playback.setProject(duplicates, 3800);
  if (!require(QTest::qWaitFor([&] { return pixelsMatch(0, false); }, 4000),
               "duplicate middle clip did not seek to red"))
    return false;
  playback.play();
  if (!require(QTest::qWaitFor(
                   [&] {
                     return playback.position() > 4200 && pixelsMatch(1, false);
                   },
                   4000),
               "duplicate middle clip did not play into green"))
    return false;
  playback.pause();
  if (!require(seekAndCheck(300, 0, false),
               "return to first duplicate went blank") ||
      !require(seekAndCheck(3800, 0, false),
               "revisited duplicate reused its old playback frame"))
    return false;
  // Match the reported three-scene failure: while the middle duplicate plays,
  // its other decoder preloads a DIFFERENT file. Seeking back must replace
  // that file and seek after the new source actually finishes loading. Qt
  // reports LoadedMedia for the old source while stopping it during replacement.
  duplicates.assets.append(mixed.assets[1]);
  duplicates.clips[2] = {3, 2, 0, 1000, 1.0};
  playback.setProject(duplicates, 4400);
  if (!require(QTest::qWaitFor([&] {
                 return !playback.seekPending() && preview.videoSlotReady(0) &&
                        preview.videoSlotReady(1);
               }, 4000), "mixed-source duplicate preloads did not settle"))
    return false;
  playback.play();
  if (!require(QTest::qWaitFor([&] { return playback.position() > 4600; }, 3000),
               "middle duplicate did not advance before backward seek"))
    return false;
  playback.pause();
  playback.scrubTo(2300);
  auto *reloadedSink = playback.activePlayer()->videoSink();
  QSignalSpy reloadedFrames(reloadedSink, &QVideoSink::videoFrameChanged);
  if (!require(QTest::qWaitFor([&] {
                 for (const auto &args : reloadedFrames)
                   if (args[0].value<QVideoFrame>().isValid())
                     return true;
                 return false;
               }, 4000), "backward source replacement produced no frame"))
    return false;
  QVideoFrame firstReloaded;
  for (const auto &args : reloadedFrames) {
    const auto frame = args[0].value<QVideoFrame>();
    if (frame.isValid()) {
      firstReloaded = frame;
      break;
    }
  }
  // Qt may deliver the frame ending exactly at the target first. That is a
  // correctly dispatched seek too; distinguish it from decoding from zero.
  if (!require(firstReloaded.startTime() >= 2200000 &&
                   firstReloaded.startTime() <= 2400000,
               "source replacement decoded from zero instead of seek target") ||
      !require(QTest::qWaitFor([&] {
                 return !playback.seekPending() && pixelsMatch(2, false);
               }, 4000), "paused backward seek left the preview blank"))
    return false;
  // A backend can report a decode error without changing BufferedMedia.
  // Reloading that same file must not leave loaded=false forever. The music
  // player is no decoder and gets no simulated failure.
  for (auto *decoder : playback.findChildren<QMediaPlayer *>()) {
    if (!decoder->videoSink())
      continue;
    decoder->errorOccurred(QMediaPlayer::ResourceError,
                           QStringLiteral("simulated mid-file decode failure"));
  }
  failures.clear();
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
  StudioProject transitioned = mixed;
  QString transitionError;
  if (!require(studioSetTransition(transitioned, 1, 2,
                                   StudioTransitionKind::Crossfade, 100,
                                   transitionError) &&
                   studioSetTransition(transitioned, 2, 3,
                                       StudioTransitionKind::FadeBlack, 100,
                                       transitionError),
               "could not construct paired playback transitions"))
    return false;
  const auto colorNear = [&](QColor expected, int tolerance = 18) {
    const auto shot = preview.grab().toImage();
    if (shot.isNull())
      return false;
    const auto pixel = shot.pixelColor(shot.width() / 2, shot.height() / 2);
    return qAbs(pixel.red() - expected.red()) <= tolerance &&
           qAbs(pixel.green() - expected.green()) <= tolerance &&
           qAbs(pixel.blue() - expected.blue()) <= tolerance;
  };
  playback.setProject(transitioned, 525);
  playback.audioOutput()->setVolume(0.8F);
  if (!require(playback.duration() == 1600 &&
                   QTest::qWaitFor(
                       [&] { return colorNear(QColor(190, 64, 0)); }, 4000),
               "paused crossfade quarter did not blend both source pixels"))
    return false;
  playback.setPosition(575);
  if (!require(
          QTest::qWaitFor([&] { return colorNear(QColor(64, 190, 0)); }, 4000),
          "forward transition seek retained the wrong pair/progress")) {
    return false;
  }
  playback.setPosition(1050);
  if (!require(QTest::qWaitFor([&] { return colorNear(Qt::black, 5); }, 4000),
               "fade-through-black midpoint was not black"))
    return false;
  playback.setPosition(525);
  if (!require(
          QTest::qWaitFor([&] { return colorNear(QColor(190, 64, 0)); }, 4000),
          "reverse transition seek retained later scene textures"))
    return false;
  const auto allPlayers = playback.findChildren<QMediaPlayer *>();
  QVector<QMediaPlayer *> players;
  for (auto *candidate : allPlayers)
    if (candidate->videoSink())
      players.push_back(candidate);
  const auto *outgoingAudio = playback.activePlayer()->audioOutput();
  const auto *incomingAudio = players[0] == playback.activePlayer()
                                  ? players[1]->audioOutput()
                                  : players[0]->audioOutput();
  if (!require(qAbs(outgoingAudio->volume() - 0.6F) < 0.01F &&
                   qAbs(incomingAudio->volume() - 0.2F) < 0.01F &&
                   outgoingAudio->isMuted() && incomingAudio->isMuted(),
               "transition source gains did not follow shared audio "
               "weights/master volume"))
    return false;
  QTest::qWait(150);
  if (!require(playback.position() == 525 && colorNear(QColor(190, 64, 0)),
               "paused pair advanced while queued decoder frames drained"))
    return false;
  playback.setPosition(450);
  playback.play();
  if (!require(QTest::qWaitFor(
                   [&] {
                     return playback.position() >= 700 && pixelsMatch(1, true);
                   },
                   4000),
               "playing transition failed to hand off to the incoming scene") ||
      !require(QTest::qWaitFor(
                   [&] {
                     return playback.position() >= 1150 &&
                            pixelsMatch(2, false);
                   },
                   4000),
               "second transition failed after bounded decoder reuse"))
    return false;
  playback.pause();
  if (!require(failures.isEmpty(), "Unexpected transition decoder error"))
    return false;

  StudioProject directional = mixed;
  directional.clips = {{1, 1, 0, 600, 1}, {3, 3, 0, 600, 1}};
  for (const auto kind :
       {StudioTransitionKind::WipeLeft, StudioTransitionKind::WipeRight,
        StudioTransitionKind::WipeUp, StudioTransitionKind::WipeDown,
        StudioTransitionKind::SlideLeft, StudioTransitionKind::SlideRight,
        StudioTransitionKind::SlideUp, StudioTransitionKind::SlideDown}) {
    if (!require(
            studioSetTransition(directional, 1, 3, kind, 100, transitionError),
            "Could not configure directional transport"))
      return false;
    playback.setProject(directional, 525);
    if (!require(QTest::qWaitFor([&] { return colorNear(Qt::red); }, 4000),
                 "Directional transport lost outgoing quarter frame"))
      return false;
    playback.setPosition(575);
    if (!require(QTest::qWaitFor([&] { return colorNear(Qt::blue); }, 4000),
                 "Directional transport lost incoming quarter frame"))
      return false;
    playback.setPosition(525);
    if (!require(QTest::qWaitFor([&] { return colorNear(Qt::red); }, 4000),
                 "Reverse directional seek retained incoming frame"))
      return false;
  }
  playback.setPosition(450);
  playback.play();
  if (!require(
          QTest::qWaitFor(
              [&] { return playback.position() >= 700 && colorNear(Qt::blue); },
              4000),
          "Directional playback failed incoming decoder handoff"))
    return false;
  playback.pause();

  // Full source ranges exercise the decoder's EOF callback, rather than only
  // the composition timer's earlier trimmed-out boundary.
  StudioProject eof = mixed;
  eof.clips = {{1, 1, 0, 1000, 1}, {2, 2, 0, 1000, 1}};
  if (!require(studioSetTransition(eof, 1, 2, StudioTransitionKind::Crossfade,
                                   200, transitionError),
               "Could not create full-source EOF transition"))
    return false;
  playback.setProject(eof, 700);
  bool handoffReady = false;
  const auto handoff = QObject::connect(
      &playback, &StudioPlayback::activeClipChanged, &preview, [&](quint64 id) {
        if (id == 2)
          handoffReady =
              preview.videoSlotReady(players.indexOf(playback.activePlayer()));
      });
  playback.play();
  if (!require(
          QTest::qWaitFor([&] { return playback.position() >= 1200; }, 5000) &&
              handoffReady && pixelsMatch(1, true),
          "Full-source EOF discarded the already-playing incoming frame"))
    return false;
  playback.pause();
  QObject::disconnect(handoff);

  // An incoming asset can disappear after metadata probing. Its decoder error
  // must become actionable at the overlap, not leave Playing waiting forever.
  StudioProject unavailable = eof;
  unavailable.assets[1].path = scratch.filePath("missing-incoming.mp4");
  playback.setProject(unavailable, 700);
  playback.play();
  return require(
      QTest::qWaitFor([&] { return !failures.isEmpty(); }, 5000) &&
          playback.playbackState() == QMediaPlayer::PausedState,
      "Failed incoming decoder left transition playback waiting forever");
}
