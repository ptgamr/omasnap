/** @fileoverview Exported canvas styling shared by preview and export. */
#pragma once

#include <QColor>

struct StudioStyle {
  int background = 0;
  int padding = 0; // Percent of each canvas dimension, on each side.
  int radius = 0;  // Pixels at a 1080-pixel canvas height.
  bool operator==(const StudioStyle &) const = default;

  [[nodiscard]] QColor color() const {
    switch (background) {
    case 1:
      return QColor(61, 49, 92);
    case 2:
      return QColor(220, 210, 190);
    case 3:
      return QColor(234, 234, 239);
    default:
      return QColor(22, 24, 35);
    }
  }
};
