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

// One three-stop linear gradient. Stops are evenly spaced, exactly like
// Bettershot's CGGradient with nil locations. Endpoints live in unit space
// with a top-left origin: (0,0) is the top-left corner, (1,1) bottom-right.
struct StudioGradientStop {
  double red;
  double green;
  double blue;
};

struct StudioGradient {
  const char *name;
  StudioGradientStop stops[3];
  double startX;
  double startY;
  double endX;
  double endY;
};

namespace {
inline QColor studioStopColor(const StudioGradientStop &stop) {
  return QColor::fromRgbF(static_cast<float>(stop.red),
                          static_cast<float>(stop.green),
                          static_cast<float>(stop.blue));
}
} // namespace

struct StudioStyle {
  int background = 0;
  int padding = 0; // Percent of each canvas dimension, on each side.
  int radius = 0;  // Pixels at a 1080-pixel canvas height.
  // Local wallpaper image winning over `background` while set. Stored as an
  // absolute path; a missing file surfaces as an export error, like a
  // missing scene source.
  QString wallpaperPath;
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
  // Gradient presets matching Bettershot's sixteen three-stop linear
  // gradients. Stop values are Bettershot's 0..1 components verbatim;
  // conversion to 8-bit happens at the single QColor boundary above.
  static constexpr std::array<StudioGradient, 16> gradients{{
      {"Dawn Fire",
       {{0.98, 0.31, 0.58}, {0.40, 0.32, 0.95}, {0.29, 0.84, 0.80}},
       0, 0, 1, 1},
      {"Deep Ocean",
       {{0.04, 0.05, 0.50}, {0.26, 0.19, 0.93}, {0.42, 0.67, 0.98}},
       0.5, 0, 1, 1},
      {"Coral Bloom",
       {{0.98, 0.38, 0.36}, {0.99, 0.71, 0.36}, {0.90, 0.33, 0.65}},
       0, 0, 1, 1},
      {"Arctic Lens",
       {{0.87, 0.95, 0.94}, {0.46, 0.77, 0.86}, {0.25, 0.53, 0.93}},
       0, 0, 1, 1},
      {"Neon Pulse",
       {{0.08, 0.02, 0.22}, {0.35, 0.12, 0.84}, {0.95, 0.26, 0.42}},
       1, 0, 0, 1},
      {"Ripe Mango",
       {{0.99, 0.75, 0.20}, {0.96, 0.33, 0.21}, {0.67, 0.19, 0.89}},
       0, 0, 1, 1},
      {"Soft Linen",
       {{0.94, 0.94, 0.92}, {0.80, 0.88, 0.94}, {0.95, 0.76, 0.70}},
       0, 0, 1, 1},
      {"Tidal Pool",
       {{0.08, 0.30, 0.54}, {0.25, 0.64, 0.72}, {0.70, 0.92, 0.78}},
       0, 1, 1, 0},
      {"Forge",
       {{0.18, 0.03, 0.08}, {0.86, 0.17, 0.18}, {1.00, 0.67, 0.25}},
       0, 0, 1, 1},
      {"Twilight",
       {{0.24, 0.08, 0.51}, {0.59, 0.22, 0.94}, {0.96, 0.42, 0.74}},
       0.5, 0, 1, 1},
      {"Lagoon",
       {{0.43, 0.86, 0.75}, {0.25, 0.62, 0.80}, {0.22, 0.35, 0.75}},
       0, 0, 1, 1},
      {"Orchard",
       {{0.99, 0.91, 0.30}, {0.44, 0.78, 0.29}, {0.12, 0.58, 0.42}},
       1, 0, 0, 1},
      {"Gemstone",
       {{0.10, 0.08, 0.28}, {0.35, 0.15, 0.65}, {0.76, 0.39, 0.95}},
       0, 1, 1, 0},
      {"Sherbet",
       {{1.00, 0.49, 0.51}, {1.00, 0.74, 0.48}, {0.56, 0.78, 0.98}},
       0, 0, 1, 1},
      {"Granite",
       {{0.93, 0.96, 0.95}, {0.64, 0.72, 0.82}, {0.33, 0.42, 0.55}},
       0, 0, 1, 1},
      {"Sunrise",
       {{0.98, 0.62, 0.77}, {0.98, 0.82, 0.47}, {0.42, 0.71, 0.96}},
       0, 1, 1, 0},
  }};
  static constexpr int backgroundCount =
      static_cast<int>(backgrounds.size() + gradients.size());
  static constexpr int solidCount =
      static_cast<int>(backgrounds.size());
  static constexpr int gradientCount =
      static_cast<int>(gradients.size());

  [[nodiscard]] static bool isGradient(int index) {
    return index >= solidCount && index < backgroundCount;
  }

  [[nodiscard]] static QString backgroundName(int index) {
    if (index < 0 || index >= backgroundCount)
      return {};
    if (index < solidCount)
      return QString::fromUtf8(backgrounds[static_cast<size_t>(index)].name);
    return QString::fromUtf8(
        gradients[static_cast<size_t>(index - solidCount)].name);
  }

  [[nodiscard]] static StudioGradient gradient(int index) {
    const int clamped =
        (index >= solidCount && index < backgroundCount) ? index - solidCount
                                                         : 0;
    return gradients[static_cast<size_t>(clamped)];
  }

  [[nodiscard]] QColor color() const {
    if (isGradient(background))
      return studioStopColor(gradient(background).stops[1]);
    const int index =
        (background >= 0 && background < solidCount) ? background : 0;
    const auto preset = backgrounds[static_cast<size_t>(index)];
    return QColor(preset.red, preset.green, preset.blue);
  }
};
