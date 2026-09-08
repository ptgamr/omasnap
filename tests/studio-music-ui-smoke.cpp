/** @fileoverview Music dialog: picking, volume, undo, and transport. */
#include "studio-music-ui-smoke.hpp"
#include "studio-playback.hpp"
#include "studio.hpp"
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QMediaPlayer>
#include <QProcess>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

namespace {
bool runTool(const QString &tool, const QStringList &arguments) {
  QProcess process;
  process.start(tool, arguments);
  if (!process.waitForFinished(30000)) {
    process.kill();
    process.waitForFinished(1000);
    return false;
  }
  return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
} // namespace

bool runStudioMusicUiChecks(const QString &source, QString &error) {
  const QString ffmpeg =
      QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (ffmpeg.isEmpty())
    return true;
  QTemporaryDir scratch;
  const auto path = scratch.filePath(QStringLiteral("music.mp4"));
  if (!QFile::copy(source, path))
    return false;
  const QString song = scratch.filePath(QStringLiteral("song.m4a"));
  if (!runTool(ffmpeg, {QStringLiteral("-v"), QStringLiteral("error"), "-f",
                        "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                        "-t", "5", "-c:a", "aac", song})) {
    error = QStringLiteral("could not generate music fixture");
    return false;
  }
  StudioWindow window(path, nullptr, scratch.filePath("palette.toml"));
  window.show();
  auto *player = window.findChild<StudioPlayback *>();
  const auto require = [&error](bool ok, const char *why) {
    if (!ok)
      error = QString::fromLatin1(why);
    return ok;
  };
  if (!require(
          QTest::qWaitFor([&] { return player->duration() == 6000; }, 6000),
          "music UI source did not load"))
    return false;
  auto *musicButton =
      window.findChild<QPushButton *>(QStringLiteral("studioMusic"));
  if (!require(musicButton && musicButton->isEnabled(),
               "music button missing or disabled"))
    return false;
  musicButton->click();
  auto *dialog = window.findChild<QDialog *>(QStringLiteral("studioMusicDialog"));
  if (!require(dialog && dialog->isVisible(), "music dialog did not open"))
    return false;
  auto *fileLabel = dialog->findChild<QLabel *>(QStringLiteral("musicFileName"));
  auto *volume = dialog->findChild<QSlider *>(QStringLiteral("musicVolume"));
  auto *volumeValue =
      dialog->findChild<QLabel *>(QStringLiteral("musicVolumeValue"));
  auto *choose = dialog->findChild<QPushButton *>(QStringLiteral("musicChoose"));
  auto *remove = dialog->findChild<QPushButton *>(QStringLiteral("musicRemove"));
  if (!require(fileLabel && volume && volumeValue && choose && remove,
               "music dialog controls are missing"))
    return false;
  if (!require(fileLabel->text() == QStringLiteral("None") &&
                   volume->value() == 20 &&
                   volumeValue->text() == QStringLiteral("20%") &&
                   !remove->isEnabled(),
               "music dialog did not start empty"))
    return false;
  // Cancelling the picker leaves the silence alone.
  choose->click();
  if (auto *picker = window.findChild<QFileDialog *>()) {
    picker->reject();
    QTest::qWait(50);
  }
  if (!require(fileLabel->text() == QStringLiteral("None"),
               "cancelled music pick changed the project"))
    return false;
  // A video-only file has container duration but no audio bed to mix.
  choose->click();
  if (auto *videoPicker = window.findChild<QFileDialog *>()) {
    videoPicker->fileSelected(source);
    videoPicker->close();
  }
  auto *status = window.findChild<QLabel *>(QStringLiteral("studioStatus"));
  if (!require(status && QTest::qWaitFor([&] { return choose->isEnabled(); },
                                         10000) &&
                   fileLabel->text() == QStringLiteral("None") &&
                   status->text().contains(QStringLiteral("readable audio")),
               "video-only file was accepted as music"))
    return false;
  // Picking probes the duration off the GUI thread, then commits one edit.
  choose->click();
  auto *picker = window.findChild<QFileDialog *>();
  if (!require(picker, "music picker did not open"))
    return false;
  picker->fileSelected(song);
  picker->close();
  if (!require(QTest::qWaitFor(
                   [&] {
                     return fileLabel->text() == QStringLiteral("song.m4a");
                   },
                   10000),
               "picked music never landed in the project"))
    return false;
  if (!require(remove->isEnabled(), "music remove stayed disabled"))
    return false;
  // Transport plays on with the song underneath; removing the volume to
  // zero and back is one gesture each only through the slider signals.
  QTest::keyClick(&window, Qt::Key_Space);
  if (!require(QTest::qWaitFor(
                   [&] {
                     return player->playbackState() ==
                            QMediaPlayer::PlayingState;
                   },
                   4000),
               "transport did not play with music set"))
    return false;
  player->pause();
  volume->setValue(50);
  if (!require(volume->value() == 50, "music volume did not apply"))
    return false;
  // Undo restores the volume first, then the silence: two edits in.
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(volume->value() == 20 &&
                   volumeValue->text() == QStringLiteral("20%") &&
                   fileLabel->text() == QStringLiteral("song.m4a"),
               "volume change was not a separate undo step"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(fileLabel->text() == QStringLiteral("None") &&
                   !remove->isEnabled(),
               "music pick was not undoable"))
    return false;
  dialog->accept();
  window.close();
  return require(QTest::qWaitFor([&] { return !window.isVisible(); }, 7000),
                 "music project did not close");
}
