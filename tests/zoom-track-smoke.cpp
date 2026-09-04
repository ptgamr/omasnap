/** @fileoverview Tests the zoom/pan model: the easing shape, panning that
 *  stops at the frame edge, cue placement that cannot overlap, JSON round
 *  trips, and the ffmpeg expressions the export is built from. */
#include "zoom-track-smoke.hpp"

#include "zoom-track.hpp"

#include <QJsonObject>

#include <cmath>

namespace {

bool check(bool condition, QString &error, const QString &message) {
  if (condition)
    return true;
  error = message;
  return false;
}

ZoomTrack oneCue() {
  ZoomCue cue;
  cue.id = 1;
  cue.startMs = 1000;
  cue.endMs = 3000;
  cue.easeInMs = 400;
  cue.easeOutMs = 400;
  cue.target = {0.25, 0.75};
  cue.scale = 2.0;
  return ZoomTrack{{cue}};
}

} // namespace

bool runZoomTrackSmoke(QString &error) {
  const ZoomTrack track = oneCue();

  // Outside a cue the camera is out, and exactly out: an export that scaled
  // by 1.0001 everywhere would soften every untouched frame.
  if (!check(zoomViewAt(track, 0).scale == 1.0 &&
                 zoomViewAt(track, 999).scale == 1.0 &&
                 zoomViewAt(track, 3000).scale == 1.0 &&
                 zoomViewAt(track, 9000).scale == 1.0,
             error, QStringLiteral("the camera is not resting outside cues")))
    return false;
  if (!check(zoomSourceRect(track, 0) == QRectF(0, 0, 1, 1), error,
             QStringLiteral("a resting camera does not show the whole "
                            "frame")))
    return false;

  // Inside, it holds at the cue's scale.
  if (!check(qFuzzyCompare(zoomViewAt(track, 2000).scale, 2.0), error,
             QStringLiteral("the camera does not reach the cue's scale")))
    return false;

  // The ramps are monotonic and start/end at rest, which is what stops a
  // zoom from visibly jerking.
  qreal previous = 0.0;
  for (qint64 t = 1000; t <= 1400; t += 25) {
    const qreal scale = zoomViewAt(track, t).scale;
    if (scale < previous - 1e-9) {
      error = QStringLiteral("the ease-in is not monotonic at %1 ms").arg(t);
      return false;
    }
    previous = scale;
  }
  if (!check(zoomViewAt(track, 1010).scale < 1.05 &&
                 zoomViewAt(track, 2990).scale < 1.05,
             error,
             QStringLiteral("the ramps do not start and end at rest")))
    return false;
  // Halfway through the ease-in, smoothstep is exactly halfway.
  if (!check(std::abs(zoomViewAt(track, 1200).scale - 1.5) < 0.01, error,
             QStringLiteral("the ease is not a smoothstep")))
    return false;

  // The window is the frame's shape, sized 1/scale, and inside the frame.
  const QRectF window = zoomSourceRect(track, 2000);
  if (!check(std::abs(window.width() - 0.5) < 1e-9 &&
                 std::abs(window.height() - 0.5) < 1e-9,
             error, QStringLiteral("the zoom window is the wrong size")))
    return false;
  if (!check(window.left() >= -1e-9 && window.top() >= -1e-9 &&
                 window.right() <= 1.0 + 1e-9 &&
                 window.bottom() <= 1.0 + 1e-9,
             error, QStringLiteral("the zoom window leaves the frame")))
    return false;

  // A cue aimed into the corner pans as far as it can and then stops, rather
  // than sampling past the border.
  ZoomTrack corner = oneCue();
  corner.cues[0].target = {0.0, 0.0};
  const QRectF cornerWindow = zoomSourceRect(corner, 2000);
  if (!check(std::abs(cornerWindow.left()) < 1e-9 &&
                 std::abs(cornerWindow.top()) < 1e-9,
             error, QStringLiteral("a corner cue did not pan to the edge")))
    return false;

  // A cue shorter than its own ramps still reaches somewhere and still
  // starts and ends at rest.
  ZoomTrack tight = oneCue();
  tight.cues[0].startMs = 0;
  tight.cues[0].endMs = 300;
  if (!check(zoomViewAt(tight, 150).scale > 1.2 &&
                 zoomViewAt(tight, 0).scale == 1.0 &&
                 zoomViewAt(tight, 300).scale == 1.0,
             error, QStringLiteral("a cue shorter than its ramps misbehaves")))
    return false;

  // Degenerate cues are ignored rather than rendered.
  ZoomTrack junk;
  junk.cues = {ZoomCue{9, 100, 120, 40, 40, {0.5, 0.5}, 2.0},
               ZoomCue{10, 500, 2000, 400, 400, {0.5, 0.5}, 1.0}};
  if (!check(sortedCues(junk).isEmpty(), error,
             QStringLiteral("a too-short or 1x cue was kept")))
    return false;

  // Placing cues: one lands, a second inside it is refused, and one placed
  // just before an existing cue is trimmed to meet it rather than overlap.
  ZoomTrack built;
  const quint64 first = addZoomCue(built, 5000, {0.5, 0.5}, 2.0, 2000, 20000);
  if (!check(first != 0 && built.cues.size() == 1, error,
             QStringLiteral("the first cue was not placed")))
    return false;
  if (!check(addZoomCue(built, 5500, {0.5, 0.5}, 2.0, 2000, 20000) == 0 &&
                 built.cues.size() == 1,
             error, QStringLiteral("a cue was placed inside another")))
    return false;
  const quint64 before = addZoomCue(built, 4000, {0.5, 0.5}, 2.0, 3000, 20000);
  if (!check(before != 0 && built.cues.size() == 2, error,
             QStringLiteral("a cue before an existing one was refused")))
    return false;
  for (const ZoomCue &cue : built.cues) {
    if (cue.id == before && cue.endMs != 5000) {
      error = QStringLiteral("a cue was not trimmed to meet its neighbour");
      return false;
    }
  }
  if (!check(zoomCueAt(built, 4500) != nullptr &&
                 zoomCueAt(built, 4500)->id == before &&
                 zoomCueAt(built, 100) == nullptr,
             error, QStringLiteral("cue lookup by time is wrong")))
    return false;
  if (!check(removeZoomCue(built, before) && built.cues.size() == 1 &&
                 !removeZoomCue(built, 999),
             error, QStringLiteral("removing a cue is wrong")))
    return false;

  // A cue is never created past the end of the clip, where it would render
  // nothing and sit as an unreachable sliver at the edge of the lane.
  ZoomTrack shortClip;
  if (!check(addZoomCue(shortClip, 1900, {0.5, 0.5}, 2.0, 2500, 2000) == 0,
             error, QStringLiteral("a cue was created past the end")))
    return false;
  const quint64 trimmed =
      addZoomCue(shortClip, 500, {0.5, 0.5}, 2.0, 2500, 2000);
  if (!check(trimmed != 0 && shortClip.cues.first().endMs == 2000, error,
             QStringLiteral("a cue was not trimmed to the clip length")))
    return false;

  // The cue count is capped, because every cue lengthens the single ffmpeg
  // argument the export has to exec with.
  ZoomTrack many;
  for (int index = 0; index < kMaxZoomCues + 10; ++index)
    static_cast<void>(addZoomCue(many, index * 1000, {0.5, 0.5}, 2.0, 500,
                                 1000000));
  if (!check(many.cues.size() == kMaxZoomCues, error,
             QStringLiteral("the cue count is not capped at %1")
                 .arg(kMaxZoomCues)))
    return false;
  {
    // Even at the cap the filter stays well inside the exec argument limit.
    const ZoomPanExpressions full = zoomPanExpressions(many, 30000, 1001);
    const qsizetype bytes = full.z.size() + full.x.size() + full.y.size();
    if (!check(bytes < 120000, error,
               QStringLiteral("a full track generates a %1-byte filter, which "
                              "is near the exec argument limit")
                   .arg(bytes)))
      return false;
  }

  // Overlapping cues are resolved before anything reads them, so the preview
  // and the export cannot pick different centres for the same instant.
  ZoomTrack overlapping;
  overlapping.cues = {ZoomCue{1, 500, 4000, 400, 400, {0.25, 0.25}, 3.0},
                      ZoomCue{2, 2000, 5000, 400, 400, {0.75, 0.75}, 2.0}};
  const QVector<ZoomCue> resolved = sortedCues(overlapping);
  if (!check(resolved.size() == 2 && resolved.at(0).endMs == 2000 &&
                 resolved.at(1).startMs == 2000,
             error, QStringLiteral("overlapping cues were not made disjoint")))
    return false;

  // The track crosses a process boundary as part of the project.
  ZoomTrack parsed;
  QString parseError;
  if (!check(readZoomTrack(writeZoomTrack(track), parsed, parseError) &&
                 parsed == track,
             error,
             QStringLiteral("the track did not survive a JSON round trip: %1")
                 .arg(parseError)))
    return false;
  QJsonObject future = writeZoomTrack(track);
  future.insert(QStringLiteral("schema"), ZoomTrack::kSchema + 1);
  ZoomTrack ignored;
  QString schemaError;
  if (!check(!readZoomTrack(future, ignored, schemaError), error,
             QStringLiteral("a future zoom schema was accepted")))
    return false;

  // An empty track produces a filter that does nothing, and says so, so the
  // export can leave every frame untouched instead of rescaling it.
  const ZoomPanExpressions none = zoomPanExpressions({}, 60);
  if (!check(none.identity && none.z == QStringLiteral("1"), error,
             QStringLiteral("an empty track did not produce an identity "
                            "filter")))
    return false;

  // A real track produces expressions in zoompan's own variables, and no
  // shell metacharacters: these are passed in an argument vector.
  const ZoomPanExpressions expressions = zoomPanExpressions(track, 60);
  if (!check(!expressions.identity && expressions.z.contains("on*1/60") &&
                 expressions.x.contains(QStringLiteral("iw")) &&
                 expressions.y.contains(QStringLiteral("ih")),
             error, QStringLiteral("the zoompan expressions look wrong")))
    return false;
  for (const QString &expression :
       {expressions.z, expressions.x, expressions.y}) {
    if (expression.contains(QLatin1Char(';')) ||
        expression.contains(QLatin1Char('`')) ||
        expression.contains(QLatin1Char('$'))) {
      error = QStringLiteral("a zoompan expression looks like a shell "
                             "fragment: %1")
                  .arg(expression);
      return false;
    }
  }
  return true;
}
