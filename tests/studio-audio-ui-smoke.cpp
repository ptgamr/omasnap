/** @fileoverview Audio lane: waveform paint and clip selection. */
#include "studio-audio-ui-smoke.hpp"
#include "studio-playback.hpp"
#include "studio-project.hpp"
#include "studio.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QSlider>
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
  project.audioClips = {{11, 2, 0, 1500, 1.0, 0},
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
  auto *gain = window.findChild<QSlider *>(QStringLiteral("audioGain"));
  auto *gainValue =
      window.findChild<QLabel *>(QStringLiteral("audioGainValue"));
  if (!require(gain && gainValue, "audio gain controls are missing"))
    return false;
  // The fixture clip sits at zero gain, selected first: the slider starts
  // at zero too, so no signal fires and only the explicit sync can read 0%.
  if (!require(gain->value() == 0 &&
                   gainValue->text() == QStringLiteral("0%"),
               "zero gain reads 100%"))
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
  // Trim the first sound shorter by its right edge.
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, lanePoint(500));
  const QPoint trimPress(
      64 + qRound((timeline->width() - 80) * 1480 / 6000.0), 147);
  const QPoint trimDrop(
      64 + qRound((timeline->width() - 80) * 1000 / 6000.0), 147);
  QTest::mousePress(timeline, Qt::LeftButton, Qt::NoModifier, trimPress);
  QTest::mouseMove(timeline, trimDrop);
  QTest::mouseRelease(timeline, Qt::LeftButton, Qt::NoModifier, trimDrop);
  if (!require(timeline->selectedAudioClip() == 11,
               "trim lost the audio selection"))
    return false;
  // Move the long sound first by its body.
  const QPoint movePress(
      64 + qRound((timeline->width() - 80) * 3000 / 6000.0), 147);
  const QPoint moveDrop(
      64 + qRound((timeline->width() - 80) * 200 / 6000.0), 147);
  QTest::mousePress(timeline, Qt::LeftButton, Qt::NoModifier, movePress);
  QTest::mouseMove(timeline, moveDrop);
  QTest::mouseRelease(timeline, Qt::LeftButton, Qt::NoModifier, moveDrop);
  if (!require(timeline->selectedAudioClip() == 12,
               "move lost the audio selection"))
    return false;
  // The reordered lane reaches playback: 500 ms is the epic now, not the
  // song the stale order would play there.
  player->setPosition(500);
  auto *follower = player->audioPlayerForTest();
  if (!require(QTest::qWaitFor(
                   [&] {
                     return follower->source().toLocalFile() == epic;
                   },
                   5000),
               "reordered lane did not reach playback"))
    return false;
  // Split the long sound at the playhead, then delete the tail.
  player->setPosition(3000);
  QTest::keyClick(&window, Qt::Key_S);
  if (!require(timeline->selectedAudioClip() == 13,
               "split did not select the incoming sound"))
    return false;
  QTest::keyClick(&window, Qt::Key_Delete);
  if (!require(timeline->selectedAudioClip() == 0,
               "delete kept the audio selection"))
    return false;
  // Duplicate the selected sound, then tune the copy.
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, lanePoint(500));
  QTest::keyClick(&window, Qt::Key_D, Qt::ControlModifier);
  if (!require(timeline->selectedAudioClip() == 14,
               "duplicate did not select the new sound"))
    return false;
  auto *speed = window.findChild<QComboBox *>(QStringLiteral("audioSpeed"));
  if (!require(speed, "audio speed control is missing"))
    return false;
  gain->setValue(50);
  const int twice = speed->findText(QStringLiteral("2×"));
  if (!require(twice >= 0, "2x audio speed preset missing"))
    return false;
  speed->setCurrentIndex(twice);
  speed->activated(twice);
  // Undo restores the speed; redo brings the tuned copy back for saving.
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(speed->currentText() == QStringLiteral("1×"),
               "audio speed change was not undoable"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z,
                  Qt::ControlModifier | Qt::ShiftModifier);
  if (!require(speed->currentText() == QStringLiteral("2×"),
               "audio speed change was not redoable"))
    return false;
  // S with the playhead outside the selected sound refuses instead of
  // cutting whatever sits there.
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, lanePoint(4750));
  player->setPosition(1000);
  QTest::keyClick(&window, Qt::Key_S);
  if (!require(timeline->selectedAudioClip() == 11,
               "split cut the unselected sound"))
    return false;
  // Gain drags stay live mid-gesture as one undo step, readout included.
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, lanePoint(4250));
  if (!require(timeline->selectedAudioClip() == 14,
               "gain target did not select"))
    return false;
  const QPoint grip(gain->rect().left() + 10, gain->rect().center().y());
  QTest::mousePress(gain, Qt::LeftButton, Qt::NoModifier, grip);
  QTest::mouseMove(gain, grip + QPoint(20, 0));
  if (!require(gain->isEnabled() && gain->value() != 50,
               "gain drag stalled mid-gesture"))
    return false;
  QTest::mouseMove(gain, grip + QPoint(40, 0));
  QTest::mouseRelease(gain, Qt::LeftButton, Qt::NoModifier,
                      grip + QPoint(40, 0));
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(gain->value() == 50 &&
                   gainValue->text() == QStringLiteral("50%"),
               "gain drag was not one undo step"))
    return false;
  window.close();
  if (!require(QTest::qWaitFor([&] { return !window.isVisible(); }, 7000),
               "audio project did not close"))
    return false;
  const auto saved = loadStudioProject(document);
  const bool orderOk = saved.project.audioClips.size() == 3 &&
                       saved.project.audioClips[0].id == 12 &&
                       saved.project.audioClips[1].id == 14 &&
                       saved.project.audioClips[2].id == 11;
  const auto &tuned =
      saved.project.audioClips.size() == 3 ? saved.project.audioClips[1]
                                           : saved.project.audioClips[0];
  const auto &trimmed =
      saved.project.audioClips.size() == 3 ? saved.project.audioClips[2]
                                           : saved.project.audioClips[0];
  // The duplicate copies the split clip 12 (outMs exactly 3000: the
  // split boundary is playhead minus span start at speed 1), then gains 50
  // and doubles speed; the trimmed clip 11 keeps its dragged-out range.
  if (!require(saved.error.isEmpty() && orderOk &&
                   trimmed.outMs >= 985 && trimmed.outMs <= 1015 &&
                   tuned.gain == 50 && tuned.speed == 2.0 &&
                   tuned.inMs == 0 && tuned.outMs == 3000,
               "audio gestures did not persist"))
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
  if (!require(QTest::qWaitFor([&] { return !shortWindow.isVisible(); }, 7000),
               "short audio project did not close"))
    return false;
  // Unified import: audio drops land on the lane, mixed drops split by
  // probe, and junk changes nothing.
  StudioWindow importWindow(base, nullptr, scratch.filePath("palette.toml"));
  importWindow.show();
  auto *importPlayer = importWindow.findChild<StudioPlayback *>();
  auto *importTimeline = importWindow.findChild<StudioTimeline *>();
  if (!require(importTimeline &&
                   QTest::qWaitFor(
                       [&] { return importPlayer->duration() == 6000; }, 6000),
               "import window did not load"))
    return false;
  const auto dropFiles = [&](const QStringList &paths) {
    QMimeData mime;
    QList<QUrl> urls;
    for (const auto &path : paths)
      urls.push_back(QUrl::fromLocalFile(path));
    mime.setUrls(urls);
    const QPoint point(100, 100);
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(&importWindow, &enter);
    if (!enter.isAccepted())
      return false;
    QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&importWindow, &drop);
    return drop.isAccepted();
  };
  const auto laneSelects = [&](qint64 ms) {
    const QPoint at(64 + qRound((importTimeline->width() - 80) * ms / 6000.0),
                    147);
    QTest::mouseClick(importTimeline, Qt::LeftButton, Qt::NoModifier, at);
    return importTimeline->selectedAudioClip();
  };
  if (!require(dropFiles({song}), "audio drop was rejected") ||
          !require(QTest::qWaitFor(
                       [&] {
                         return importTimeline->audioPeaksFor(2) != nullptr;
                       },
                       10000),
                   "dropped audio never decoded") ||
          !require(laneSelects(1000) != 0, "dropped audio did not land"))
    return false;
  // The picker offers audio alongside video.
  QTest::keyClick(&importWindow, Qt::Key_O, Qt::ControlModifier);
  auto *mediaPicker = importWindow.findChild<QFileDialog *>();
  if (!require(mediaPicker && mediaPicker->isVisible(),
               "Ctrl+O did not open Add media"))
    return false;
  if (!require(mediaPicker->nameFilters()
                   .join(QStringLiteral(" "))
                   .contains(QStringLiteral("mp3")),
               "media picker hides audio"))
    return false;
  mediaPicker->reject();
  QTest::qWait(50);
  const QString second = scratch.filePath(QStringLiteral("second.mp4"));
  const QString song2 = scratch.filePath(QStringLiteral("song2.wav"));
  if (!require(QFile::copy(base, second) && writeSquareWav(song2, 1, 1),
               "could not stage mixed fixtures"))
    return false;
  if (!require(dropFiles({second, song2}), "mixed drop was rejected") ||
          !require(QTest::qWaitFor(
                       [&] { return importPlayer->duration() == 12000; },
                       10000),
                   "mixed drop did not append the scene"))
    return false;
  const QPoint mixedAt(
      64 + qRound((importTimeline->width() - 80) * 2500 / 12000.0), 147);
  QTest::mouseClick(importTimeline, Qt::LeftButton, Qt::NoModifier, mixedAt);
  if (!require(importTimeline->selectedAudioClip() != 0,
               "mixed drop did not append the sound"))
    return false;
  // Cover art is not a picture to edit: the probe keeps songs with
  // pictures on the lane, and the drop adds no scene.
  const QString ffmpeg =
      QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (!require(!ffmpeg.isEmpty(), "ffmpeg missing for cover fixture"))
    return false;
  const QString cover = scratch.filePath(QStringLiteral("cover.mp3"));
  {
    QProcess picture;
    picture.start(
        ffmpeg,
        {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-y"),
         QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
         QStringLiteral("sine=frequency=880:sample_rate=48000:d=2"),
         QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
         QStringLiteral("color=red:s=64x64:d=1"), QStringLiteral("-map"),
         QStringLiteral("0:a:0"), QStringLiteral("-map"),
         QStringLiteral("1:v:0"), QStringLiteral("-c:a"),
         QStringLiteral("libmp3lame"), QStringLiteral("-c:v"),
         QStringLiteral("mjpeg"), QStringLiteral("-disposition:v:1"),
         QStringLiteral("attached_pic"), QStringLiteral("-id3v2_version"),
         QStringLiteral("3"), cover});
    if (!require(picture.waitForFinished(30000) &&
                     picture.exitStatus() == QProcess::NormalExit &&
                     picture.exitCode() == 0,
                 "could not stage cover-art fixture"))
      return false;
  }
  const StudioSource coverSource = probeStudioSource(cover);
  if (!require(!coverSource.usable() && coverSource.usableAudio(),
               "cover art probed as a video source"))
    return false;
  // Asset 5: base, song, second, song2 took 1..4.
  if (!require(dropFiles({cover}), "cover-art drop was rejected") ||
          !require(QTest::qWaitFor(
                       [&] {
                         return importTimeline->audioPeaksFor(5) != nullptr;
                       },
                       10000),
                   "cover-art song never decoded"))
    return false;
  if (!require(importPlayer->duration() == 12000,
               "cover art added a scene"))
    return false;
  const QPoint coverAt(
      64 + qRound((importTimeline->width() - 80) * 4000 / 12000.0), 147);
  QTest::mouseClick(importTimeline, Qt::LeftButton, Qt::NoModifier, coverAt);
  if (!require(importTimeline->selectedAudioClip() != 0,
               "cover-art song did not land on the lane"))
    return false;
  // Junk changes nothing but the status line.
  const QString junk = scratch.filePath(QStringLiteral("note.txt"));
  {
    QFile note(junk);
    if (!require(note.open(QIODevice::WriteOnly) && note.write("hi") == 2,
                 "could not stage junk fixture"))
      return false;
  }
  auto *addMedia = importWindow.findChild<QPushButton *>(QStringLiteral("addScenes"));
  auto *importStatus =
      importWindow.findChild<QLabel *>(QStringLiteral("studioStatus"));
  if (!require(addMedia && importStatus, "import controls are missing"))
    return false;
  if (!require(dropFiles({junk}), "junk drop was rejected") ||
          !require(QTest::qWaitFor([&] { return addMedia->isEnabled(); },
                                   10000) &&
                       importPlayer->duration() == 12000 &&
                       importStatus->text().contains(
                           QStringLiteral("supported")),
                   "junk drop changed the project"))
    return false;
  // Relinking an audio asset recovers its lane without touching scenes.
  StudioProject lost;
  lost.assets = {{1, base, probeStudioSource(base)}};
  StudioSource goneSource;
  goneSource.durationMs = 2000;
  goneSource.audioStreams = 1;
  lost.assets.push_back(
      {2, scratch.filePath(QStringLiteral("gone.wav")), goneSource});
  lost.canvas = {320, 180};
  lost.clips = {{1, 1, 0, 6000, 1}};
  lost.audioClips = {{11, 2, 0, 2000, 1.0, 100}};
  const QString lostDocument =
      scratch.filePath(QStringLiteral("lost-audio.omasnap.json"));
  if (!saveStudioProject(lostDocument, lost).isEmpty())
    return false;
  StudioWindow lostWindow(lostDocument, nullptr,
                          scratch.filePath("palette.toml"));
  lostWindow.show();
  QPushButton *relink = nullptr;
  for (auto *button : lostWindow.findChildren<QPushButton *>())
    if (button->text() == QStringLiteral("Relink media"))
      relink = button;
  if (!require(relink && QTest::qWaitFor([&] { return relink->isVisible(); },
                                         7000),
               "missing audio did not offer relink"))
    return false;
  relink->click();
  auto *relinkDialog = lostWindow.findChild<QFileDialog *>();
  if (!require(relinkDialog, "relink picker did not open"))
    return false;
  relinkDialog->fileSelected(song);
  auto *lostTimeline = lostWindow.findChild<StudioTimeline *>();
  if (!require(
          lostTimeline &&
              QTest::qWaitFor(
                  [&] {
                    const auto *peaks = lostTimeline->audioPeaksFor(2);
                    return !relink->isVisible() && peaks != nullptr &&
                           !peaks->isEmpty();
                  },
                  10000),
          "audio relink did not recover the lane"))
    return false;
  lostWindow.close();
  importWindow.close();
  if (!require(QTest::qWaitFor(
                   [&] {
                     return !lostWindow.isVisible() &&
                            !importWindow.isVisible();
                   },
                   7000),
               "import windows did not close"))
    return false;
  // A full video lane never blocks sounds: decoupled caps judge actual
  // additions, not the batch size.
  StudioProject crowded;
  crowded.assets = {{1, base, probeStudioSource(base)}};
  crowded.canvas = {320, 180};
  for (int i = 0; i < 1000; ++i)
    crowded.clips.push_back(
        {static_cast<quint64>(i + 1), 1, 0, 6, 1.0});
  const QString crowdedDocument =
      scratch.filePath(QStringLiteral("crowded.omasnap.json"));
  if (!saveStudioProject(crowdedDocument, crowded).isEmpty())
    return false;
  StudioWindow crowdedWindow(crowdedDocument, nullptr,
                             scratch.filePath("palette.toml"));
  crowdedWindow.show();
  auto *crowdedPlayer = crowdedWindow.findChild<StudioPlayback *>();
  auto *crowdedTimeline = crowdedWindow.findChild<StudioTimeline *>();
  if (!require(crowdedTimeline &&
                   QTest::qWaitFor(
                       [&] { return crowdedPlayer->duration() == 6000; },
                       6000),
               "crowded project did not load"))
    return false;
  const QPoint crowdedDrop(crowdedWindow.width() / 2, 100);
  {
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(song)});
    QDragEnterEvent enter(crowdedDrop, Qt::CopyAction, &mime, Qt::LeftButton,
                          Qt::NoModifier);
    QApplication::sendEvent(&crowdedWindow, &enter);
    if (!require(enter.isAccepted(), "crowded drop was rejected"))
      return false;
    QDropEvent drop(crowdedDrop, Qt::CopyAction, &mime, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&crowdedWindow, &drop);
  }
  if (!require(QTest::qWaitFor(
                   [&] {
                     return crowdedTimeline->audioPeaksFor(2) != nullptr;
                   },
                   10000),
               "sound was refused by a full video lane"))
    return false;
  const QPoint crowdedAt(
      64 + qRound((crowdedTimeline->width() - 80) * 1000 / 6000.0), 147);
  QTest::mouseClick(crowdedTimeline, Qt::LeftButton, Qt::NoModifier,
                    crowdedAt);
  if (!require(crowdedTimeline->selectedAudioClip() != 0 &&
                   crowdedPlayer->duration() == 6000,
               "sound did not land beside a thousand scenes"))
    return false;
  crowdedWindow.close();
  return require(QTest::qWaitFor(
                     [&] { return !crowdedWindow.isVisible(); }, 7000),
                 "crowded project did not close");
}
