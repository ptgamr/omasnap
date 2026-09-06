#include "studio-transitions-ui-smoke.hpp"
#include "studio-playback.hpp"
#include "studio.hpp"
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
          type && duration &&
              QTest::qWaitFor([&] { return player->duration() == 6000; }, 6000),
          "Transition controls or media unavailable"))
    return false;
  player->setPosition(3000);
  QTest::keyClick(&window, Qt::Key_S);
  timeline->setSelectedClip(1);
  QTest::keyClick(&window, Qt::Key_T);
  if (!require(type->isEnabled() && type->hasFocus(),
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
  QTest::keyClick(duration, Qt::Key_Space);
  if (!require(player->playbackState() == QMediaPlayer::PlayingState,
               "Space from transition duration did not transport"))
    return false;
  player->pause();
  player->setPosition(2700);
  QTest::keyClick(&window, Qt::Key_S);
  if (!require(player->duration() == 5400 && timeline->selectedClip() == 1,
               "Split inside a blend unexpectedly changed the project"))
    return false;
  QTest::keyClick(&window, Qt::Key_B);
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
  if (!require(timeline->selectedClip() == 1 && type->hasFocus(),
               "Transition badge did not select its exact outgoing scene"))
    return false;
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
