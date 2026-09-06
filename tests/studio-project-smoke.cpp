#include "studio-project-smoke.hpp"
#include "studio-project.hpp"
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <limits>

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
  return true;
}
