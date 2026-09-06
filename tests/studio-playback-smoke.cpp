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
  return failures.isEmpty();
}
