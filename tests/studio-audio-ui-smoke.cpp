/** @fileoverview Audio lane: waveform paint and clip selection. */
#include "studio-audio-ui-smoke.hpp"
#include "studio-playback.hpp"
#include "studio-project.hpp"
#include "studio.hpp"
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

namespace {
// Full-scale square WAV: deterministic peaks through any decoder.
bool writeSquareWav(const QString &path, int totalSeconds, int loudSeconds) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;
  const int rate = 8000, samples = rate * totalSeconds;
  QByteArray header;
  QDataStream head(&header, QIODevice::WriteOnly);
  head.setByteOrder(QDataStream::LittleEndian);
  head.writeRawData("RIFF", 4);
  head << quint32(36 + samples * 2);
  head.writeRawData("WAVEfmt ", 8);
  head << quint32(16) << quint16(1) << quint16(1) << quint32(rate)
       << quint32(rate * 2) << quint16(2) << quint16(16);
  head.writeRawData("data", 4);
  head << quint32(samples * 2);
  if (file.write(header) != header.size())
    return false;
  QByteArray pcm;
  QDataStream body(&pcm, QIODevice::WriteOnly);
  body.setByteOrder(QDataStream::LittleEndian);
  for (int i = 0; i < samples; ++i) {
    const qint16 sample =
        i < rate * loudSeconds ? ((i / 10) % 2 ? 32767 : -32767) : 0;
    body << sample;
  }
  return file.write(pcm) == pcm.size();
}
} // namespace

bool runStudioAudioUiChecks(const QString &source, QString &error) {
  QTemporaryDir scratch;
  const auto base = scratch.filePath(QStringLiteral("base.mp4"));
  if (!QFile::copy(source, base))
    return false;
  const QString song = scratch.filePath(QStringLiteral("song.wav"));
  if (!writeSquareWav(song, 2, 2))
    return false;
  // Ten seconds, loud for six: overruns the six-second video, so the lane
  // must map the visible slice, not squeeze the source into view.
  const QString epic = scratch.filePath(QStringLiteral("epic.wav"));
  if (!writeSquareWav(epic, 10, 6))
    return false;
  StudioProject project;
  project.assets = {{1, base, probeStudioSource(base)}};
  StudioSource songSource;
  songSource.durationMs = 2000;
  songSource.audioStreams = 1;
  project.assets.push_back({2, song, songSource});
  StudioSource epicSource;
  epicSource.durationMs = 10000;
  epicSource.audioStreams = 1;
  project.assets.push_back({3, epic, epicSource});
  project.canvas = {320, 180};
  project.clips = {{1, 1, 0, 6000, 1}};
  project.audioClips = {{11, 2, 0, 1500, 1.0, 100},
                        {12, 3, 0, 10000, 1.0, 100}};
  const QString document = scratch.filePath(QStringLiteral("audio.omasnap.json"));
  if (!saveStudioProject(document, project).isEmpty())
    return false;
  StudioWindow window(document, nullptr, scratch.filePath("palette.toml"));
  window.show();
  auto *player = window.findChild<StudioPlayback *>();
  auto *timeline = window.findChild<StudioTimeline *>();
  auto *theme = window.findChild<StudioTheme *>();
  const auto require = [&error](bool ok, const char *why) {
    if (!ok)
      error = QString::fromLatin1(why);
    return ok;
  };
  if (!require(timeline && player && theme, "audio lane controls are missing"))
    return false;
  if (!require(
          QTest::qWaitFor([&] { return player->duration() == 6000; }, 6000),
          "audio lane source did not load"))
    return false;
  // Peaks arrive from the worker; the lane paints flat until then.
  if (!require(QTest::qWaitFor(
                   [&] {
                     return timeline->audioPeaksFor(2) != nullptr &&
                            timeline->audioPeaksFor(3) != nullptr;
                   },
                   10000),
               "audio waveform never decoded"))
    return false;
  // Lane geometry mirrors the timeline constants: trim bar at y 38 h 44,
  // zoom lane 92..122, audio lane 132..162; 64 px inset with 16 px margin.
  const auto lanePoint = [&](qint64 ms) {
    return QPoint(64 + qRound((timeline->width() - 80) * ms / 6000.0), 147);
  };
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, lanePoint(1000));
  if (!require(timeline->selectedAudioClip() == 11,
               "audio lane click did not select the clip"))
    return false;
  const QPixmap shot = timeline->grab();
  const qreal dpr = shot.devicePixelRatio();
  const auto pixelAt = [&](int x, int y) {
    return shot.toImage().pixelColor(qRound(x * dpr), qRound(y * dpr));
  };
  // One row inside the lane top: the antialiased waveform stays two below.
  if (!require(pixelAt(lanePoint(1000).x(), 133) == theme->chrome().accent,
               "selected audio clip is not painted"))
    return false;
  // The overlong clip maps its visible slice: 5500 ms shows source 3500 ms
  // (loud), not the source squeezed into view (silent past 6000 ms). Off
  // the midline, where silent buckets paint nothing.
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, lanePoint(3000));
  if (!require(timeline->selectedAudioClip() == 12,
               "overlong audio clip did not select"))
    return false;
  const QPixmap rescored = timeline->grab();
  const qreal rescoredDpr = rescored.devicePixelRatio();
  if (!require(rescored.toImage().pixelColor(
                   qRound(lanePoint(5500).x() * rescoredDpr),
                   qRound(140 * rescoredDpr)) == theme->chrome().onAccent(),
               "overlong sound shows squeezed timing"))
    return false;
  // Selections stay exclusive across lanes.
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, lanePoint(1000));
  const QPoint scenePoint(64 + qRound((timeline->width() - 80) * 3000 / 6000.0),
                          60);
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::ControlModifier, scenePoint);
  if (!require(timeline->selectedClip() != 0 &&
                   timeline->selectedAudioClip() == 0,
               "scene selection did not clear the audio selection"))
    return false;
  window.close();
  if (!require(QTest::qWaitFor([&] { return !window.isVisible(); }, 7000),
               "audio project did not close"))
    return false;
  const auto saved = loadStudioProject(document);
  if (!require(saved.error.isEmpty() && saved.project.audioClips.size() == 2,
               "audio lane did not survive a save"))
    return false;
  // A lane shorter than the video leaves empty space; clicking it clears.
  StudioProject shortLane;
  shortLane.assets = {{1, base, probeStudioSource(base)}};
  shortLane.assets.push_back({2, song, songSource});
  shortLane.canvas = {320, 180};
  shortLane.clips = {{1, 1, 0, 6000, 1}};
  shortLane.audioClips = {{11, 2, 0, 1500, 1.0, 100}};
  const QString shortDocument =
      scratch.filePath(QStringLiteral("short-audio.omasnap.json"));
  if (!saveStudioProject(shortDocument, shortLane).isEmpty())
    return false;
  StudioWindow shortWindow(shortDocument, nullptr,
                           scratch.filePath("palette.toml"));
  shortWindow.show();
  auto *shortPlayer = shortWindow.findChild<StudioPlayback *>();
  auto *shortTimeline = shortWindow.findChild<StudioTimeline *>();
  if (!require(shortTimeline &&
                   QTest::qWaitFor(
                       [&] { return shortPlayer->duration() == 6000; }, 6000),
               "short audio lane did not load"))
    return false;
  const auto shortPoint = [&](qint64 ms) {
    return QPoint(64 + qRound((shortTimeline->width() - 80) * ms / 6000.0),
                  147);
  };
  QTest::mouseClick(shortTimeline, Qt::LeftButton, Qt::NoModifier,
                    shortPoint(1000));
  if (!require(shortTimeline->selectedAudioClip() == 11,
               "short lane click did not select"))
    return false;
  QTest::mouseClick(shortTimeline, Qt::LeftButton, Qt::NoModifier,
                    shortPoint(4000));
  if (!require(shortTimeline->selectedAudioClip() == 0,
               "empty lane click kept the audio selection"))
    return false;
  shortWindow.close();
  return require(QTest::qWaitFor([&] { return !shortWindow.isVisible(); }, 7000),
                 "short audio project did not close");
}
