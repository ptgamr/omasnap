/** @fileoverview The zoom/pan model (see zoom-track.hpp). */
#include "zoom-track.hpp"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace {

/// Starts and ends at zero velocity, so a zoom does not jerk at either end.
qreal smoothstep(qreal t) {
  const qreal x = qBound<qreal>(0.0, t, 1.0);
  return x * x * (3.0 - 2.0 * x);
}

/// How far in a cue is at `timeMs`, 0 outside it and 1 while it holds.
qreal cueWeight(const ZoomCue &cue, qint64 timeMs) {
  if (timeMs <= cue.startMs || timeMs >= cue.endMs)
    return 0.0;
  const qint64 length = cue.endMs - cue.startMs;
  // Ramps never overlap, however they were written down: a cue shorter than
  // its own ramps splits its length between them.
  qint64 easeIn = qMax<qint64>(kMinEaseMs, cue.easeInMs);
  qint64 easeOut = qMax<qint64>(kMinEaseMs, cue.easeOutMs);
  if (easeIn + easeOut > length) {
    const qreal share = static_cast<qreal>(length) / (easeIn + easeOut);
    easeIn = static_cast<qint64>(easeIn * share);
    easeOut = length - easeIn;
  }
  const qint64 into = timeMs - cue.startMs;
  const qint64 left = cue.endMs - timeMs;
  if (easeIn > 0 && into < easeIn)
    return smoothstep(static_cast<qreal>(into) / static_cast<qreal>(easeIn));
  if (easeOut > 0 && left < easeOut)
    return smoothstep(static_cast<qreal>(left) / static_cast<qreal>(easeOut));
  return 1.0;
}

/// The centre a view of `scale` may actually use: panning stops at the frame
/// edge rather than sampling past it.
QPointF clampCentre(const QPointF &target, qreal scale) {
  if (scale <= 1.0)
    return {0.5, 0.5};
  const qreal half = 0.5 / scale;
  return {qBound(half, target.x(), 1.0 - half),
          qBound(half, target.y(), 1.0 - half)};
}

QString number(qreal value) {
  return QString::number(value, 'f', 6);
}

} // namespace

QVector<ZoomCue> sortedCues(const ZoomTrack &track) {
  QVector<ZoomCue> cues;
  cues.reserve(track.cues.size());
  for (const ZoomCue &cue : track.cues) {
    if (cue.endMs - cue.startMs < kMinCueMs || cue.scale <= kMinZoomScale)
      continue;
    ZoomCue usable = cue;
    usable.scale = qBound(kMinZoomScale, usable.scale, kMaxZoomScale);
    cues.push_back(usable);
  }
  std::sort(cues.begin(), cues.end(),
            [](const ZoomCue &a, const ZoomCue &b) {
              return a.startMs < b.startMs;
            });
  return cues;
}

ZoomView zoomViewAt(const ZoomTrack &track, qint64 timeMs) {
  ZoomView view;
  qreal best = 0.0;
  for (const ZoomCue &cue : sortedCues(track)) {
    const qreal weight = cueWeight(cue, timeMs);
    if (weight <= 0.0)
      continue;
    // Interpolating between the resting 1x and the cue is what makes the
    // ramp: at weight 0 the camera is out, at 1 it is fully in.
    const qreal scale = 1.0 + (cue.scale - 1.0) * weight;
    if (scale <= best)
      continue;
    best = scale;
    view.scale = scale;
    view.centre = clampCentre(cue.target, scale);
  }
  return view;
}

QRectF zoomSourceRect(const ZoomTrack &track, qint64 timeMs) {
  const ZoomView view = zoomViewAt(track, timeMs);
  const qreal size = 1.0 / qMax<qreal>(1.0, view.scale);
  return {view.centre.x() - size / 2.0, view.centre.y() - size / 2.0, size,
          size};
}

ZoomPanExpressions zoomPanExpressions(const ZoomTrack &track, int fps,
                                      qint64 startOffsetMs) {
  ZoomPanExpressions expressions;
  expressions.z = QStringLiteral("1");
  const QVector<ZoomCue> cues = sortedCues(track);
  if (cues.isEmpty() || fps <= 0) {
    // iw/2 and ih/2 keep the untouched frame centred, which is what a zoom
    // of 1 means.
    expressions.x = QStringLiteral("iw/2-(iw/zoom/2)");
    expressions.y = QStringLiteral("ih/2-(ih/zoom/2)");
    return expressions;
  }
  expressions.identity = false;

  // zoompan counts output frames in `on`; time is that over the frame rate,
  // plus wherever this filter's first frame sits on the timeline, which is
  // the same clock zoomViewAt() is evaluated on.
  const QString time =
      startOffsetMs == 0
          ? QStringLiteral("(on/%1)").arg(fps)
          : QStringLiteral("((on/%1)+%2)")
                .arg(QString::number(fps), number(startOffsetMs / 1000.0));
  QString scale = QStringLiteral("1");
  QString centreX = QStringLiteral("0.5");
  QString centreY = QStringLiteral("0.5");

  for (const ZoomCue &cue : cues) {
    const qreal start = cue.startMs / 1000.0;
    const qreal end = cue.endMs / 1000.0;
    const qreal length = end - start;
    qreal easeIn = qMax<qint64>(kMinEaseMs, cue.easeInMs) / 1000.0;
    qreal easeOut = qMax<qint64>(kMinEaseMs, cue.easeOutMs) / 1000.0;
    if (easeIn + easeOut > length) {
      const qreal share = length / (easeIn + easeOut);
      easeIn *= share;
      easeOut = length - easeIn;
    }
    // The same smoothstep the preview uses, written out: ramp up, hold at 1,
    // ramp down. `clip` keeps each ramp's input inside 0..1 so the holding
    // section is exactly 1 rather than an extrapolation.
    const QString into = QStringLiteral("clip((%1-%2)/%3,0,1)")
                             .arg(time, number(start), number(easeIn));
    const QString left = QStringLiteral("clip((%1-%2)/%3,0,1)")
                             .arg(number(end), time, number(easeOut));
    const QString ramp =
        QStringLiteral("(min(%1*%1*(3-2*%1),%2*%2*(3-2*%2)))").arg(into, left);
    const QString weight = QStringLiteral("if(between(%1,%2,%3),%4,0)")
                               .arg(time, number(start), number(end), ramp);
    const QString cueScale =
        QStringLiteral("(1+%1*%2)").arg(number(cue.scale - 1.0), weight);
    const QPointF centre = clampCentre(cue.target, cue.scale);
    // Later cues win where they are further in, matching zoomViewAt()'s
    // choice of the deepest cue at that instant.
    scale = QStringLiteral("max(%1,%2)").arg(scale, cueScale);
    centreX = QStringLiteral("if(gt(%1,1.0001),%2,%3)")
                  .arg(cueScale, number(centre.x()), centreX);
    centreY = QStringLiteral("if(gt(%1,1.0001),%2,%3)")
                  .arg(cueScale, number(centre.y()), centreY);
  }

  expressions.z = scale;
  // zoompan's x and y are the crop window's top-left in input pixels, and
  // `zoom` there is the value z evaluated to for this frame.
  expressions.x = QStringLiteral("(%1)*iw-(iw/zoom/2)").arg(centreX);
  expressions.y = QStringLiteral("(%1)*ih-(ih/zoom/2)").arg(centreY);
  return expressions;
}

QJsonObject writeZoomTrack(const ZoomTrack &track) {
  QJsonArray cues;
  for (const ZoomCue &cue : track.cues) {
    cues.append(QJsonObject{
        {QStringLiteral("id"), static_cast<qint64>(cue.id)},
        {QStringLiteral("startMs"), cue.startMs},
        {QStringLiteral("endMs"), cue.endMs},
        {QStringLiteral("easeInMs"), cue.easeInMs},
        {QStringLiteral("easeOutMs"), cue.easeOutMs},
        {QStringLiteral("targetX"), cue.target.x()},
        {QStringLiteral("targetY"), cue.target.y()},
        {QStringLiteral("scale"), cue.scale}});
  }
  return {{QStringLiteral("schema"), ZoomTrack::kSchema},
          {QStringLiteral("cues"), cues}};
}

bool readZoomTrack(const QJsonObject &object, ZoomTrack &track,
                   QString &error) {
  const int schema = object.value(QStringLiteral("schema")).toInt();
  if (schema != ZoomTrack::kSchema) {
    error = QStringLiteral("Unsupported zoom track schema %1").arg(schema);
    return false;
  }
  ZoomTrack parsed;
  for (const QJsonValue value :
       object.value(QStringLiteral("cues")).toArray()) {
    const QJsonObject entry = value.toObject();
    ZoomCue cue;
    cue.id = static_cast<quint64>(
        entry.value(QStringLiteral("id")).toInteger());
    cue.startMs = entry.value(QStringLiteral("startMs")).toInteger();
    cue.endMs = entry.value(QStringLiteral("endMs")).toInteger();
    cue.easeInMs = entry.value(QStringLiteral("easeInMs")).toInteger(400);
    cue.easeOutMs = entry.value(QStringLiteral("easeOutMs")).toInteger(400);
    cue.target = {entry.value(QStringLiteral("targetX")).toDouble(0.5),
                  entry.value(QStringLiteral("targetY")).toDouble(0.5)};
    cue.scale = entry.value(QStringLiteral("scale")).toDouble(2.0);
    parsed.cues.push_back(cue);
  }
  track = parsed;
  return true;
}

quint64 addZoomCue(ZoomTrack &track, qint64 atMs, const QPointF &target,
                   qreal scale, qint64 durationMs) {
  const QVector<ZoomCue> existing = sortedCues(track);
  // A new cue starts at the playhead and runs for as long as it can without
  // touching the next one, so dropping cues in quick succession never
  // silently produces an overlap the model would have to resolve.
  qint64 start = qMax<qint64>(0, atMs);
  qint64 end = start + qMax<qint64>(kMinCueMs, durationMs);
  for (const ZoomCue &cue : existing) {
    if (cue.endMs <= start)
      continue;
    if (cue.startMs >= end)
      break;
    if (cue.startMs <= start)
      return 0; // The playhead is inside a cue already.
    end = cue.startMs;
    break;
  }
  if (end - start < kMinCueMs)
    return 0;

  quint64 nextId = 1;
  for (const ZoomCue &cue : track.cues)
    nextId = qMax(nextId, cue.id + 1);
  ZoomCue cue;
  cue.id = nextId;
  cue.startMs = start;
  cue.endMs = end;
  cue.target = {qBound(0.0, target.x(), 1.0), qBound(0.0, target.y(), 1.0)};
  cue.scale = qBound(kMinZoomScale, scale, kMaxZoomScale);
  track.cues.push_back(cue);
  return cue.id;
}

bool removeZoomCue(ZoomTrack &track, quint64 id) {
  const auto match = [id](const ZoomCue &cue) { return cue.id == id; };
  const qsizetype before = track.cues.size();
  track.cues.removeIf(match);
  return track.cues.size() != before;
}

const ZoomCue *zoomCueAt(const ZoomTrack &track, qint64 timeMs) {
  for (const ZoomCue &cue : track.cues) {
    if (timeMs >= cue.startMs && timeMs < cue.endMs)
      return &cue;
  }
  return nullptr;
}
