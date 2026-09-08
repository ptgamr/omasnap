#include "studio-transitions-ui-smoke.hpp"
#include "studio-playback.hpp"
#include "studio.hpp"
#include <QAbstractItemView>
#include <QAudioOutput>
#include <QMenu>
#include <QFile>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>

bool runStudioTransitionsUiChecks(const QString &source, QString &error) {
  QTemporaryDir scratch;
  const auto path = scratch.filePath("transitions.mp4");
  const auto require = [&error](bool ok, const char *message) {
    if (!ok)
      error = QString::fromLatin1(message);
    return ok;
  };
  if (!require(QFile::copy(source, path), "Could not copy transition fixture"))
    return false;
  StudioWindow window(path, nullptr, scratch.filePath("palette.toml"));
  window.show();
  auto *timeline = window.findChild<StudioTimeline *>();
  auto *player = window.findChild<StudioPlayback *>();
  auto *type = window.findChild<QComboBox *>("transitionType");
  auto *duration = window.findChild<QSpinBox *>("transitionDuration");
  if (!require(
          timeline && player && type && duration &&
              QTest::qWaitFor([&] { return player->duration() == 6000; }, 6000),
          "Transition controls or media unavailable"))
    return false;
  player->setPosition(3000);
  QTest::keyClick(&window, Qt::Key_S);
  timeline->setSelectedClip(1);
  QTest::keyClick(&window, Qt::Key_T);
  // QTest sends to this window explicitly. A live compositor may keep another
  // application active; check the intended focus route without requiring the
  // test to steal desktop focus.
  if (!require(
          QTest::qWaitFor(
              [&] { return type->isEnabled() && window.focusWidget() == type; },
              2000),
          "T did not focus the scene boundary editor"))
    return false;
  timeline->scrubbed(500);
  type->setCurrentIndex(1);
  if (!require(player->duration() == 5700 && duration->value() == 300,
               "Crossfade did not consume a 300ms overlap"))
    return false;
  duration->setValue(600);
  if (!require(player->duration() == 5400,
               "Transition duration was not applied"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 5700 && duration->value() == 300,
               "Undo did not restore previous transition duration"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
  type->setCurrentIndex(2);
  if (!require(player->duration() == 5400 && duration->value() == 600,
               "Fade through black changed overlap duration"))
    return false;
  for (int index = 3; index <= 10; ++index) {
    const int previous = type->currentIndex();
    type->setCurrentIndex(index);
    if (!require(type->currentIndex() == index && player->duration() == 5400 &&
                     duration->value() == 600,
                 "Directional transition changed timing or inspector identity"))
      return false;
    QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
    if (!require(type->currentIndex() == previous && player->duration() == 5400,
                 "Undo did not restore directional transition kind"))
      return false;
    QTest::keyClick(&window, Qt::Key_Z,
                    Qt::ControlModifier | Qt::ShiftModifier);
    if (!require(type->currentIndex() == index,
                 "Redo did not restore directional transition kind"))
      return false;
  }
  type->setCurrentIndex(2);
  type->showPopup();
  QTest::qWait(100);
  const auto popup = type->view()->window()->grab().toImage();
  const auto popupBackground = popup.pixelColor(popup.width() / 2, 3);
  type->hidePopup();
  if (!require(popupBackground == StudioChrome{}.background,
               "Transition popup leaked platform-theme chrome"))
    return false;
  QTest::keyClick(duration, Qt::Key_Space);
  if (!require(player->playbackState() == QMediaPlayer::PlayingState,
               "Space from transition duration did not transport"))
    return false;
  player->pause();
  // Inspector fields keep native editing: hotkeys must not fire from them.
  player->setPosition(1000);
  timeline->setSelectedClip(1);
  if (!require(QTest::qWaitFor([&] { return player->position() == 1000; }, 4000),
               "playhead did not settle before input checks"))
    return false;
  duration->setFocus();
  if (!require(QTest::qWaitFor([&] { return window.focusWidget() == duration; }, 2000),
               "duration field did not take focus"))
    return false;
  QTest::keyClick(duration, Qt::Key_Backspace);
  QTest::keyClick(duration, Qt::Key_S);
  QTest::keyClick(duration, Qt::Key_Z);
  QTest::keyClick(duration, Qt::Key_M);
  QTest::keyClick(duration, Qt::Key_Left);
  if (!require(player->duration() == 5400 && duration->value() == 600 &&
                  timeline->selectedClip() == 1 &&
                  timeline->selectedCue() == 0 &&
                  player->position() == 1000 &&
                  !player->audioOutput()->isMuted(),
               "Inspector keypress leaked into transport or edits"))
    return false;
  type->setFocus();
  if (!require(QTest::qWaitFor([&] { return window.focusWidget() == type; }, 2000),
               "transition type did not take focus"))
    return false;
  QTest::keyClick(type, Qt::Key_Backspace);
  if (!require(player->duration() == 5400,
               "Combo keypress changed project duration"))
    return false;
  if (!require(timeline->selectedClip() == 1,
               "Combo keypress changed scene selection"))
    return false;
  auto *sceneIn = window.findChild<QSpinBox *>("sceneIn");
  if (!require(sceneIn, "Scene trim field missing"))
    return false;
  sceneIn->setFocus();
  if (!require(QTest::qWaitFor([&] { return window.focusWidget() == sceneIn; }, 2000),
               "scene trim field did not take focus"))
    return false;
  QTest::keyClick(sceneIn, Qt::Key_Backspace);
  if (!require(player->duration() == 5400 && sceneIn->value() == 0,
               "Scene trim keypress leaked into edits"))
    return false;
  // Typing a duration and pressing Enter applies it as one undo step.
  duration->setFocus();
  QTest::keyClick(duration, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClicks(duration, QStringLiteral("700"));
  QTest::keyClick(duration, Qt::Key_Enter);
  if (!require(duration->value() == 700 && player->duration() == 5300,
               "Typed transition duration did not apply"))
    return false;
  // Hotkeys need window focus: focus is still in the duration field.
  window.setFocus();
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  window.setFocus();
  QTest::keyClick(&window, Qt::Key_Space);
  player->pause();
  if (!require(player->duration() == 5400 && duration->value() == 600,
               "Typed duration was not one undo step"))
    return false;
  // A typed duration the export grid normalizes snaps back in the field,
  // even though the spinbox still has focus.
  duration->setFocus();
  if (!require(QTest::qWaitFor([&] { return window.focusWidget() == duration; }, 2000),
               "duration field did not take focus"))
    return false;
  QTest::keyClick(duration, Qt::Key_A, Qt::ControlModifier);
  QTest::keyClicks(duration, QStringLiteral("601"));
  QTest::keyClick(duration, Qt::Key_Enter);
  if (!require(duration->value() == 600 && player->duration() == 5400,
               "Normalized duration did not reconcile in the field"))
    return false;
  player->setPosition(2700);
  QTest::keyClick(&window, Qt::Key_S);
  if (!require(player->duration() == 5400 && timeline->selectedClip() == 1,
               "Split inside a blend unexpectedly changed the project"))
    return false;
  timeline->setRange(2600, 2800);
  QTest::keyClick(&window, Qt::Key_Delete);
  if (!require(player->duration() == 5400 && timeline->rangeIn() == 2600,
               "Cut through a blend silently removed the transition"))
    return false;
  QTest::keyClick(&window, Qt::Key_Escape);
  const auto point = [&](qint64 ms, int y) {
    return QPoint(64 + qRound((timeline->width() - 80) * ms / 5400.0), y);
  };
  QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, point(2700, 75));
  if (!require(timeline->selectedTransition() == 1 &&
                  timeline->selectedClip() == 0 &&
                  window.focusWidget() == type,
               "Transition badge did not select the boundary"))
    return false;
  // The 600 ms overlap consumes the kept tail/head: incoming scene 2
  // starts at 2400 of the 5400 ms composition.
  if (!require(QTest::qWaitFor([&] { return player->position() == 2400; }, 4000),
               "Boundary selection did not park at the overlap start"))
    return false;
  // Delete with a boundary selected removes the transition, not the scene,
  // and the boundary stays selected as a hard cut. Focus leaves the Type
  // field first: hotkeys stay suspended while typing.
  window.setFocus();
  QTest::keyClick(&window, Qt::Key_Delete);
  if (!require(player->duration() == 6000 &&
                  timeline->selectedTransition() == 1 &&
                  timeline->selectedClip() == 0,
               "Delete did not remove only the transition"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 5400 &&
                  timeline->selectedTransition() == 1 &&
                  timeline->selectedClip() == 0,
               "Undo did not restore the transition and its selection"))
    return false;
  // The timeline context menu offers the same removal.
  QTest::mouseClick(timeline, Qt::RightButton, Qt::NoModifier, point(1000, 75));
  auto *menu = timeline->findChild<QMenu *>();
  QAction *remove = nullptr;
  if (menu)
    for (auto *action : menu->actions())
      if (action->text().startsWith(QStringLiteral("Delete selection")))
        remove = action;
  if (!require(menu && menu->isVisible() && remove && remove->isEnabled(),
               "Context menu did not offer boundary deletion"))
    return false;
  remove->trigger();
  if (!require(player->duration() == 6000 && timeline->selectedTransition() == 1,
               "Menu deletion did not remove only the transition"))
    return false;
  menu->close();
  if (!require(QTest::qWaitFor(
                    [&] { return timeline->findChild<QMenu *>() == nullptr; },
                    2000),
               "Closed menu lingered"))
    return false;
  // A hard-cut boundary has nothing to delete: the menu offers nothing.
  QTest::mouseClick(timeline, Qt::RightButton, Qt::NoModifier, point(1000, 75));
  auto *plainMenu = timeline->findChild<QMenu *>();
  QAction *plainRemove = nullptr;
  if (plainMenu)
    for (auto *action : plainMenu->actions())
      if (action->text().startsWith(QStringLiteral("Delete selection")))
        plainRemove = action;
  if (!require(plainMenu && plainMenu->isVisible() && plainRemove &&
                  !plainRemove->isEnabled(),
               "Context menu offered deletion with no transition"))
    return false;
  plainMenu->close();
  window.setFocus();
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 5400 && timeline->selectedTransition() == 1,
               "Undo did not restore the menu-removed transition"))
    return false;
  // T on the last scene keeps its clip: there is no outgoing boundary.
  timeline->setSelectedClip(2);
  QTest::keyClick(&window, Qt::Key_T);
  if (!require(timeline->selectedClip() == 2 &&
                  timeline->selectedTransition() == 0,
               "T on the last scene abandoned its clip"))
    return false;
  timeline->setSelectedClip(1);
  QPushButton *later = nullptr;
  for (auto *button : window.findChildren<QPushButton *>())
    if (button->text() == QStringLiteral("Later"))
      later = button;
  if (!require(later && later->isEnabled(), "Reorder command unavailable"))
    return false;
  later->click();
  if (!require(player->duration() == 6000,
               "Reorder attached transition to the wrong pair"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 5400 && type->currentIndex() == 2,
               "Undo reorder did not restore transition pair"))
    return false;
  QTest::keyClick(&window, Qt::Key_Delete);
  if (!require(player->duration() == 3000,
               "Deleting a scene left its transition behind"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  if (!require(player->duration() == 5400 && type->currentIndex() == 2,
               "Undo scene deletion did not restore the blend"))
    return false;
  type->setCurrentIndex(0);
  if (!require(player->duration() == 6000,
               "Hard cut did not restore full scene duration"))
    return false;
  QTest::keyClick(&window, Qt::Key_Z, Qt::ControlModifier);
  window.close();
  if (!require(QTest::qWaitFor([&] { return !window.isVisible(); }, 7000),
               "Transition project did not save on close"))
    return false;
  const auto saved = loadStudioProject(path + ".omasnap.json");
  const auto *transition = studioTransition(saved.project, 1, 2);
  if (!require(saved.error.isEmpty() && transition &&
                   transition->kind == StudioTransitionKind::FadeBlack &&
                   transition->durationMs == 600 &&
                   studioDuration(saved.project) == 5400,
               "Transition pair, kind, or duration did not persist"))
    return false;
  return true;
}
