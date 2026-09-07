/** @fileoverview Scene import, arrangement, keyboard, and persistence workflow.
 */
#include "studio-scenes-ui-smoke.hpp"
#include "studio-playback.hpp"
#include "studio.hpp"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QMimeData>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

namespace {
bool dropFiles(StudioWindow &window, const QStringList &paths, QPoint point) {
  QMimeData mime;
  QList<QUrl> urls;
  for (const auto &path : paths)
    urls.push_back(QUrl::fromLocalFile(path));
  mime.setUrls(urls);
  QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton,
                        Qt::NoModifier);
  QApplication::sendEvent(&window, &enter);
  if (!enter.isAccepted())
    return false;
  QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&window, &drop);
  return drop.isAccepted();
}
} // namespace

bool runStudioScenesUiChecks(const QString &source, QString &error) {
  QTemporaryDir scratch;
  const auto require = [&error](bool ok, const char *message) {
    if (!ok)
      error = QString::fromLatin1(message);
    return ok;
  };
  QStringList paths;
  for (const auto *name :
       {"initial.mp4", "second.mp4", "third.mp4", "fourth.mp4"}) {
    paths.push_back(scratch.filePath(QString::fromLatin1(name)));
    if (!require(QFile::copy(source, paths.last()),
                 "could not copy scene UI fixtures"))
      return false;
  }
  StudioWindow window(paths[0], nullptr, scratch.filePath("palette.toml"));
  window.show();
  auto *timeline = window.findChild<StudioTimeline *>();
  auto *player = window.findChild<StudioPlayback *>();
  auto *add = window.findChild<QPushButton *>("addScenes");
  auto *duplicate = window.findChild<QPushButton *>("duplicateScene");
  auto *in = window.findChild<QSpinBox *>("sceneIn");
  auto *out = window.findChild<QSpinBox *>("sceneOut");
  if (!require(timeline && player && add && duplicate && in && out,
               "scene controls are missing") ||
      !require(
          QTest::qWaitFor(
              [&] { return player->duration() == 6000 && add->isEnabled(); },
              7000),
          "scene UI source did not load"))
    return false;
  // The picker is asynchronous and has a visible keyboard route. Cancelling
  // must not create a scene or an undo entry.
  QTest::keyClick(&window, Qt::Key_O, Qt::ControlModifier);
  auto *dialog = window.findChild<QFileDialog *>();
  if (!require(dialog && dialog->isVisible(), "Ctrl+O did not open Add scenes"))
    return false;
  dialog->reject();
  QTest::qWait(30);
  auto *tabs = window.findChild<QTabWidget *>();
  for (int i = 0; tabs && i < tabs->count(); ++i)
    if (tabs->tabText(i) == QStringLiteral("Clip"))
      tabs->setCurrentIndex(i);
  QTest::qWait(30);
  auto *page = window.findChild<QWidget *>("studioScenePage");
  auto *theme = window.findChild<StudioTheme *>();
  if (!require(page && theme &&
                   page->grab().toImage().pixelColor(2, 2) ==
                       theme->chrome().background,
               "scene inspector fell back to desktop palette background"))
    return false;
  if (!require(dropFiles(window, paths.mid(1), {100, 100}),
               "local video file drop was rejected") ||
      !require(
          QTest::qWaitFor(
              [&] { return player->duration() == 24000 && add->isEnabled(); },
              7000),
          "three-file drop did not append three scenes"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 6000,
               "one undo did not remove the complete import batch"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  if (!require(player->duration() == 24000,
               "redo did not restore the import batch"))
    return false;
  timeline->setSelectedClip(2);
  QTest::keyClick(&window, Qt::Key_D, Qt::ControlModifier);
  if (!require(player->duration() == 30000 && timeline->selectedClip() == 5,
               "Ctrl+D did not duplicate and select the scene"))
    return false;
  in->setValue(500);
  out->setValue(5500);
  if (!require(player->duration() == 29000 && in->value() == 500 &&
                   out->value() == 5500,
               "scene in/out controls did not trim only the duplicate"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 30000,
               "scene edge edits were not undoable"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  for (const auto *label : {"Earlier", "Later"}) {
    QPushButton *order = nullptr;
    for (auto *button : window.findChildren<QPushButton *>())
      if (button->text() == QLatin1String(label))
        order = button;
    if (!require(order && order->isEnabled(),
                 "scene ordering button was unavailable"))
      return false;
    order->click();
  }
  const auto point = [&](qint64 ms) {
    return QPoint(64 + qRound((timeline->width() - 80) * ms / 29000.0), 60);
  };
  QTest::mousePress(timeline, Qt::LeftButton, Qt::NoModifier, point(12000));
  QTest::mouseMove(timeline, point(12500), 40);
  QTest::mouseRelease(timeline, Qt::LeftButton, Qt::NoModifier, point(12500));
  if (!require(player->duration() < 28700 && player->duration() > 28300 &&
                   in->value() > 800 && out->value() == 5500,
               "selected scene edge drag did not trim its source range"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 29000 && in->value() == 500 &&
                   out->value() == 5500,
               "one undo did not restore the scene edge drag"))
    return false;
  // Move the duplicate from between second/third scenes to the final boundary.
  QTest::mousePress(timeline, Qt::LeftButton, Qt::ControlModifier,
                    point(14500));
  QTest::mouseMove(timeline, point(28500), 40);
  QTest::mouseRelease(timeline, Qt::LeftButton, Qt::ControlModifier,
                      point(28500));
  if (!require(player->duration() == 29000 && timeline->selectedClip() == 5,
               "scene drag changed duration or selection"))
    return false;
  const QPoint boundary = timeline->mapTo(&window, point(6000));
  if (!require(timeline->insertionBefore(point(6000).x()) == 2 &&
                   dropFiles(window, {paths[3]}, boundary),
               "drop on a scene boundary was rejected") ||
      !require(
          QTest::qWaitFor(
              [&] { return player->duration() == 35000 && add->isEnabled(); },
              7000),
          "boundary drop did not insert the scene"))
    return false;
  const int insertedBoundary =
      64 + qRound((timeline->width() - 80) * 6000 / 35000.0);
  if (!require(
          timeline->insertionBefore(insertedBoundary) == 6,
          "boundary drop appended instead of inserting before second scene"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 29000,
               "boundary insertion was not undoable"))
    return false;
  // A partly-valid batch is atomic: not even its first good file may land.
  if (!require(dropFiles(window, {paths[1], scratch.filePath("missing.mp4")},
                         {100, 100}),
               "invalid import batch was not accepted for asynchronous "
               "validation") ||
      !require(QTest::qWaitFor([&] { return add->isEnabled(); }, 7000) &&
                   player->duration() == 29000,
               "invalid import partially changed the project"))
    return false;
  QTest::keyClick(in, Qt::Key_Space);
  if (!require(player->playbackState() == QMediaPlayer::PlayingState,
               "Space from scene timing control did not toggle playback"))
    return false;
  player->pause();
  window.close();
  if (!require(QTest::qWaitFor([&] { return !window.isVisible(); }, 7000),
               "scene project did not finish saving on close"))
    return false;
  const auto saved = loadStudioProject(paths[0] + ".omasnap.json");
  if (!require(saved.error.isEmpty() && saved.project.clips.size() == 5,
               "saved scene project is invalid"))
    return false;
  QVector<quint64> ids;
  for (const auto &clip : saved.project.clips)
    ids.push_back(clip.id);
  if (!require(ids == QVector<quint64>{1, 2, 3, 4, 5} &&
                   saved.project.clips[1].inMs == 0 &&
                   saved.project.clips[1].outMs == 6000 &&
                   saved.project.clips[4].inMs == 500 &&
                   saved.project.clips[4].outMs == 5500,
               "drag order or independent source ranges did not persist"))
    return false;
  for (int i = 0; i < 4; ++i) {
    const auto *asset =
        studioAsset(saved.project, saved.project.clips[i].assetId);
    if (!require(asset && asset->path == paths[i],
                 "multi-file import order changed"))
      return false;
  }
  StudioWindow reopened(paths[0] + ".omasnap.json", nullptr,
                        scratch.filePath("palette.toml"));
  reopened.show();
  auto *again = reopened.findChild<StudioPlayback *>();
  if (!require(QTest::qWaitFor(
                   [&] { return again && again->duration() == 29000; }, 7000),
               "combined scenes did not reopen"))
    return false;
  reopened.close();
  return require(QTest::qWaitFor([&] { return !reopened.isVisible(); }, 7000),
                 "reopened scene project did not close");
}
