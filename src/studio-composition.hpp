/** @fileoverview Pure FFmpeg composition planning for Studio projects. */
#pragma once

#include "studio-project.hpp"

/** Build one export from an immutable project snapshot. No disk or process
 * work happens here. Source timestamps are retimed before the final project
 * frame-rate conversion (including VFR inputs). Every source is fit on a
 * black canonical canvas before global camera and canvas styling, exactly
 * like the GPU preview. The primary audio stream becomes stereo, matching
 * playback; missing audio is silence. Returns no arguments and an actionable
 * error for invalid input. */
// Transitions overlap existing trimmed source ranges; RGB picture weights
// and linear audio gains match studioBlendAt. Global zoom/style apply
// afterward.
[[nodiscard]] QStringList
studioCompositionArguments(const StudioProject &project,
                           const QString &destination, QString &error);
