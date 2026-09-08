/** @fileoverview Inspector background section: tabs, presets, grid. */
#include "studio-background-ui-smoke.hpp"
#include "studio-playback.hpp"
#include "studio.hpp"
#include <QComboBox>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>

#include <algorithm>

bool runStudioBackgroundUiChecks(const QString &source, QString &error) {
  // Discovery is environment-dependent: assert shape, tolerate absence.
  const QStringList discovered = studioQuattroWallpaperPaths();
  const auto sorted = discovered;
  if (![&] {
        if (!std::is_sorted(sorted.begin(), sorted.end()))
          return false;
        for (const auto &path : discovered) {
          if (!QFile::exists(path))
            return false;
          const QString lower = path.toLower();
          if (!lower.endsWith(QStringLiteral(".png")) &&
              !lower.endsWith(QStringLiteral(".jpg")) &&
              !lower.endsWith(QStringLiteral(".jpeg")) &&
              !lower.endsWith(QStringLiteral(".webp")) &&
              !lower.endsWith(QStringLiteral(".bmp")))
            return false;
        }
        return true;
      }()) {
    error = QStringLiteral("wallpaper discovery returned unsorted, missing, "
                           "or non-image paths");
    return false;
  }
  QTemporaryDir scratch;
  const auto path = scratch.filePath("background.mp4");
  if (!QFile::copy(source, path))
    return false;
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
          "background UI source did not load"))
    return false;
  auto *background =
      window.findChild<QComboBox *>(QStringLiteral("canvasBackground"));
  auto *colorTab =
      window.findChild<QPushButton *>(QStringLiteral("backgroundColorTab"));
  auto *gradientTab = window.findChild<QPushButton *>(
      QStringLiteral("backgroundGradientTab"));
  auto *wallpaperTab = window.findChild<QPushButton *>(
      QStringLiteral("backgroundWallpaperTab"));
  auto *presets =
      window.findChild<QComboBox *>(QStringLiteral("canvasPresets"));
  auto *padding = window.findChild<QSlider *>(QStringLiteral("canvasPadding"));
  auto *shadow = window.findChild<QSlider *>(QStringLiteral("canvasShadow"));
  auto *grid = window.findChild<QWidget *>(QStringLiteral("wallpaperGrid"));
  if (!require(background && colorTab && gradientTab && wallpaperTab &&
                   presets && padding && shadow && grid,
               "background inspector controls are missing"))
    return false;
  if (!require(background->count() == StudioStyle::solidCount &&
                   background->isVisible() && colorTab->isChecked(),
               "background section did not start on solids"))
    return false;
  gradientTab->click();
  if (!require(background->count() == StudioStyle::gradientCount &&
                   gradientTab->isChecked(),
               "gradient tab did not filter the combo"))
    return false;
  colorTab->click();
  if (!require(background->count() == StudioStyle::solidCount &&
                   background->currentText() == QStringLiteral("Obsidian"),
               "color tab did not restore solids"))
    return false;
  // Floating card: Dawn Fire, padding 10, corners 24, shadow 30.
  presets->activated(2);
  if (!require(padding->value() == 10 && shadow->value() == 30 &&
                   gradientTab->isChecked() &&
                   background->currentText() == QStringLiteral("Dawn Fire"),
               "built-in preset did not apply"))
    return false;
  wallpaperTab->click();
  if (!require(!background->isVisible() && grid->isVisible(),
               "wallpaper tab did not swap the combo for the grid"))
    return false;
  // Browsing wallpapers survives unrelated edits with a preset model.
  padding->setValue(padding->value() + 1);
  if (!require(wallpaperTab->isChecked() && grid->isVisible(),
               "a slider move kicked out wallpaper browsing"))
    return false;
  // The hidden combo must not corrupt the stored preset.
  gradientTab->click();
  if (!require(background->currentText() == QStringLiteral("Dawn Fire"),
               "browsing wallpapers corrupted the stored preset"))
    return false;
  wallpaperTab->click();
  if (discovered.isEmpty()) {
    // No themes here (CI): await the async load, then the custom tile and
    // final status must exist instead of a stuck "Loading…".
    auto *status =
        window.findChild<QLabel *>(QStringLiteral("wallpaperStatus"));
    if (!require(status, "wallpaper status label is missing"))
      return false;
    if (!require(QTest::qWaitFor(
                     [&] {
                       const auto tiles =
                           grid->findChildren<QPushButton *>();
                       return tiles.size() == 1 &&
                              tiles.first()->objectName() ==
                                  QStringLiteral("canvasWallpaperPlus") &&
                              status->text() ==
                                  QStringLiteral("No theme wallpapers found.");
                     },
                     10000),
                 "empty discovery left loading status or hid the custom tile"))
      return false;
    return true;
  }
  if (!require(QTest::qWaitFor(
                   [&] {
                     // The custom tile exists immediately; thumbs arrive
                     // asynchronously from the worker.
                     return grid->findChildren<QPushButton *>().size() >= 2;
                   },
                   10000),
               "wallpaper grid never populated"))
    return false;
  const auto tiles = grid->findChildren<QPushButton *>();
  // First tile carries a discovered thumbnail; the last opens the picker.
  if (!require(tiles.size() >= 2 && !tiles.first()->icon().isNull() &&
                   tiles.last()->text() == QStringLiteral("+"),
               "wallpaper grid tiles are wrong"))
    return false;
  tiles.first()->click();
  const auto reselected = grid->findChildren<QPushButton *>();
  int checked = 0;
  for (const auto *tile : reselected) {
    if (tile->text() == QStringLiteral("+"))
      continue;
    if (tile->isChecked())
      ++checked;
  }
  if (!require(checked == 1, "picking a wallpaper tile did not select it"))
    return false;
  return true;
}
