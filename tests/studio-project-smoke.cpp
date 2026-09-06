#include "studio-project-smoke.hpp"
#include "studio-project.hpp"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <limits>

namespace {
bool cutChecks(QString &error) {
  const auto check = [&](bool ok, const QString &message) {
    if (!ok)
      error = message;
    return ok;
  };
  StudioProject original;
  StudioSource source;
  source.size = {1280, 720};
  source.fpsNumerator = 30;
  source.durationMs = 10000;
  original.assets = {{1, QStringLiteral("untouched-source.mp4"), source}};
  original.clips = {{11, 1, 0, 10000, 1}};
  original.zoom.cues = {{1, 100, 600, 100, 100, {0.2, 0.3}, 2},
                        {2, 1500, 2500, 200, 200, {0.4, 0.5}, 3},
                        {3, 3500, 5000, 300, 300, {0.6, 0.7}, 2}};
  original.trimInMs = 500;
  original.trimOutMs = 9000;
  auto edited = original;
  const auto cut = studioDeleteRange(edited, 3000, 1000, 99);
  if (!check(cut.changed && cut.fromMs == 1000 && cut.toMs == 3000 &&
                 cut.removedMs == 2000 && studioDuration(edited) == 8000 &&
                 edited.clips.size() == 2 &&
                 edited.clips[0] == StudioClip{11, 1, 0, 1000, 1} &&
                 edited.clips[1] == StudioClip{99, 1, 3000, 10000, 1} &&
                 edited.assets == original.assets,
             QStringLiteral("Interior reverse range cut did not preserve "
                            "source/ranges/identity")))
    return false;
  if (!check(edited.zoom.cues.size() == 2 &&
                 edited.zoom.cues[0] == original.zoom.cues[0] &&
                 edited.zoom.cues[1].id == 3 &&
                 edited.zoom.cues[1].startMs == 1500 &&
                 edited.zoom.cues[1].endMs == 3000 &&
                 edited.zoom.cues[1].target == original.zoom.cues[2].target &&
                 edited.trimInMs == 500 && edited.trimOutMs == 7000,
             QStringLiteral(
                 "Ripple cut lost or mistimed attached zoom/review range")))
    return false;
  for (qint64 time = 0; time < studioDuration(edited); time += 17) {
    const auto expected =
        studioFrameAt(original, time < 1000 ? time : time + 2000);
    const auto actual = studioFrameAt(edited, time);
    if (!check(expected && actual && expected->sourceMs == actual->sourceMs,
               QStringLiteral("Ripple cut retained removed source time")))
      return false;
  }
  const auto unchanged = edited;
  if (!check(!studioDeleteRange(edited, 500, 500, 100).changed &&
                 edited == unchanged &&
                 !studioDeleteRange(edited, 200, 400, 99).changed &&
                 edited == unchanged && !studioSplitClip(edited, 0, 100) &&
                 !studioSplitClip(edited, 1000, 100) && edited == unchanged,
             QStringLiteral("No-op/duplicate identity changed the project")))
    return false;
  const auto second = studioDeleteRange(edited, 1500, 2000, 100);
  if (!check(second.changed && studioDuration(edited) == 7500 &&
                 edited.clips.size() == 3,
             QStringLiteral("Repeated interior cut failed")))
    return false;
  StudioProject decoded;
  if (!check(
          decodeStudioProject(encodeStudioProject(edited), decoded).isEmpty() &&
              decoded == edited,
          QStringLiteral("Cut project did not round-trip")))
    return false;

  auto cross = original;
  cross.zoom.cues.clear();
  cross.clips = {
      {11, 1, 0, 3000, 1}, {12, 1, 0, 4000, 1}, {13, 1, 6000, 9000, 1}};
  const auto crossCut = studioDeleteRange(cross, 2500, 7500, 0);
  if (!check(
          crossCut.changed && crossCut.removedMs == 5000 &&
              cross.clips.size() == 2 &&
              cross.clips[0] == StudioClip{11, 1, 0, 2500, 1} &&
              cross.clips[1] == StudioClip{13, 1, 6500, 9000, 1},
          QStringLiteral(
              "Cross-scene range cut lost surviving identities/source bounds")))
    return false;
  if (!check(studioDeleteClip(cross, 11).changed && cross.clips.size() == 1 &&
                 cross.clips[0].id == 13 &&
                 !studioDeleteClip(cross, 999).changed,
             QStringLiteral("Explicit scene deletion removed wrong scene")))
    return false;
  auto bridge = original;
  bridge.zoom.cues = {{55, 500, 4000, 400, 400, {0.3, 0.4}, 2}};
  bridge.trimInMs = 1200;
  bridge.trimOutMs = 1800;
  if (!check(studioDeleteRange(bridge, 1000, 3000, 99).changed &&
                 bridge.zoom.cues.size() == 1 && bridge.zoom.cues[0].id == 55 &&
                 bridge.zoom.cues[0].startMs == 500 &&
                 bridge.zoom.cues[0].endMs == 2000 && bridge.trimInMs == 0 &&
                 bridge.trimOutMs == -1,
             QStringLiteral(
                 "Cut-spanning zoom or collapsed review range mapping failed")))
    return false;
  auto edges = original;
  edges.zoom.cues.clear();
  if (!check(studioDeleteRange(edges, -100, 1000, 0).changed &&
                 edges.clips[0].inMs == 1000 &&
                 studioDeleteRange(edges, 8000, 20000, 0).changed &&
                 edges.clips[0].outMs == 9000,
             QStringLiteral("Clamped edge cuts failed")))
    return false;

  StudioHistory history;
  StudioEditState before;
  before.project = original;
  before.selectedClip = 11;
  before.selectedCue = 2;
  before.rangeIn = 0;
  before.rangeOut = 10000;
  before.positionMs = 1500;
  history.reset(before);
  auto after = before;
  const auto all = studioDeleteClip(after.project, 11);
  after.positionMs =
      studioTimeAfterDelete(after.positionMs, all.fromMs, all.toMs);
  after.rangeIn = after.rangeOut = -1;
  after.selectedClip = after.selectedCue = 0;
  history.push(after);
  if (!check(all.changed && after.project.clips.isEmpty() &&
                 after.project.zoom.cues.isEmpty() &&
                 validateStudioProject(after.project).isEmpty() &&
                 history.undo() && history.current() == before &&
                 history.redo() && history.current() == after,
             QStringLiteral("Final scene deletion is not exactly undoable")))
    return false;

  for (const double speed : {0.125, 0.5, 1.0, 2.0, 3.0, 8.0}) {
    auto sped = original;
    sped.zoom.cues.clear();
    sped.trimInMs = 0;
    sped.trimOutMs = -1;
    sped.clips[0].speed = speed;
    const qint64 duration = studioDuration(sped);
    auto split = sped;
    if (!check(studioSplitClip(split, duration / 3, 99) &&
                   studioDuration(split) == duration &&
                   split.clips[0].outMs == split.clips[1].inMs &&
                   split.clips[1].id == 99 && split.assets == sped.assets,
               QStringLiteral("Speed %1 split changed duration/source")
                   .arg(speed)))
      return false;
    const auto fastCut =
        studioDeleteRange(sped, duration / 3, duration * 2 / 3, 99);
    if (!check(fastCut.changed &&
                   duration - studioDuration(sped) == fastCut.removedMs &&
                   fastCut.toMs - fastCut.fromMs == fastCut.removedMs &&
                   qAbs(fastCut.fromMs - duration / 3) <= 34 &&
                   qAbs(fastCut.toMs - duration * 2 / 3) <= 34 &&
                   validateStudioProject(sped).isEmpty(),
               QStringLiteral("Speed %1 cut drift or invalid effective bounds")
                   .arg(speed)))
      return false;
  }
  return true;
}
bool sceneChecks(QString &error) {
  const auto check = [&](bool ok, const QString &message) {
    if (!ok)
      error = message;
    return ok;
  };
  StudioProject p;
  p.canvas = {1280, 720};
  p.fpsNumerator = 30000;
  p.fpsDenominator = 1001;
  StudioSource first;
  first.size = {1280, 720};
  first.fpsNumerator = 24;
  first.durationMs = 10000;
  first.audioStreams = 1;
  auto portrait = first;
  portrait.size = {720, 1280};
  portrait.fpsNumerator = 60;
  portrait.audioStreams = 0;
  QString operationError;
  if (!check(
          studioInsertScenes(p,
                             {{1, QStringLiteral("first.mp4"), first},
                              {2, QStringLiteral("portrait.mp4"), portrait}},
                             {{11, 1, 0, 1000, 1}, {12, 2, 1000, 3000, 1}}, 0,
                             operationError) &&
              p.clips.size() == 2 && p.assets.size() == 2 &&
              studioDuration(p) == 3000 && p.canvas == QSize(1280, 720) &&
              p.fpsNumerator == 30000 && p.fpsDenominator == 1001,
          QStringLiteral(
              "Mixed-scene import changed canonical canvas/rate or failed: %1")
              .arg(operationError)))
    return false;
  p.zoom.cues = {{1, 100, 500, 100, 100, {0.2, 0.3}, 2},
                 {2, 1300, 1800, 150, 150, {0.6, 0.7}, 3}};
  p.trimInMs = 200;
  p.trimOutMs = 2500;
  const auto original = p;
  if (!check(!studioMoveClip(p, 11, 12, operationError) &&
                 operationError.isEmpty() && p == original &&
                 !studioTrimClip(p, 11, 0, 1000, operationError) &&
                 operationError.isEmpty() && p == original,
             QStringLiteral(
                 "No-op scene edit fragmented zooms or reset review range")))
    return false;
  if (!check(studioMoveClip(p, 12, 11, operationError) && p.clips[0].id == 12 &&
                 p.zoom.cues[0].id == 2 && p.zoom.cues[0].startMs == 300 &&
                 p.zoom.cues[1].id == 1 && p.zoom.cues[1].startMs == 2100 &&
                 p.trimInMs == 0 && p.trimOutMs == -1,
             QStringLiteral(
                 "Scene reorder did not carry zooms/reset review range")))
    return false;
  if (!check(
          studioMoveClip(p, 12, 0, operationError) &&
              p.clips == original.clips && p.zoom == original.zoom,
          QStringLiteral(
              "Scene reorder return changed attached zoom identities/timing")))
    return false;
  if (!check(studioDuplicateClip(p, 11, 77, operationError) &&
                 p.clips[1].id == 77 && p.clips[1].assetId == 1 &&
                 p.assets == original.assets && p.zoom.cues.size() == 3 &&
                 p.zoom.cues[0].id == 1 && p.zoom.cues[1].id != 1 &&
                 p.zoom.cues[1].id != 2 && p.zoom.cues[1].startMs == 1100 &&
                 p.zoom.cues[2].id == 2 && p.zoom.cues[2].startMs == 2300,
             QStringLiteral(
                 "Duplicate did not create independent zooms/source instance")))
    return false;
  const quint64 duplicateCue = p.zoom.cues[1].id;
  if (!check(
          studioTrimClip(p, 77, 200, 900, operationError) &&
              p.clips[0].inMs == 0 && p.clips[0].outMs == 1000 &&
              p.clips[1].inMs == 200 && p.clips[1].outMs == 900 &&
              p.zoom.cues[1].id == duplicateCue &&
              p.zoom.cues[1].startMs == 1000 && p.zoom.cues[1].endMs == 1300 &&
              p.zoom.cues[2].startMs == 2000 && studioDuration(p) == 3700,
          QStringLiteral(
              "Trimming duplicate changed original or misplaced scene zooms")))
    return false;
  const auto beforeInvalid = p;
  if (!check(!studioTrimClip(p, 77, 900, 200, operationError) &&
                 !operationError.isEmpty() && p == beforeInvalid &&
                 !studioMoveClip(p, 77, 999, operationError) &&
                 !operationError.isEmpty() && p == beforeInvalid &&
                 !studioDuplicateClip(p, 77, 11, operationError) &&
                 !operationError.isEmpty() && p == beforeInvalid &&
                 !studioInsertScenes(
                     p, {{1, QStringLiteral("collision.mp4"), first}},
                     {{99, 1, 0, 500, 1}}, 0, operationError) &&
                 !operationError.isEmpty() && p == beforeInvalid,
             QStringLiteral("Invalid scene change partially mutated project")))
    return false;
  if (!check(
          studioInsertScenes(p, {}, {{99, 2, 0, 500, 1}}, 1, operationError) &&
              p.clips[1].id == 99 && p.zoom.cues[1].startMs == 1500 &&
              p.assets == original.assets,
          QStringLiteral(
              "Insertion before a scene lost original zoom/source timing")))
    return false;
  StudioHistory history;
  StudioEditState before;
  before.project = original;
  before.selectedClip = 12;
  before.positionMs = 1500;
  history.reset(before);
  auto after = before;
  after.project = p;
  after.selectedClip = 77;
  history.push(after);
  if (!check(history.undo() && history.current() == before && history.redo() &&
                 history.current() == after,
             QStringLiteral("Scene edits are not exactly undoable")))
    return false;
  StudioProject decoded;
  if (!check(decodeStudioProject(encodeStudioProject(p), decoded).isEmpty() &&
                 decoded == p,
             QStringLiteral("Combined scenes failed save/reopen")))
    return false;

  auto crossing = original;
  crossing.zoom.cues = {{55, 500, 1500, 200, 200, {0.3, 0.4}, 2}};
  if (!check(studioInsertScenes(crossing, {}, {{99, 1, 5000, 5500, 1}}, 2,
                                operationError) &&
                 crossing.zoom ==
                     ZoomTrack{{{55, 500, 1500, 200, 200, {0.3, 0.4}, 2}}},
             QStringLiteral("Append fragmented an unchanged cross-scene zoom")))
    return false;
  if (!check(studioMoveClip(crossing, 12, 11, operationError) &&
                 crossing.zoom.cues.size() == 2 &&
                 crossing.zoom.cues[0].id != 55 &&
                 crossing.zoom.cues[0].startMs == 0 &&
                 crossing.zoom.cues[0].endMs == 500 &&
                 crossing.zoom.cues[1].id == 55 &&
                 crossing.zoom.cues[1].startMs == 2500 &&
                 crossing.zoom.cues[1].endMs == 3000,
             QStringLiteral("Reordered cross-scene zoom did not preserve first "
                            "original fragment identity")))
    return false;
  auto capped = original;
  capped.clips = {{11, 1, 0, 10000, 1}};
  capped.zoom.cues.clear();
  capped.trimInMs = 0;
  capped.trimOutMs = -1;
  for (int i = 0; i < kMaxZoomCues; ++i)
    capped.zoom.cues.push_back({static_cast<quint64>(i + 1),
                                i * 250,
                                i * 250 + 200,
                                60,
                                60,
                                {0.5, 0.5},
                                2});
  const auto exactCap = capped;
  if (!check(!studioDuplicateClip(capped, 11, 77, operationError) &&
                 operationError.contains(QStringLiteral("zoom-cue limit")) &&
                 capped == exactCap,
             QStringLiteral("Zoom fragment overflow silently discarded edits")))
    return false;
  return true;
}
} // namespace

bool runStudioProjectChecks(QString &error) {
  const auto check = [&](bool ok, const QString &message) {
    if (!ok)
      error = message;
    return ok;
  };
  StudioProject p;
  StudioSource source;
  source.size = {1280, 720};
  source.fpsNumerator = 30000;
  source.fpsDenominator = 1001;
  source.durationMs = 10000;
  source.audioStreams = 2;
  p.assets = {{1, QStringLiteral("missing-recording.mp4"), source}};
  p.clips = {{11, 1, 1000, 5000, 1}, {12, 1, 2000, 6000, 2}};
  if (!check(validateStudioProject(p).isEmpty(),
             QStringLiteral("Valid project rejected")) ||
      !check(studioDuration(p) == 6000,
             QStringLiteral("Composition duration drift")))
    return false;
  const auto before = studioFrameAt(p, 3999), boundary = studioFrameAt(p, 4000),
             fast = studioFrameAt(p, 4500);
  if (!check(before && before->span.clipId == 11 && before->sourceMs == 4999 &&
                 boundary && boundary->span.clipId == 12 &&
                 boundary->sourceMs == 2000 && fast && fast->sourceMs == 3000,
             QStringLiteral("Source-time mapping differs at cut/speed")) ||
      !check(!studioFrameAt(p, -1) && !studioFrameAt(p, 6000),
             QStringLiteral("Out-of-project time resolves")) ||
      !check(studioTimelineTime(p, 11, 3000) == 2000 &&
                 studioTimelineTime(p, 12, 3000) == 4500 &&
                 !studioTimelineTime(p, 12, 6000),
             QStringLiteral("Repeated-source inverse mapping ambiguous")))
    return false;
  StudioProject split = p;
  split.clips = {
      {11, 1, 1000, 3000, 1}, {13, 1, 3000, 5000, 1}, p.clips.back()};
  for (qint64 time = 0; time < 6000; time += 17) {
    if (!check(studioFrameAt(p, time)->sourceMs ==
                   studioFrameAt(split, time)->sourceMs,
               QStringLiteral("Splitting changes content time")))
      return false;
  }
  StudioHistory history;
  StudioEditState initial;
  initial.project = p;
  history.reset(initial);
  history.setCursor(11, 0, 1200, 1700, 1350);
  const auto beforeEdit = history.current();
  auto afterEdit = beforeEdit;
  afterEdit.project = split;
  afterEdit.rangeIn = afterEdit.rangeOut = -1;
  afterEdit.selectedClip = 13;
  history.push(afterEdit);
  if (!check(history.undo() && history.current() == beforeEdit &&
                 history.redo() && history.current() == afterEdit,
             QStringLiteral(
                 "Undo/redo did not restore project and cursor exactly")))
    return false;
  history.undo();
  auto branch = beforeEdit;
  branch.project.style.padding = 10;
  history.push(branch);
  if (!check(!history.canRedo(),
             QStringLiteral("New edit retains obsolete redo branch")))
    return false;
  history.push(branch);
  if (!check(history.undo() && !history.canUndo(),
             QStringLiteral("No-op edit adds history")))
    return false;

  p.zoom.cues = {{7, 1000, 2000, 200, 200, {0.3, 0.7}, 2}};
  p.style = {2, 10, 20};
  p.trimInMs = 500;
  p.trimOutMs = 5500;
  StudioProject decoded;
  if (!check(decodeStudioProject(encodeStudioProject(p), decoded).isEmpty() &&
                 decoded == p,
             QStringLiteral("Project serialization not lossless")))
    return false;
  StudioProject empty;
  if (!check(
          decodeStudioProject(encodeStudioProject(empty), decoded).isEmpty() &&
              decoded == empty,
          QStringLiteral("Empty project does not persist")))
    return false;
  const auto preserved = decoded;
  auto json = QJsonDocument::fromJson(encodeStudioProject(p)).object();
  json["schema"] = 999;
  if (!check(!decodeStudioProject(QJsonDocument(json).toJson(), decoded)
                     .isEmpty() &&
                 decoded == preserved,
             QStringLiteral("Invalid schema mutates project")))
    return false;
  json = QJsonDocument::fromJson(encodeStudioProject(p)).object();
  auto clips = json["clips"].toArray();
  auto invalidClip = clips[0].toObject();
  invalidClip["inMs"] = QStringLiteral("1000");
  clips[0] = invalidClip;
  json["clips"] = clips;
  if (!check(
          !decodeStudioProject(QJsonDocument(json).toJson(), decoded).isEmpty(),
          QStringLiteral("Mistyped range accepted")))
    return false;
  auto invalid = p;
  invalid.clips[1].id = invalid.clips[0].id;
  if (!check(!validateStudioProject(invalid).isEmpty(),
             QStringLiteral("Duplicate scene IDs accepted")))
    return false;
  invalid = p;
  invalid.clips[0].speed = std::numeric_limits<double>::quiet_NaN();
  if (!check(!validateStudioProject(invalid).isEmpty() &&
                 studioComposition(invalid).isEmpty(),
             QStringLiteral("Non-finite speed accepted")))
    return false;
  invalid = p;
  invalid.clips[0].assetId = 999;
  if (!check(!validateStudioProject(invalid).isEmpty(),
             QStringLiteral("Unknown asset reference accepted")))
    return false;
  if (!check(!decodeStudioProject(QByteArray(4 * 1024 * 1024 + 1, ' '), decoded)
                  .isEmpty(),
             QStringLiteral("Unbounded project accepted")))
    return false;

  QTemporaryDir dir;
  if (!check(dir.isValid(), QStringLiteral("No temporary project folder")))
    return false;
  const QString file = dir.filePath(QStringLiteral("edit.omasnap-project"));
  p.assets[0].path = dir.filePath(QStringLiteral("unavailable.mp4"));
  if (!check(saveStudioProject(file, p).isEmpty(),
             QStringLiteral("Atomic project save failed")))
    return false;
  const auto loaded = loadStudioProject(file);
  const auto permissions = QFileInfo(file).permissions();
  if (!check(
          (permissions & (QFileDevice::ReadOwner | QFileDevice::WriteOwner)) ==
                  (QFileDevice::ReadOwner | QFileDevice::WriteOwner) &&
              !(permissions &
                (QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                 QFileDevice::ReadOther | QFileDevice::WriteOther |
                 QFileDevice::ExeOwner | QFileDevice::ExeGroup |
                 QFileDevice::ExeOther)),
          QStringLiteral("Project permissions expose private source paths")))
    return false;
  const QString symlink =
      dir.filePath(QStringLiteral("linked.omasnap-project"));
  if (!check(QFile::link(file, symlink) && QFileInfo(symlink).isSymLink() &&
                 !saveStudioProject(symlink, empty).isEmpty() &&
                 QFileInfo(symlink).isSymLink() &&
                 loadStudioProject(file).project == p,
             QStringLiteral("Project save followed or replaced a symlink")))
    return false;
  if (!check(
          loaded.error.isEmpty() && loaded.project == p &&
              loaded.missingAssets == QVector<quint64>{1},
          QStringLiteral("Missing media erased project or was not reported")))
    return false;
  if (!check(!saveStudioProject(file, invalid).isEmpty() &&
                 loadStudioProject(file).project == p,
             QStringLiteral("Invalid save replaced good project")))
    return false;
  if (!check(saveStudioProject(file, empty).isEmpty() &&
                 loadStudioProject(file).project == empty,
             QStringLiteral("Saving empty project failed")))
    return false;
  return cutChecks(error) && sceneChecks(error);
}
