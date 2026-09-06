#include "studio-project.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
QVector<StudioSpan> studioComposition(const StudioProject &p) {
  QVector<StudioSpan> spans;
  qint64 position = 0;
  for (const auto &clip : p.clips) {
    const qint64 duration = clipDuration(clip);
    if (duration <= 0 || duration > maxDuration - position)
      return {};
    spans.push_back({clip.id, clip.assetId, position, position + duration,
                     clip.inMs, clip.outMs, clip.speed});
    position += duration;
  }
  return spans;
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
  QJsonArray assets, clips;
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
  return QJsonDocument(
             QJsonObject{
                 {"schema", StudioProject::kSchema},
                 {"assets", assets},
                 {"clips", clips},
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
      !root["canvas"].isObject() || !root["style"].isObject() ||
      !root["zoom"].isObject() || !integer(root["trimInMs"], 0, maxDuration) ||
      !integer(root["trimOutMs"], -1, maxDuration))
    return malformed;
  const auto assets = root["assets"].toArray(), clips = root["clips"].toArray();
  if (assets.size() > maxItems || clips.size() > maxItems)
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
bool studioSplitClip(StudioProject &p, qint64 at, quint64 newId) {
  if (!validateStudioProject(p).isEmpty() || !unusedClipId(p, newId) ||
      p.clips.size() >= maxItems)
    return false;
  const auto spans = studioComposition(p);
  for (qsizetype i = 0; i < spans.size(); ++i) {
    if (at <= spans[i].startMs || at >= spans[i].endMs)
      continue;
    const auto split =
        splitAt(p.clips[i], at - spans[i].startMs, frameMilliseconds(p));
    if (!split)
      return false;
    p.clips[i] = split->left;
    auto right = split->right;
    right.id = newId;
    p.clips.insert(i + 1, right);
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
  return {true, *start, *end, removed};
}
StudioCutResult studioDeleteClip(StudioProject &p, quint64 id) {
  for (const auto &span : studioComposition(p))
    if (span.clipId == id)
      return studioDeleteRange(p, span.startMs, span.endMs, 0);
  return {};
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
