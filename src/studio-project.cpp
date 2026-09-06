#include "studio-project.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 maxBytes = 4 * 1024 * 1024LL;
constexpr qint64 maxDuration = 7 * 24 * 60 * 60 * 1000LL;
constexpr qsizetype maxItems = 1000;
bool finishSceneChange(StudioProject &, StudioProject, QString &,
                       quint64 originalId = 0, quint64 duplicateId = 0);
qint64 frameSnapped(const StudioProject &p, qint64 requested, bool floor) {
  if (p.fpsNumerator <= 0 || p.fpsDenominator <= 0 || requested <= 0)
    return 0;
  const double frameMs = 1000.0 * p.fpsDenominator / p.fpsNumerator;
  const double count = static_cast<double>(requested) / frameMs;
  const auto frames =
      floor ? static_cast<qint64>(std::floor(count)) : qRound64(count);
  return qRound64(static_cast<double>(frames) * frameMs);
}
qint64 clipDuration(const StudioClip &clip) {
  if (!std::isfinite(clip.speed) || clip.speed < 0.125 || clip.speed > 8 ||
      clip.inMs < 0 || clip.outMs <= clip.inMs || clip.outMs > maxDuration)
    return 0;
  return qRound64(static_cast<double>(clip.outMs - clip.inMs) / clip.speed);
}
struct ClipSplit {
  StudioClip left;
  StudioClip right;
  qint64 leftMs = 0;
};
std::optional<ClipSplit> splitAt(const StudioClip &clip, qint64 offset,
                                 qint64 maxSnapMs) {
  const qint64 duration = clipDuration(clip);
  if (offset <= 0 || offset >= duration || clip.outMs - clip.inMs < 2)
    return std::nullopt;
  const qint64 centre =
      clip.inMs + qRound64(static_cast<double>(offset) * clip.speed);
  std::optional<ClipSplit> best;
  const int radius = qCeil(clip.speed);
  for (int delta = -radius; delta <= radius; ++delta) {
    const qint64 boundary = centre + delta;
    if (boundary <= clip.inMs || boundary >= clip.outMs)
      continue;
    StudioClip left = clip, right = clip;
    left.outMs = boundary;
    right.inMs = boundary;
    const qint64 leftMs = clipDuration(left), rightMs = clipDuration(right);
    if (leftMs <= 0 || rightMs <= 0 || leftMs + rightMs != duration ||
        qAbs(leftMs - offset) > maxSnapMs)
      continue;
    if (!best || qAbs(leftMs - offset) < qAbs(best->leftMs - offset))
      best = ClipSplit{left, right, leftMs};
  }
  return best;
}
qint64 frameMilliseconds(const StudioProject &p) {
  return qCeil(1000.0 * p.fpsDenominator / p.fpsNumerator);
}
bool unusedClipId(const StudioProject &p, quint64 id) {
  if (!id)
    return false;
  for (const auto &clip : p.clips)
    if (clip.id == id)
      return false;
  return true;
}
bool intersectsBlend(const StudioProject &p, qint64 from, qint64 to) {
  const auto spans = studioComposition(p);
  for (qsizetype i = 1; i < spans.size(); ++i)
    if (qMax(from, spans[i].startMs) < qMin(to, spans[i - 1].endMs))
      return true;
  return false;
}
qint64 serialTime(const StudioProject &p, qint64 time) {
  qint64 serial = 0;
  for (const auto &span : studioComposition(p)) {
    if (time >= span.startMs && time < span.endMs)
      return serial + time - span.startMs;
    serial += span.endMs - span.startMs;
  }
  return serial;
}
void rippleZoomAndRange(const StudioProject &before, StudioProject &after,
                        qint64 from, qint64 to) {
  after.zoom.cues.clear();
  for (auto cue : before.zoom.cues) {
    cue.startMs = studioTimeAfterDelete(cue.startMs, from, to);
    cue.endMs = studioTimeAfterDelete(cue.endMs, from, to);
    if (cue.endMs - cue.startMs >= kMinCueMs)
      after.zoom.cues.push_back(cue);
  }
  after.trimInMs = studioTimeAfterDelete(before.trimInMs, from, to);
  after.trimOutMs = before.trimOutMs < 0
                        ? -1
                        : studioTimeAfterDelete(before.trimOutMs, from, to);
  if (studioDuration(after) == 0 ||
      (after.trimOutMs >= 0 && after.trimOutMs <= after.trimInMs)) {
    after.trimInMs = 0;
    after.trimOutMs = -1;
  }
}
/** Split temporary pieces without allocating identity: discarded middle pieces
 * never need IDs, and only a surviving second occurrence gets a new one. */
std::optional<qint64> splitPieces(QVector<StudioClip> &pieces, qint64 at,
                                  qint64 maxSnapMs) {
  qint64 start = 0;
  for (qsizetype i = 0; i < pieces.size(); ++i) {
    const qint64 end = start + clipDuration(pieces[i]);
    if (at == start || at == end)
      return at;
    if (at > start && at < end) {
      const auto split = splitAt(pieces[i], at - start, maxSnapMs);
      if (!split)
        return std::nullopt;
      pieces[i] = split->left;
      pieces.insert(i + 1, split->right);
      return start + split->leftMs;
    }
    start = end;
  }
  return std::nullopt;
}
bool integer(const QJsonValue &value, qint64 low, qint64 high) {
  if (!value.isDouble())
    return false;
  const double number = value.toDouble();
  return std::isfinite(number) && number == std::floor(number) &&
         number >= static_cast<double>(low) &&
         number <= static_cast<double>(high);
}
quint64 idFrom(const QJsonValue &value) {
  if (!value.isString())
    return 0;
  bool ok = false;
  const quint64 id = value.toString().toULongLong(&ok);
  return ok ? id : 0;
}
QJsonObject sourceJson(const StudioSource &s) {
  return {{"width", s.size.width()},
          {"height", s.size.height()},
          {"fpsNumerator", s.fpsNumerator},
          {"fpsDenominator", s.fpsDenominator},
          {"rotation", s.rotation},
          {"unsupportedTransform", s.unsupportedTransform},
          {"durationMs", s.durationMs},
          {"audioStreams", s.audioStreams}};
}
} // namespace

const StudioAsset *studioAsset(const StudioProject &p, quint64 id) {
  for (const auto &asset : p.assets)
    if (asset.id == id)
      return &asset;
  return nullptr;
}
const StudioTransition *studioTransition(const StudioProject &p,
                                         quint64 outgoing, quint64 incoming) {
  for (const auto &transition : p.transitions)
    if (transition.outgoingClipId == outgoing &&
        transition.incomingClipId == incoming)
      return &transition;
  return nullptr;
}
qint64 studioTransitionMaximum(const StudioProject &p, quint64 outgoing,
                               quint64 incoming) {
  for (qsizetype i = 0; i + 1 < p.clips.size(); ++i)
    if (p.clips[i].id == outgoing && p.clips[i + 1].id == incoming)
      return frameSnapped(p,
                          qMax<qint64>(0, (qMin(clipDuration(p.clips[i]),
                                                clipDuration(p.clips[i + 1])) -
                                           250) /
                                              2),
                          true);
  return 0;
}
QVector<StudioSpan> studioComposition(const StudioProject &p) {
  QVector<StudioSpan> spans;
  QHash<quint64, const StudioTransition *> outgoingTransitions;
  for (const auto &transition : p.transitions)
    outgoingTransitions.insert(transition.outgoingClipId, &transition);
  qint64 position = 0;
  for (const auto &clip : p.clips) {
    if (!spans.isEmpty()) {
      const auto *transition =
          outgoingTransitions.value(spans.back().clipId, nullptr);
      if (transition && transition->incomingClipId == clip.id) {
        if (transition->durationMs <= 0 || transition->durationMs > position)
          return {};
        position -= transition->durationMs;
      }
    }
    const qint64 duration = clipDuration(clip);
    if (position < 0 || duration <= 0 || duration > maxDuration - position)
      return {};
    spans.push_back({clip.id, clip.assetId, position, position + duration,
                     clip.inMs, clip.outMs, clip.speed});
    position += duration;
  }
  return spans;
}
std::optional<StudioBlend> studioBlendAt(const StudioProject &p, qint64 time) {
  const auto spans = studioComposition(p);
  for (qsizetype i = 1; i < spans.size(); ++i) {
    const auto &outgoing = spans[i - 1], &incoming = spans[i];
    if (time < incoming.startMs || time >= outgoing.endMs)
      continue;
    const auto *transition =
        studioTransition(p, outgoing.clipId, incoming.clipId);
    if (!transition || transition->durationMs <= 0)
      continue;
    const auto frame = [time](const StudioSpan &span) {
      return StudioFrame{
          span,
          qBound(span.inMs,
                 span.inMs + qRound64(static_cast<double>(time - span.startMs) *
                                      span.speed),
                 span.outMs - 1)};
    };
    const double progress = static_cast<double>(time - incoming.startMs) /
                            static_cast<double>(transition->durationMs);
    const bool black = transition->kind == StudioTransitionKind::FadeBlack;
    return StudioBlend{frame(outgoing),
                       frame(incoming),
                       transition->kind,
                       progress,
                       black ? qMax(0.0, 1 - 2 * progress) : 1 - progress,
                       black ? qMax(0.0, 2 * progress - 1) : progress,
                       1 - progress,
                       progress};
  }
  return std::nullopt;
}
qint64 studioDuration(const StudioProject &p) {
  const auto spans = studioComposition(p);
  return spans.isEmpty() ? 0 : spans.back().endMs;
}
std::optional<StudioFrame> studioFrameAt(const StudioProject &p, qint64 time) {
  for (const auto &span : studioComposition(p)) {
    if (time >= span.startMs && time < span.endMs) {
      const qint64 source =
          span.inMs +
          qRound64(static_cast<double>(time - span.startMs) * span.speed);
      return StudioFrame{span, qBound(span.inMs, source, span.outMs - 1)};
    }
  }
  return std::nullopt;
}
std::optional<qint64> studioTimelineTime(const StudioProject &p, quint64 id,
                                         qint64 source) {
  for (const auto &span : studioComposition(p)) {
    if (span.clipId == id && source >= span.inMs && source < span.outMs)
      return qMin(
          span.endMs - 1,
          span.startMs +
              qRound64(static_cast<double>(source - span.inMs) / span.speed));
  }
  return std::nullopt;
}
QString validateStudioProject(const StudioProject &p) {
  if (p.assets.size() > maxItems || p.clips.size() > maxItems)
    return QStringLiteral("Project exceeds the 1000 asset/scene limit.");
  if (p.canvas.width() < 2 || p.canvas.height() < 2 ||
      p.canvas.width() % 2 != 0 || p.canvas.height() % 2 != 0 ||
      p.canvas.width() > 16384 || p.canvas.height() > 16384 ||
      p.fpsNumerator < 1 || p.fpsNumerator > 1000000 || p.fpsDenominator < 1 ||
      p.fpsDenominator > 1000000 ||
      static_cast<double>(p.fpsNumerator) / p.fpsDenominator > 240)
    return QStringLiteral("Invalid project canvas or frame rate.");
  if (p.style.background < 0 || p.style.background > 3 || p.style.padding < 0 ||
      p.style.padding > 40 || p.style.radius < 0 || p.style.radius > 100)
    return QStringLiteral("Invalid project canvas styling.");
  QSet<quint64> assets;
  for (const auto &a : p.assets) {
    if (!a.id || assets.contains(a.id) || a.path.isEmpty() ||
        a.path.size() > 32768 || a.path.contains(QChar::Null) ||
        !a.source.usable() || a.source.size.width() > 32768 ||
        a.source.size.height() > 32768 || a.source.fpsNumerator > 1000000 ||
        a.source.fpsDenominator > 1000000 || a.source.durationMs <= 0 ||
        a.source.durationMs > maxDuration || a.source.audioStreams < 0 ||
        a.source.audioStreams > 64 ||
        (a.source.rotation != 0 && a.source.rotation != 90 &&
         a.source.rotation != 180 && a.source.rotation != 270))
      return QStringLiteral("Invalid or duplicate source asset.");
    assets.insert(a.id);
  }
  QSet<quint64> clips;
  qint64 total = 0;
  for (const auto &c : p.clips) {
    const auto *a = studioAsset(p, c.assetId);
    const auto duration = clipDuration(c);
    if (!c.id || clips.contains(c.id) || !a || duration <= 0 ||
        c.outMs > a->source.durationMs || duration > maxDuration - total)
      return QStringLiteral(
          "Invalid scene identity, source range, speed, or duration.");
    clips.insert(c.id);
    total += duration;
  }
  if (p.transitions.size() > qMax<qsizetype>(0, p.clips.size() - 1))
    return QStringLiteral("Too many scene transitions.");
  QSet<quint64> boundaries;
  for (const auto &t : p.transitions) {
    if (!t.outgoingClipId || !t.incomingClipId ||
        boundaries.contains(t.outgoingClipId) ||
        (t.kind != StudioTransitionKind::Crossfade &&
         t.kind != StudioTransitionKind::FadeBlack) ||
        t.durationMs <= 0 ||
        t.durationMs >
            studioTransitionMaximum(p, t.outgoingClipId, t.incomingClipId) ||
        frameSnapped(p, t.durationMs, false) != t.durationMs)
      return QStringLiteral("Invalid transition pair or duration; preserve 250 "
                            "ms between neighboring blends.");
    boundaries.insert(t.outgoingClipId);
    total -= t.durationMs;
  }
  if (p.trimInMs < 0 || p.trimInMs > total || p.trimOutMs < -1 ||
      (p.trimOutMs != -1 && (p.trimOutMs > total || p.trimOutMs < p.trimInMs)))
    return QStringLiteral("Invalid project review/export range.");
  if (p.zoom.cues.size() > kMaxZoomCues)
    return QStringLiteral("Too many zoom cues.");
  QSet<quint64> cues;
  for (const auto &c : p.zoom.cues) {
    if (!c.id || c.id > 9007199254740991ULL || cues.contains(c.id) ||
        c.startMs < 0 || c.endMs <= c.startMs || c.endMs > total ||
        c.endMs - c.startMs < kMinCueMs || c.easeInMs < 0 || c.easeOutMs < 0 ||
        c.easeInMs > maxDuration || c.easeOutMs > maxDuration ||
        !std::isfinite(c.scale) || c.scale < kMinZoomScale ||
        c.scale > kMaxZoomScale || !std::isfinite(c.target.x()) ||
        !std::isfinite(c.target.y()) || c.target.x() < 0 || c.target.x() > 1 ||
        c.target.y() < 0 || c.target.y() > 1)
      return QStringLiteral("Invalid zoom cue.");
    cues.insert(c.id);
  }
  const auto sorted = sortedCues(p.zoom);
  if (sorted.size() != p.zoom.cues.size())
    return QStringLiteral("Overlapping zoom cues.");
  for (const auto &c : sorted)
    if (!p.zoom.cues.contains(c))
      return QStringLiteral("Overlapping zoom cues.");
  return {};
}
QByteArray encodeStudioProject(const StudioProject &p) {
  QJsonArray assets, clips, transitions;
  for (const auto &a : p.assets)
    assets.append(QJsonObject{{"id", QString::number(a.id)},
                              {"path", a.path},
                              {"source", sourceJson(a.source)}});
  for (const auto &c : p.clips)
    clips.append(QJsonObject{{"id", QString::number(c.id)},
                             {"assetId", QString::number(c.assetId)},
                             {"inMs", c.inMs},
                             {"outMs", c.outMs},
                             {"speed", c.speed}});
  for (const auto &t : p.transitions)
    transitions.append(QJsonObject{
        {"outgoingClipId", QString::number(t.outgoingClipId)},
        {"incomingClipId", QString::number(t.incomingClipId)},
        {"kind", t.kind == StudioTransitionKind::Crossfade ? "crossfade"
                                                           : "fade-black"},
        {"durationMs", t.durationMs}});
  return QJsonDocument(
             QJsonObject{
                 {"schema", StudioProject::kSchema},
                 {"assets", assets},
                 {"clips", clips},
                 {"transitions", transitions},
                 {"canvas", QJsonObject{{"width", p.canvas.width()},
                                        {"height", p.canvas.height()},
                                        {"fpsNumerator", p.fpsNumerator},
                                        {"fpsDenominator", p.fpsDenominator}}},
                 {"style", QJsonObject{{"background", p.style.background},
                                       {"padding", p.style.padding},
                                       {"radius", p.style.radius}}},
                 {"zoom", writeZoomTrack(p.zoom)},
                 {"trimInMs", p.trimInMs},
                 {"trimOutMs", p.trimOutMs}})
      .toJson(QJsonDocument::Indented);
}
QString decodeStudioProject(const QByteArray &data, StudioProject &out) {
  const QString malformed = QStringLiteral("Malformed Studio project.");
  if (data.size() > maxBytes)
    return QStringLiteral("Project exceeds the 4 MiB limit.");
  QJsonParseError parseError;
  const auto doc = QJsonDocument::fromJson(data, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject())
    return malformed;
  const auto root = doc.object();
  if (!integer(root["schema"], StudioProject::kSchema, StudioProject::kSchema))
    return QStringLiteral("Unsupported Studio project schema.");
  if (!root["assets"].isArray() || !root["clips"].isArray() ||
      !root["transitions"].isArray() || !root["canvas"].isObject() ||
      !root["style"].isObject() || !root["zoom"].isObject() ||
      !integer(root["trimInMs"], 0, maxDuration) ||
      !integer(root["trimOutMs"], -1, maxDuration))
    return malformed;
  const auto assets = root["assets"].toArray(), clips = root["clips"].toArray();
  if (assets.size() > maxItems || clips.size() > maxItems ||
      root["transitions"].toArray().size() > maxItems)
    return QStringLiteral("Project exceeds the 1000 asset/scene limit.");
  StudioProject p;
  p.trimInMs = root["trimInMs"].toInteger();
  p.trimOutMs = root["trimOutMs"].toInteger();
  const auto canvas = root["canvas"].toObject(),
             style = root["style"].toObject();
  for (const auto *key : {"width", "height", "fpsNumerator", "fpsDenominator"})
    if (!integer(canvas[key], 1, 1000000))
      return malformed;
  for (const auto *key : {"background", "padding", "radius"})
    if (!integer(style[key], 0, 100))
      return malformed;
  p.canvas = {canvas["width"].toInt(), canvas["height"].toInt()};
  p.fpsNumerator = canvas["fpsNumerator"].toInt();
  p.fpsDenominator = canvas["fpsDenominator"].toInt();
  p.style = {style["background"].toInt(), style["padding"].toInt(),
             style["radius"].toInt()};
  for (const auto &value : assets) {
    const auto a = value.toObject();
    const auto s = a["source"].toObject();
    if (!value.isObject() || !a["path"].isString() || !a["source"].isObject() ||
        !s["unsupportedTransform"].isBool() ||
        !integer(s["durationMs"], 1, maxDuration) ||
        !integer(s["rotation"], 0, 270) || !integer(s["audioStreams"], 0, 64))
      return malformed;
    for (const auto *key :
         {"width", "height", "fpsNumerator", "fpsDenominator"})
      if (!integer(s[key], 1, 1000000))
        return malformed;
    StudioSource source;
    source.size = {s["width"].toInt(), s["height"].toInt()};
    source.fpsNumerator = s["fpsNumerator"].toInt();
    source.fpsDenominator = s["fpsDenominator"].toInt();
    source.rotation = s["rotation"].toInt();
    source.unsupportedTransform = s["unsupportedTransform"].toBool();
    source.durationMs = s["durationMs"].toInteger();
    source.audioStreams = s["audioStreams"].toInt();
    p.assets.push_back({idFrom(a["id"]), a["path"].toString(), source});
  }
  for (const auto &value : clips) {
    const auto c = value.toObject();
    if (!value.isObject() || !integer(c["inMs"], 0, maxDuration) ||
        !integer(c["outMs"], 0, maxDuration) || !c["speed"].isDouble())
      return malformed;
    p.clips.push_back({idFrom(c["id"]), idFrom(c["assetId"]),
                       c["inMs"].toInteger(), c["outMs"].toInteger(),
                       c["speed"].toDouble()});
  }
  for (const auto &value : root["transitions"].toArray()) {
    const auto t = value.toObject();
    const auto kind = t["kind"].toString();
    if (!value.isObject() || !integer(t["durationMs"], 1, maxDuration) ||
        (kind != QStringLiteral("crossfade") &&
         kind != QStringLiteral("fade-black")))
      return malformed;
    p.transitions.push_back(
        {idFrom(t["outgoingClipId"]), idFrom(t["incomingClipId"]),
         kind == QStringLiteral("crossfade") ? StudioTransitionKind::Crossfade
                                             : StudioTransitionKind::FadeBlack,
         t["durationMs"].toInteger()});
  }
  const auto zoom = root["zoom"].toObject();
  if (!zoom["cues"].isArray() || zoom["cues"].toArray().size() > kMaxZoomCues)
    return malformed;
  for (const auto &value : zoom["cues"].toArray()) {
    const auto c = value.toObject();
    if (!value.isObject() || !integer(c["id"], 1, 9007199254740991LL) ||
        !integer(c["startMs"], 0, maxDuration) ||
        !integer(c["endMs"], 0, maxDuration) ||
        !integer(c["easeInMs"], 0, maxDuration) ||
        !integer(c["easeOutMs"], 0, maxDuration) || !c["scale"].isDouble() ||
        !c["targetX"].isDouble() || !c["targetY"].isDouble())
      return malformed;
  }
  QString error;
  if (!readZoomTrack(zoom, p.zoom, error))
    return error;
  error = validateStudioProject(p);
  if (!error.isEmpty())
    return error;
  out = p;
  return {};
}
StudioProjectLoad loadStudioProject(const QString &path) {
  StudioProjectLoad result;
  QFile file(path);
  if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
    result.error = QStringLiteral("Cannot open project: %1").arg(path);
    return result;
  }
  result.error = decodeStudioProject(file.read(maxBytes + 1), result.project);
  if (!result.error.isEmpty())
    return result;
  const QDir folder = QFileInfo(path).absoluteDir();
  for (auto &asset : result.project.assets) {
    if (QDir::isRelativePath(asset.path))
      asset.path = folder.absoluteFilePath(asset.path);
    if (!QFileInfo(asset.path).isFile())
      result.missingAssets.push_back(asset.id);
  }
  return result;
}
QString saveStudioProject(const QString &path, const StudioProject &project) {
  if (QFileInfo(path).isSymLink())
    return QStringLiteral("Refusing to save through a project symlink.");
  const auto validation = validateStudioProject(project);
  if (!validation.isEmpty())
    return validation;
  const QByteArray data = encodeStudioProject(project);
  if (data.size() > maxBytes)
    return QStringLiteral("Project exceeds the 4 MiB limit.");
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      file.write(data) != data.size() || !file.commit())
    return QStringLiteral("Could not save project: %1").arg(file.errorString());
  return {};
}
bool studioSplitClip(StudioProject &p, qint64 at, quint64 newId,
                     QString *error) {
  if (error)
    error->clear();
  if (!validateStudioProject(p).isEmpty() || !unusedClipId(p, newId) ||
      p.clips.size() >= maxItems)
    return false;
  if (const auto blend = studioBlendAt(p, at)) {
    if (blend->progress > 0) {
      if (error)
        *error = QStringLiteral(
            "Remove the transition before splitting through it.");
      return false;
    }
  }
  const auto spans = studioComposition(p);
  for (qsizetype i = 0; i < spans.size(); ++i) {
    if (at <= spans[i].startMs || at >= spans[i].endMs)
      continue;
    const auto split =
        splitAt(p.clips[i], at - spans[i].startMs, frameMilliseconds(p));
    if (!split)
      return false;
    auto edited = p;
    edited.clips[i] = split->left;
    auto right = split->right;
    right.id = newId;
    edited.clips.insert(i + 1, right);
    for (auto &transition : edited.transitions)
      if (transition.outgoingClipId == split->left.id)
        transition.outgoingClipId = right.id;
    if (!validateStudioProject(edited).isEmpty()) {
      if (error)
        *error = QStringLiteral("Leave more source room beside the transition, "
                                "or remove it before splitting.");
      return false;
    }
    p = std::move(edited);
    return true;
  }
  return false;
}
qint64 studioTimeAfterDelete(qint64 time, qint64 from, qint64 to) {
  if (to <= from || time <= from)
    return time;
  return time < to ? from : time - (to - from);
}
StudioCutResult studioDeleteRange(StudioProject &p, qint64 from, qint64 to,
                                  quint64 remainderId) {
  if (!validateStudioProject(p).isEmpty())
    return {};
  const qint64 duration = studioDuration(p);
  if (from > to)
    std::swap(from, to);
  from = qBound<qint64>(0, from, duration);
  to = qBound<qint64>(0, to, duration);
  if (from == to)
    return {};
  if (intersectsBlend(p, from, to))
    return {false, 0, 0, 0,
            QStringLiteral("Remove the transition before cutting through it.")};
  if (!p.transitions.isEmpty()) {
    StudioProject serial = p;
    serial.transitions.clear();
    serial.zoom.cues.clear();
    serial.trimInMs = 0;
    serial.trimOutMs = -1;
    const qint64 serialFrom = serialTime(p, from), serialTo = serialTime(p, to);
    const auto result =
        studioDeleteRange(serial, serialFrom, serialTo, remainderId);
    if (!result.changed)
      return result;
    const qint64 actualFrom = from + result.fromMs - serialFrom;
    const qint64 actualTo = actualFrom + result.removedMs;
    if (intersectsBlend(p, actualFrom, actualTo))
      return {false, 0, 0, 0,
              QStringLiteral("The snapped cut enters a transition; remove the "
                             "transition first.")};
    serial.transitions = p.transitions;
    const auto oldFrame = studioFrameAt(p, qMin(actualFrom, duration - 1));
    if (oldFrame && remainderId && !unusedClipId(serial, remainderId))
      for (auto &transition : serial.transitions)
        if (transition.outgoingClipId == oldFrame->span.clipId)
          transition.outgoingClipId = remainderId;
    rippleZoomAndRange(p, serial, actualFrom, actualTo);
    if (!validateStudioProject(serial).isEmpty() ||
        duration - studioDuration(serial) != result.removedMs)
      return {false, 0, 0, 0,
              QStringLiteral("Leave more source room beside the transition, or "
                             "remove it before cutting.")};
    p = std::move(serial);
    return {true, actualFrom, actualTo, result.removedMs, {}};
  }
  QVector<StudioClip> pieces = p.clips;
  const auto end = splitPieces(pieces, to, frameMilliseconds(p));
  if (!end)
    return {};
  const auto start = splitPieces(pieces, from, frameMilliseconds(p));
  if (!start || *start >= *end)
    return {};
  StudioProject edited = p;
  edited.clips.clear();
  qint64 position = 0;
  QSet<quint64> ids;
  for (auto clip : pieces) {
    const qint64 pieceEnd = position + clipDuration(clip);
    if (pieceEnd <= *start || position >= *end) {
      if (ids.contains(clip.id)) {
        if (!unusedClipId(p, remainderId) || ids.contains(remainderId))
          return {};
        clip.id = remainderId;
      }
      ids.insert(clip.id);
      edited.clips.push_back(clip);
    }
    position = pieceEnd;
  }
  if (edited.clips.size() > maxItems)
    return {};
  const qint64 removed = duration - studioDuration(edited);
  if (removed != *end - *start)
    return {};
  edited.zoom.cues.clear();
  for (auto cue : p.zoom.cues) {
    cue.startMs = studioTimeAfterDelete(cue.startMs, *start, *end);
    cue.endMs = studioTimeAfterDelete(cue.endMs, *start, *end);
    if (cue.endMs - cue.startMs >= kMinCueMs)
      edited.zoom.cues.push_back(cue);
  }
  normalizeZoomTrack(edited.zoom, studioDuration(edited));
  edited.trimInMs = studioTimeAfterDelete(p.trimInMs, *start, *end);
  if (p.trimOutMs >= 0)
    edited.trimOutMs = studioTimeAfterDelete(p.trimOutMs, *start, *end);
  if (studioDuration(edited) == 0 ||
      (edited.trimOutMs >= 0 && edited.trimOutMs <= edited.trimInMs)) {
    edited.trimInMs = 0;
    edited.trimOutMs = -1;
  }
  if (!validateStudioProject(edited).isEmpty())
    return {};
  p = edited;
  return {true, *start, *end, removed, {}};
}
StudioCutResult studioDeleteClip(StudioProject &p, quint64 id) {
  if (!p.transitions.isEmpty()) {
    const auto validation = validateStudioProject(p);
    if (!validation.isEmpty())
      return {false, 0, 0, 0, validation};
    for (const auto &span : studioComposition(p)) {
      if (span.clipId != id)
        continue;
      const qint64 before = studioDuration(p);
      auto edited = p;
      edited.clips.removeIf(
          [id](const StudioClip &clip) { return clip.id == id; });
      QString error;
      if (!finishSceneChange(p, std::move(edited), error))
        return {false, 0, 0, 0, error};
      const qint64 removed = before - studioDuration(p);
      return {true, span.startMs, span.startMs + removed, removed, {}};
    }
    return {};
  }
  for (const auto &span : studioComposition(p))
    if (span.clipId == id)
      return studioDeleteRange(p, span.startMs, span.endMs, 0);
  return {};
}

namespace {
struct ZoomFragment {
  ZoomCue cue;
  qint64 oldStartMs = 0;
  qint64 oldEndMs = 0;
};
std::optional<ZoomFragment> sceneZoom(const ZoomCue &cue,
                                      const StudioSpan &oldSpan,
                                      const StudioSpan &newSpan) {
  const qint64 oldStart = qMax(cue.startMs, oldSpan.startMs);
  const qint64 oldEnd = qMin(cue.endMs, oldSpan.endMs);
  if (oldStart >= oldEnd)
    return std::nullopt;
  const double sourceStart =
      oldStart == oldSpan.startMs
          ? oldSpan.inMs
          : static_cast<double>(oldSpan.inMs) +
                static_cast<double>(oldStart - oldSpan.startMs) * oldSpan.speed;
  const double sourceEnd =
      oldEnd == oldSpan.endMs
          ? oldSpan.outMs
          : static_cast<double>(oldSpan.inMs) +
                static_cast<double>(oldEnd - oldSpan.startMs) * oldSpan.speed;
  const double keptStart = qMax(sourceStart, static_cast<double>(newSpan.inMs));
  const double keptEnd = qMin(sourceEnd, static_cast<double>(newSpan.outMs));
  if (keptStart >= keptEnd)
    return std::nullopt;
  auto mapped = cue;
  mapped.startMs =
      qBound(newSpan.startMs,
             newSpan.startMs +
                 qRound64((keptStart - static_cast<double>(newSpan.inMs)) /
                          newSpan.speed),
             newSpan.endMs);
  mapped.endMs = qBound(
      newSpan.startMs,
      newSpan.startMs + qRound64((keptEnd - static_cast<double>(newSpan.inMs)) /
                                 newSpan.speed),
      newSpan.endMs);
  if (mapped.startMs >= mapped.endMs)
    return std::nullopt;
  return ZoomFragment{mapped, oldStart, oldEnd};
}
bool finishSceneChange(StudioProject &project, StudioProject edited,
                       QString &error, quint64 originalId,
                       quint64 duplicateId) {
  edited.trimInMs = 0;
  edited.trimOutMs = -1;
  edited.zoom.cues.clear();
  // A transition belongs to one directed pair. Arrangement never transfers it
  // to a different neighbor; short trims clamp to the remaining source handles.
  QVector<StudioTransition> retained;
  for (auto transition : edited.transitions) {
    const auto maximum = studioTransitionMaximum(
        edited, transition.outgoingClipId, transition.incomingClipId);
    if (maximum <= 0)
      continue;
    transition.durationMs = qMin(transition.durationMs, maximum);
    retained.push_back(transition);
  }
  edited.transitions = std::move(retained);
  error = validateStudioProject(edited);
  if (!error.isEmpty())
    return false;
  const auto oldSpans = studioComposition(project);
  QHash<quint64, StudioSpan> newSpans;
  for (const auto &span : studioComposition(edited))
    newSpans.insert(span.clipId, span);
  quint64 nextCueId = 1;
  for (const auto &cue : project.zoom.cues)
    nextCueId = qMax(nextCueId, cue.id + 1);
  for (const auto &cue : project.zoom.cues) {
    QVector<ZoomFragment> originals, duplicates;
    for (const auto &oldSpan : oldSpans) {
      const auto found = newSpans.constFind(oldSpan.clipId);
      if (found != newSpans.cend()) {
        const auto mapped = sceneZoom(cue, oldSpan, *found);
        if (mapped) {
          // Appending a scene must not fragment an otherwise unchanged cue.
          // Merge only pieces still adjacent in both old and new order.
          if (!originals.isEmpty() &&
              originals.back().oldEndMs >= mapped->oldStartMs &&
              originals.back().cue.endMs >= mapped->cue.startMs &&
              originals.back().cue.startMs <= mapped->cue.endMs) {
            originals.back().cue.startMs =
                qMin(originals.back().cue.startMs, mapped->cue.startMs);
            originals.back().cue.endMs =
                qMax(originals.back().cue.endMs, mapped->cue.endMs);
            originals.back().oldEndMs =
                qMax(originals.back().oldEndMs, mapped->oldEndMs);
          } else
            originals.push_back(*mapped);
        }
      }
      if (oldSpan.clipId == originalId && newSpans.contains(duplicateId)) {
        const auto mapped =
            sceneZoom(cue, oldSpan, newSpans.value(duplicateId));
        if (mapped)
          duplicates.push_back(*mapped);
      }
    }
    bool retainedId = false;
    const auto append = [&](const ZoomFragment &fragment, bool duplicate) {
      if (fragment.cue.endMs - fragment.cue.startMs < kMinCueMs)
        return true;
      if (edited.zoom.cues.size() >= kMaxZoomCues ||
          ((duplicate || retainedId) && nextCueId > 9007199254740991ULL)) {
        error = QStringLiteral("Scene change would exceed the %1 zoom-cue "
                               "limit. Remove zooms before retrying.")
                    .arg(kMaxZoomCues);
        return false;
      }
      auto result = fragment.cue;
      if (duplicate || retainedId)
        result.id = nextCueId++;
      else
        retainedId = true;
      edited.zoom.cues.push_back(result);
      return true;
    };
    for (const auto &fragment : originals)
      if (!append(fragment, false))
        return false;
    for (const auto &fragment : duplicates)
      if (!append(fragment, true))
        return false;
  }
  std::sort(
      edited.zoom.cues.begin(), edited.zoom.cues.end(),
      [](const ZoomCue &a, const ZoomCue &b) { return a.startMs < b.startMs; });
  error = validateStudioProject(edited);
  if (error == QStringLiteral("Overlapping zoom cues."))
    error = QStringLiteral("Move zoom cues away from this boundary before "
                           "adding or changing a transition.");
  if (!error.isEmpty())
    return false;
  project = std::move(edited);
  return true;
}
qsizetype clipIndex(const StudioProject &p, quint64 id) {
  for (qsizetype i = 0; i < p.clips.size(); ++i)
    if (p.clips[i].id == id)
      return i;
  return -1;
}
} // namespace
bool studioInsertScenes(StudioProject &p, const QVector<StudioAsset> &assets,
                        const QVector<StudioClip> &clips, qsizetype index,
                        QString &error) {
  error = validateStudioProject(p);
  if (!error.isEmpty())
    return false;
  if (clips.isEmpty()) {
    if (!assets.isEmpty())
      error = QStringLiteral("Import must contain at least one scene.");
    return false;
  }
  if (index < 0 || index > p.clips.size()) {
    error = QStringLiteral("The scene insertion boundary no longer exists.");
    return false;
  }
  auto edited = p;
  edited.assets.append(assets);
  for (const auto &clip : clips)
    edited.clips.insert(index++, clip);
  return finishSceneChange(p, std::move(edited), error);
}
bool studioMoveClip(StudioProject &p, quint64 id, quint64 beforeId,
                    QString &error) {
  error = validateStudioProject(p);
  if (!error.isEmpty())
    return false;
  const auto index = clipIndex(p, id);
  if (index < 0 || (beforeId && clipIndex(p, beforeId) < 0)) {
    error = QStringLiteral(
        "The scene to move or insertion boundary no longer exists.");
    return false;
  }
  if (id == beforeId)
    return false;
  auto edited = p;
  const auto clip = edited.clips.takeAt(index);
  const auto destination =
      beforeId ? clipIndex(edited, beforeId) : edited.clips.size();
  edited.clips.insert(destination, clip);
  if (edited.clips == p.clips)
    return false;
  return finishSceneChange(p, std::move(edited), error);
}
bool studioDuplicateClip(StudioProject &p, quint64 id, quint64 newId,
                         QString &error) {
  error = validateStudioProject(p);
  if (!error.isEmpty())
    return false;
  const auto index = clipIndex(p, id);
  if (index < 0 || !unusedClipId(p, newId)) {
    error = QStringLiteral(
        "Cannot duplicate a missing scene or reuse its identity.");
    return false;
  }
  auto edited = p;
  auto copy = p.clips[index];
  copy.id = newId;
  edited.clips.insert(index + 1, copy);
  return finishSceneChange(p, std::move(edited), error, id, newId);
}
bool studioTrimClip(StudioProject &p, quint64 id, qint64 in, qint64 out,
                    QString &error) {
  error = validateStudioProject(p);
  if (!error.isEmpty())
    return false;
  const auto index = clipIndex(p, id);
  if (index < 0) {
    error = QStringLiteral("The scene to trim no longer exists.");
    return false;
  }
  if (p.clips[index].inMs == in && p.clips[index].outMs == out)
    return false;
  auto edited = p;
  edited.clips[index].inMs = in;
  edited.clips[index].outMs = out;
  return finishSceneChange(p, std::move(edited), error);
}
bool studioSetTransition(StudioProject &p, quint64 outgoing, quint64 incoming,
                         StudioTransitionKind kind, qint64 requested,
                         QString &error) {
  error = validateStudioProject(p);
  if (!error.isEmpty())
    return false;
  const auto maximum = studioTransitionMaximum(p, outgoing, incoming);
  if (maximum <= 0 || requested <= 0 ||
      (kind != StudioTransitionKind::Crossfade &&
       kind != StudioTransitionKind::FadeBlack)) {
    error = QStringLiteral("These scenes are too short for a transition; leave "
                           "250 ms between blends.");
    return false;
  }
  const auto duration =
      qBound<qint64>(frameSnapped(p, frameMilliseconds(p), false),
                     frameSnapped(p, qMin(requested, maximum), false), maximum);
  const StudioTransition transition{outgoing, incoming, kind, duration};
  if (const auto *existing = studioTransition(p, outgoing, incoming))
    if (*existing == transition)
      return false;
  auto edited = p;
  edited.transitions.removeIf([outgoing](const StudioTransition &t) {
    return t.outgoingClipId == outgoing;
  });
  edited.transitions.push_back(transition);
  return finishSceneChange(p, std::move(edited), error);
}
bool studioRemoveTransition(StudioProject &p, quint64 outgoing,
                            quint64 incoming, QString &error) {
  error = validateStudioProject(p);
  if (!error.isEmpty() || !studioTransition(p, outgoing, incoming))
    return false;
  auto edited = p;
  edited.transitions.removeIf([outgoing, incoming](const StudioTransition &t) {
    return t.outgoingClipId == outgoing && t.incomingClipId == incoming;
  });
  return finishSceneChange(p, std::move(edited), error);
}
void StudioHistory::reset(const StudioEditState &state) {
  states_ = {state};
  index_ = 0;
}
void StudioHistory::push(const StudioEditState &state) {
  if (current().project == state.project) {
    states_[index_] = state;
    return;
  }
  states_.resize(index_ + 1);
  states_.push_back(state);
  ++index_;
}
const StudioEditState &StudioHistory::current() const {
  return states_[index_];
}
bool StudioHistory::canUndo() const { return index_ > 0; }
bool StudioHistory::canRedo() const { return index_ + 1 < states_.size(); }
bool StudioHistory::undo() {
  if (!canUndo())
    return false;
  --index_;
  return true;
}
bool StudioHistory::redo() {
  if (!canRedo())
    return false;
  ++index_;
  return true;
}
void StudioHistory::setCursor(quint64 clip, quint64 cue, qint64 in, qint64 out,
                              qint64 position) {
  auto &s = states_[index_];
  s.selectedClip = clip;
  s.selectedCue = cue;
  s.rangeIn = in;
  s.rangeOut = out;
  s.positionMs = position;
}
