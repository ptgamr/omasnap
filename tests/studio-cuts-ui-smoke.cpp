#include "studio-cuts-ui-smoke.hpp"
#include "studio-playback.hpp"
#include "studio.hpp"
#include <QFile>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

bool runStudioCutsUiChecks(const QString &source, QString &error) {
  QTemporaryDir scratch;
  const auto path = scratch.filePath("cuts.mp4");
  if (!QFile::copy(source, path))
    return false;
  StudioWindow window(path, nullptr, scratch.filePath("palette.toml"));
  window.show();
  auto *timeline = window.findChild<StudioTimeline *>();
  auto *player = window.findChild<StudioPlayback *>();
  const auto require = [&error](bool ok, const char *why) {
    if (!ok)
      error = QString::fromLatin1(why);
    return ok;
  };
  if (!require(
          QTest::qWaitFor([&] { return player->duration() == 6000; }, 6000),
          "cut UI source did not load"))
    return false;
  player->setPosition(5000);
  const auto point = [&](qint64 ms) {
    return QPoint(64 + qRound((timeline->width() - 80) * ms / 6000.0), 60);
  };
  // Reverse drag and subsequent endpoint resize must both be unambiguous.
  timeline->scrubbed(4500);
  QTest::mousePress(timeline, Qt::LeftButton, Qt::ShiftModifier, point(3000));
  QTest::mouseMove(timeline, point(1000));
  QTest::mouseRelease(timeline, Qt::LeftButton, Qt::ShiftModifier, point(1000));
  if (!require(timeline->rangeOut() > timeline->rangeIn() &&
                   timeline->rangeIn() >= 990,
               "reverse drag did not select passage"))
    return false;
  QTest::mousePress(timeline, Qt::LeftButton, Qt::NoModifier, point(1000));
  QTest::mouseMove(timeline, point(1500));
  QTest::mouseRelease(timeline, Qt::LeftButton, Qt::NoModifier, point(1500));
  const qint64 from = timeline->rangeIn(), to = timeline->rangeOut();
  for (auto *button : window.findChildren<QPushButton *>())
    if (button->text() == QStringLiteral("Undo") &&
        !require(!button->isEnabled(),
                 "Selection alone created an undoable project edit"))
      return false;
  if (!require(from >= 1490 && from <= 1510 && to >= 2990,
               "range endpoint could not be adjusted"))
    return false;
  QPushButton *selectTool = nullptr, *rangeTool = nullptr, *split = nullptr;
  for (auto *button : window.findChildren<QPushButton *>()) {
    if (button->text() == QStringLiteral("Select · V")) selectTool = button;
    if (button->text() == QStringLiteral("Range · B")) rangeTool = button;
    if (button->text() == QStringLiteral("Split at playhead · S")) split = button;
  }
  if (!require(!selectTool && !rangeTool && split && !split->isCheckable(),
               "obsolete mode buttons remain or split action is missing"))
    return false;
  if (!require(player->position() == from && player->playbackState() == QMediaPlayer::PausedState,
               "range gesture did not park the playhead at its start")) return false;
  QTest::keyClick(&window, Qt::Key_End);
  if (!require(player->position() == to - 1, "End escaped selection")) return false;
  QTest::keyClick(&window, Qt::Key_Right, Qt::ShiftModifier);
  if (!require(player->position() == to - 1, "forward seek escaped selection")) return false;
  QTest::keyClick(&window, Qt::Key_Home);
  QTest::keyClick(&window, Qt::Key_Left);
  if (!require(player->position() == from, "backward frame step escaped selection")) return false;
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, point(5500));
  if (!require(QTest::qWaitFor([&] { return player->position() == to - 1; }, 4000) &&
                   timeline->hasRange(), "outside timeline click cleared or escaped selection")) return false;
  timeline->setTrim(0, 2000);
  QTest::keyClick(&window, Qt::Key_Space);
  if (!require(QTest::qWaitFor([&] { return player->position() > from + 200; }, 3000),
               "normal Space did not start playback inside selection")) return false;
  QTest::keyClick(&window, Qt::Key_Space);
  const qint64 paused = player->position();
  if (!require(player->playbackState() == QMediaPlayer::PausedState && paused < to,
               "normal Space did not pause range playback")) return false;
  QTest::keyClick(&window, Qt::Key_Space);
  if (!require(player->position() == paused, "range resume restarted instead of resuming")) return false;
  if (!require(QTest::qWaitFor([&] {
        return player->playbackState() == QMediaPlayer::PausedState && player->position() == to - 1;
      }, 6000), "selected range did not play once and stop inside its end")) return false;
  if (!require(timeline->trimIn() == 0 && timeline->trimOut() == 2000,
               "range preview changed the export range")) return false;
  timeline->setTrim(0, 6000);
  timeline->setRange(5500, 6000);
  QTest::keyClick(&window, Qt::Key_Space);
  if (!require(QTest::qWaitFor([&] {
        return player->playbackState() == QMediaPlayer::PausedState && player->position() == 5999;
      }, 4000), "range preview at project EOF did not settle inside the selection")) return false;
  timeline->setRange(from, to);
  if (!require(!window.findChild<QWidget *>("rangeControls"),
               "separate range controls still occupy the timeline")) return false;
  player->setPosition(5000);
  QTest::keyClick(&window, Qt::Key_Delete);
  if (!require(player->duration() == 6000 - (to - from) &&
                   timeline->rangeIn() == -1,
               "Delete did not ripple the selected range"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 6000 && timeline->rangeIn() == from &&
                   timeline->rangeOut() == to && player->position() == to - 1,
               "Undo did not restore deleted range and playhead"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  if (!require(player->duration() == 6000 - (to - from),
               "Redo did not restore range deletion"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  QTest::keyClick(&window, Qt::Key_Escape);
  if (!require(!timeline->hasRange(), "Escape did not clear the range"))
    return false;
  player->setPosition(0);
  if (!require(player->position() == 0, "Escape did not release range seeking constraint")) return false;
  player->setPosition(2000);
  QTest::keyClick(&window, Qt::Key_S);
  if (!require(player->duration() == 6000 && timeline->selectedClip() != 0,
               "Split changed duration or did not select incoming clip"))
    return false;
  QTest::keyClick(&window, Qt::Key_Delete);
  if (!require(player->duration() == 2000,
               "Delete did not remove selected split clip"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  // An immediate split uses the latest coalesced scrub, not the previous
  // decoded position. Undo must also cancel any old-timeline pending seek.
  timeline->scrubbed(2500);
  QTest::keyClick(&window, Qt::Key_S);
  QTest::keyClick(&window, Qt::Key_Delete);
  if (!require(player->duration() == 2500,
               "Immediate split did not flush the pending scrub"))
    return false;
  timeline->scrubbed(900);
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  QTest::qWait(60);
  if (!require(player->position() == 2500,
               "Old-timeline scrub overwrote the undo playhead"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, point(3000));
  QTest::qWait(60);
  QTest::keyClick(&window, Qt::Key_Backspace);
  if (!require(player->duration() == 0,
               "Deleting final clip did not yield empty project"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 6000,
               "Final clip deletion was not undoable"))
    return false;
  timeline->setSelectedClip(1);
  QTest::keyClick(&window, Qt::Key_D, Qt::ControlModifier);
  if (!require(player->duration() == 12000, "could not create long project for range undo")) return false;
  timeline->setRange(9000, 10000);
  player->setPosition(9500);
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 6000 && player->position() < 6000 && !timeline->hasRange(),
               "old range constrained undo into a shorter project")) return false;
  // Editing a text field must not delete video or split scenes.
  auto *text = new QLineEdit(&window);
  text->setText("test");
  text->show();
  text->setFocus();
  text->selectAll();
  QTest::keyClick(text, Qt::Key_Delete);
  if (!require(text->text().isEmpty() && player->duration() == 6000,
               "Text Delete leaked into video deletion"))
    return false;
  window.close();
  return require(QTest::qWaitFor([&] { return !window.isVisible(); }, 5000),
                 "Cut project did not save on close");
}
