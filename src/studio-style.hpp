/** @fileoverview Exported canvas styling shared by preview and export. */
#pragma once

#include <QColor>
#include <QString>

#include <array>

struct StudioBackground {
  const char *name;
  int red;
  int green;
  int blue;
};

struct StudioStyle {
  int background = 0;
  int padding = 0; // Percent of each canvas dimension, on each side.
  int radius = 0;  // Pixels at a 1080-pixel canvas height.
  bool operator==(const StudioStyle &) const = default;

  // Solid background presets matching Bettershot's twelve-color palette.
  // Index order is the inspector order. There is no mapping from the old
  // four-color indices: old projects keep a valid index with a new color.
  static constexpr std::array<StudioBackground, 12> backgrounds{{
      {"Obsidian", 5, 5, 8},
      {"Chalk", 245, 245, 240},
      {"Slate", 43, 46, 56},
      {"Ember", 240, 61, 71},
      {"Tangerine", 247, 135, 41},
      {"Saffron", 245, 189, 59},
      {"Fern", 59, 158, 92},
      {"Cobalt", 41, 128, 224},
      {"Iris", 122, 69, 232},
      {"Rose", 237, 171, 161},
      {"Seafoam", 168, 230, 189},
      {"Cloud", 161, 204, 240},
  }};
  static constexpr int backgroundCount =
      static_cast<int>(backgrounds.size());

  [[nodiscard]] static QString backgroundName(int index) {
    if (index < 0 || index >= backgroundCount)
      return {};
    return QString::fromUtf8(backgrounds[static_cast<size_t>(index)].name);
  }

  [[nodiscard]] QColor color() const {
    const int index =
        (background >= 0 && background < backgroundCount) ? background : 0;
    const auto preset = backgrounds[static_cast<size_t>(index)];
    return QColor(preset.red, preset.green, preset.blue);
  }
};
