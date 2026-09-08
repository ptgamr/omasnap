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
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
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
  if (!require(timeline && player && add,
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
  auto *panel = window.findChild<QWidget *>("studioInspector");
  auto *theme = window.findChild<StudioTheme *>();
  if (!require(panel && theme &&
                   panel->grab().toImage().pixelColor(2, 2) ==
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
  // Layouts settle asynchronously: the tweak card must end up paired with
  // the timeline row at the column bottom.
  QTest::qWait(200);
  {
    auto *inspector = window.findChild<QWidget *>("studioInspector");
    auto *clipCard = window.findChild<QWidget *>("clipCard");
    if (!require(inspector && clipCard, "tweak cards are missing"))
      return false;
    const int bottom = clipCard->y() + clipCard->height();
    if (!require(bottom <= inspector->height() &&
                    bottom >= inspector->height() - 18 - 8,
                 "tweak card is not bottom-aligned with the timeline row"))
      return false;
  }
  if (!require(player->duration() == 30000 && timeline->selectedClip() == 5,
               "Ctrl+D did not duplicate and select the scene"))
    return false;
  const auto point = [&](qint64 ms) {
    return QPoint(64 + qRound((timeline->width() - 80) * ms / 30000.0), 60);
  };
  QTest::mousePress(timeline, Qt::LeftButton, Qt::NoModifier, point(12000));
  QTest::mouseMove(timeline, point(12500), 40);
  QTest::mouseRelease(timeline, Qt::LeftButton, Qt::NoModifier, point(12500));
  if (!require(player->duration() < 29700 && player->duration() > 29300 &&
                  timeline->selectedClip() == 5,
               "selected scene edge drag did not trim its source range"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 30000,
               "one undo did not restore the scene edge drag"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  if (!require(player->duration() < 29700 && player->duration() > 29300,
               "redo did not restore the scene edge drag"))
    return false;
  const qint64 trimmed = player->duration();
  // Move the duplicate from between second/third scenes to the final boundary.
  QTest::mousePress(timeline, Qt::LeftButton, Qt::ControlModifier,
                    point(14500));
  QTest::mouseMove(timeline, point(28500), 40);
  QTest::qWait(30);
  const auto dragged = timeline->grab().toImage();
  const QPoint sourcePixel = (QPointF(point(14500)) * dragged.devicePixelRatio()).toPoint();
  if (!require(timeline->cursor().shape() == Qt::ClosedHandCursor &&
                   dragged.pixelColor(sourcePixel) == theme->chrome().background,
               "reorder drag did not lift the source clip into a moving preview"))
    return false;
  QTest::mouseRelease(timeline, Qt::LeftButton, Qt::ControlModifier,
                      point(28500));
  if (!require(player->duration() == trimmed && timeline->selectedClip() == 5,
               "scene drag changed duration or selection"))
    return false;
  const QPoint boundary = timeline->mapTo(&window, point(6000));
  if (!require(timeline->insertionBefore(point(6000).x()) == 2 &&
                   dropFiles(window, {paths[3]}, boundary),
               "drop on a scene boundary was rejected") ||
       !require(
           QTest::qWaitFor(
               [&] { return player->duration() == trimmed + 6000 && add->isEnabled(); },
               7000),
           "boundary drop did not insert the scene"))
    return false;
  const int insertedBoundary =
      64 + qRound((timeline->width() - 80) * 6000 / double(trimmed + 6000));
  if (!require(
          timeline->insertionBefore(insertedBoundary) == 6,
          "boundary drop appended instead of inserting before second scene"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == trimmed,
               "boundary insertion was not undoable"))
    return false;
  // The clip menu duplicates the selected scene like Ctrl+D.
  timeline->setSelectedClip(5);
  QTest::mouseClick(timeline, Qt::RightButton, Qt::NoModifier, point(20000));
  auto *clipMenu = timeline->findChild<QMenu *>();
  QAction *duplicateAction = nullptr;
  if (clipMenu)
    for (auto *action : clipMenu->actions())
      if (action->text().startsWith(QStringLiteral("Duplicate scene")))
        duplicateAction = action;
  if (!require(clipMenu && clipMenu->isVisible() && duplicateAction &&
                  duplicateAction->isEnabled(),
               "Context menu did not offer scene duplication"))
    return false;
  duplicateAction->trigger();
  if (!require(player->duration() > trimmed + 5000 &&
                  player->duration() < trimmed + 6000 &&
                  timeline->selectedClip() != 0 &&
                  timeline->selectedClip() != 5,
               "Menu duplication did not copy and select the scene"))
    return false;
  clipMenu->close();
  if (!require(QTest::qWaitFor(
                    [&] { return timeline->findChild<QMenu *>() == nullptr; },
                    2000),
               "Closed menu lingered"))
    return false;
  window.setFocus();
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == trimmed,
               "Menu duplication was not undoable"))
    return false;
  // A partly-valid batch is atomic: not even its first good file may land.
  if (!require(dropFiles(window, {paths[1], scratch.filePath("missing.mp4")},
                         {100, 100}),
               "invalid import batch was not accepted for asynchronous "
               "validation") ||
       !require(QTest::qWaitFor([&] { return add->isEnabled(); }, 7000) &&
                    player->duration() == trimmed,
               "invalid import partially changed the project"))
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
                   saved.project.clips[4].inMs >= 300 &&
                   saved.project.clips[4].inMs <= 700 &&
                   saved.project.clips[4].outMs == 6000,
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
                   [&] { return again && again->duration() == trimmed; }, 7000),
               "combined scenes did not reopen"))
    return false;
  reopened.close();
  return require(QTest::qWaitFor([&] { return !reopened.isVisible(); }, 7000),
                 "reopened scene project did not close");
}
