/** @fileoverview Declares the zoom/pan model: the one description of what
 *  the camera is doing at a given time.
 *
 *  Preview and export both read it. The preview draws `zoomSourceRect()` of
 *  the decoded frame; the export hands ffmpeg expressions generated from the
 *  same cues by `zoomPanExpressions()`. Nothing else is allowed to decide
 *  where the camera is, because two implementations of that would disagree
 *  and the export would surprise the person who set it up. */
#pragma once

#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

class QJsonObject;

/**
 * One zoom: ease in to `scale` centred on `target`, hold, ease back out.
 *
 * `target` is normalized to the source frame (0,0 top-left to 1,1
 * bottom-right) so a cue survives the recording being scaled, and so a cue
 * placed by clicking the preview means the same thing as one derived from a
 * pointer sidecar.
 */
struct ZoomCue {
  /// Identity that survives sorting and editing, so the UI can keep hold of
  /// the cue the user is dragging.
  quint64 id = 0;
  qint64 startMs = 0;
  qint64 endMs = 0;
  /// Ramp durations, clamped against the cue's own length at evaluation.
  qint64 easeInMs = 400;
  qint64 easeOutMs = 400;
  QPointF target{0.5, 0.5};
  qreal scale = 2.0;

  bool operator==(const ZoomCue &) const = default;
};

/** Every cue on one clip, in no particular order. */
struct ZoomTrack {
  static constexpr int kSchema = 1;
  QVector<ZoomCue> cues;

  bool operator==(const ZoomTrack &) const = default;
};

/** Where the camera is: how far in, and on what point. */
struct ZoomView {
  qreal scale = 1.0;
  QPointF centre{0.5, 0.5};
};

/// Smallest and largest zoom a cue may ask for. Below 1 would show outside
/// the frame; far above it there is nothing left to look at.
inline constexpr qreal kMinZoomScale = 1.0;
inline constexpr qreal kMaxZoomScale = 8.0;
/// Shortest cue worth having, and the shortest visible ramp.
inline constexpr qint64 kMinCueMs = 200;
inline constexpr qint64 kMinEaseMs = 60;
/// Most cues one clip may carry.
///
/// The binding limit is not the length of the argument but ffmpeg's own
/// expression complexity: measured against ffmpeg n9.0.1, this generator's
/// filter is accepted at 88 cues and rejected at 89 with "Failed to
/// configure output pad", long before the 131072-byte single-argument
/// ceiling. The cap is set well under the measured cliff rather than at it,
/// and the golden test proves the cap by handing a full track to ffmpeg
/// rather than by counting characters.
inline constexpr int kMaxZoomCues = 40;

/**
 * Puts a track into the form everything downstream agrees on: disjoint,
 * inside the clip, and within the cue cap.
 *
 * The renderers already read cues through sortedCues(), but the timeline and
 * the editing lookups read the stored list, so an overlap left the lane
 * drawing and editing cue tails that neither renderer would ever show.
 * Normalizing what is stored keeps one description of the track.
 */
void normalizeZoomTrack(ZoomTrack &track, qint64 durationMs);

/**
 * Cues sorted by start, degenerate ones dropped, and overlaps resolved by
 * trimming the earlier cue to where the next one begins.
 *
 * Both the preview and the export read this, never the raw list, which is
 * what makes them agree: with disjoint cues "the deepest cue" and "the cue
 * that is running" are the same cue, so there is no way for the two
 * descriptions to pick different centres.
 */
[[nodiscard]] QVector<ZoomCue> sortedCues(const ZoomTrack &track);

/** How long a cue's ramps actually last, after fitting them into its own
 *  length. Shared so the preview and the export split them identically. */
struct ZoomRamps {
  qreal easeInMs = 0;
  qreal easeOutMs = 0;
};
[[nodiscard]] ZoomRamps zoomRamps(const ZoomCue &cue);

/**
 * The camera at `timeMs`. Between cues this is 1x centred; inside one it
 * eases with a smoothstep, which starts and ends at zero velocity so a zoom
 * does not visibly jerk at either end.
 *
 * Overlapping cues resolve to whichever is furthest in at that moment,
 * rather than being rejected: the UI prevents overlap, and a project edited
 * by hand should still render something sensible.
 */
[[nodiscard]] ZoomView zoomViewAt(const ZoomTrack &track, qint64 timeMs);

/**
 * The part of the source frame that fills the output at `timeMs`, in
 * normalized coordinates. Always the full frame's aspect, and always inside
 * it: a cue centred near an edge pans as far as it can rather than sampling
 * past the border.
 */
[[nodiscard]] QRectF zoomSourceRect(const ZoomTrack &track, qint64 timeMs);

/** ffmpeg `zoompan` expressions, generated from the same cues. */
struct ZoomPanExpressions {
  QString z;
  QString x;
  QString y;
  /// True when the track does nothing, so the caller can skip the filter
  /// rather than pay for a no-op rescale of every frame.
  bool identity = true;
};

/**
 * Expressions for ffmpeg's `zoompan`, in terms of its output frame counter
 * `on` at `fpsNumerator/fpsDenominator`. zoompan crops `iw/z` by `ih/z` at
 * (`x`,`y`) and scales that to the output size, which is the same operation
 * `zoomSourceRect()` describes.
 *
 * The frame rate is a ratio, not a number, because rounding it is a drift:
 * 30000/1001 read as 30 puts the camera about 3.6 seconds out of step with
 * the picture over an hour, and shortens the video against its own audio.
 *
 * `startOffsetMs` is where the filter's first frame sits on the timeline.
 * zoompan counts frames from its own input, so a trimmed export starts `on`
 * at zero however far in the cut began; without the offset every cue would
 * slide by the length of the trim. A `setpts` in front cannot fix this --
 * zoompan reads the frame counter, not the timestamp.
 */
[[nodiscard]] ZoomPanExpressions zoomPanExpressions(const ZoomTrack &track,
                                                    int fpsNumerator,
                                                    int fpsDenominator = 1,
                                                    qint64 startOffsetMs = 0);

[[nodiscard]] QJsonObject writeZoomTrack(const ZoomTrack &track);
[[nodiscard]] bool readZoomTrack(const QJsonObject &object, ZoomTrack &track,
                                 QString &error);

/**
 * Places a cue centred on `target` at `atMs`, sized so it does not collide
 * with its neighbours and stays inside `limitMs`, and returns its id. Zero
 * when there is no room -- including at the very end of a clip, where a cue
 * would otherwise be created past the last frame and never render.
 */
quint64 addZoomCue(ZoomTrack &track, qint64 atMs, const QPointF &target,
                   qreal scale, qint64 durationMs, qint64 limitMs);
/** Removes the cue with `id`; false when there is no such cue. */
bool removeZoomCue(ZoomTrack &track, quint64 id);
/** The cue covering `timeMs`, or nullptr. */
[[nodiscard]] const ZoomCue *zoomCueAt(const ZoomTrack &track, qint64 timeMs);
