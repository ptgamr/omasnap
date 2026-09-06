/** @fileoverview Non-destructive Studio document and its shared time map. */
#pragma once

#include "studio-style.hpp"
#include "zoom-track.hpp"
#include <QByteArray>
#include <QSize>
#include <QStringList>
#include <optional>

struct StudioSource {
  QSize size;
  int fpsNumerator = 0;
  int fpsDenominator = 1;
  int rotation = 0;
  bool unsupportedTransform = false;
  qint64 durationMs = 0;
  int audioStreams = 0;
  [[nodiscard]] bool usable() const {
    return size.isValid() && !size.isEmpty() && fpsNumerator > 0 &&
           fpsDenominator > 0 && !unsupportedTransform;
  }
  bool operator==(const StudioSource &) const = default;
};

struct StudioAsset {
  quint64 id = 0;
  QString path;
  StudioSource source;
  bool operator==(const StudioAsset &) const = default;
};
struct StudioClip {
  quint64 id = 0;
  quint64 assetId = 0;
  qint64 inMs = 0;
  qint64 outMs = 0;
  double speed = 1.0;
  bool operator==(const StudioClip &) const = default;
};
enum class StudioTransitionKind { Crossfade, FadeBlack };
struct StudioTransition {
  quint64 outgoingClipId = 0;
  quint64 incomingClipId = 0;
  StudioTransitionKind kind = StudioTransitionKind::Crossfade;
  qint64 durationMs = 0;
  bool operator==(const StudioTransition &) const = default;
};
struct StudioProject {
  static constexpr int kSchema = 2;
  QVector<StudioAsset> assets;
  QVector<StudioClip> clips;
  QVector<StudioTransition> transitions;
  ZoomTrack zoom; // Edited composition time, never source-file time.
  StudioStyle style;
  QSize canvas{1920, 1080};
  int fpsNumerator = 30;
  int fpsDenominator = 1;
  bool operator==(const StudioProject &) const = default;
  qint64 trimInMs = 0;
  qint64 trimOutMs = -1;
};
struct StudioSpan {
  quint64 clipId = 0;
  quint64 assetId = 0;
  qint64 startMs = 0;
  qint64 endMs = 0;
  qint64 inMs = 0;
  qint64 outMs = 0;
  double speed = 1.0;
};
struct StudioFrame {
  StudioSpan span;
  qint64 sourceMs = 0;
};
struct StudioBlend {
  StudioFrame outgoing;
  StudioFrame incoming;
  StudioTransitionKind kind = StudioTransitionKind::Crossfade;
  double progress = 0;
  double outgoingOpacity = 1;
  double incomingOpacity = 0;
  double outgoingAudioGain = 1;
  double incomingAudioGain = 0;
};
[[nodiscard]] const StudioTransition *studioTransition(const StudioProject &,
                                                       quint64 outgoingClipId,
                                                       quint64 incomingClipId);
[[nodiscard]] qint64 studioTransitionMaximum(const StudioProject &,
                                             quint64 outgoingClipId,
                                             quint64 incomingClipId);
[[nodiscard]] std::optional<StudioBlend> studioBlendAt(const StudioProject &,
                                                       qint64 timelineMs);
[[nodiscard]] bool studioSetTransition(StudioProject &, quint64 outgoingClipId,
                                       quint64 incomingClipId,
                                       StudioTransitionKind, qint64 durationMs,
                                       QString &error);
[[nodiscard]] bool studioRemoveTransition(StudioProject &,
                                          quint64 outgoingClipId,
                                          quint64 incomingClipId,
                                          QString &error);
[[nodiscard]] const StudioAsset *studioAsset(const StudioProject &, quint64 id);
[[nodiscard]] QVector<StudioSpan> studioComposition(const StudioProject &);
[[nodiscard]] qint64 studioDuration(const StudioProject &);
/** Half-open spans: an exact cut belongs to the incoming clip; project end
 * has no frame. Callers seeking the last frame clamp before calling. */
[[nodiscard]] std::optional<StudioFrame> studioFrameAt(const StudioProject &,
                                                       qint64 timelineMs);
[[nodiscard]] std::optional<qint64>
studioTimelineTime(const StudioProject &, quint64 clipId, qint64 sourceMs);
[[nodiscard]] QString validateStudioProject(const StudioProject &);
[[nodiscard]] QByteArray encodeStudioProject(const StudioProject &);
/** Pure decode: does not inspect paths or touch media. Failure leaves out
 * intact. */
[[nodiscard]] QString decodeStudioProject(const QByteArray &,
                                          StudioProject &out);
struct StudioProjectLoad {
  StudioProject project;
  QString error;
  QVector<quint64> missingAssets;
};
/** Disk entry points must be called on a worker, never a GUI callback. Missing
 * referenced files are reported separately and do not erase valid edit data. */
[[nodiscard]] StudioProjectLoad loadStudioProject(const QString &path);
[[nodiscard]] QString saveStudioProject(const QString &path,
                                        const StudioProject &project);

/** Clip identity comes from the window's monotonic allocator, outside undo
 * snapshots. A split never changes composition duration or zoom timing. */
[[nodiscard]] bool studioSplitClip(StudioProject &, qint64 atMs,
                                   quint64 newClipId, QString *error = nullptr);
struct StudioCutResult {
  bool changed = false;
  qint64 fromMs = 0;
  qint64 toMs = 0;
  qint64 removedMs = 0;
  QString error;
};
/** Removes a passage and closes the gap. Effective boundaries may snap by at
 * most one output frame to representable source milliseconds. Failure leaves
 * the project untouched. remainderClipId is used only when both sides of one
 * scene survive; it must then be nonzero and unused. */
[[nodiscard]] StudioCutResult studioDeleteRange(StudioProject &, qint64 fromMs,
                                                qint64 toMs,
                                                quint64 remainderClipId);
[[nodiscard]] StudioCutResult studioDeleteClip(StudioProject &, quint64 clipId);
[[nodiscard]] qint64 studioTimeAfterDelete(qint64 timeMs, qint64 fromMs,
                                           qint64 toMs);

/** Structural scene edits reset the review/export range to the whole project.
 * Zooms follow the retained source content of each scene; scene-bound fragments
 * shorter than kMinCueMs cannot form a cue. Exceeding the cue cap is an error,
 * not silent truncation. False with empty error means an exact no-op. */
[[nodiscard]] bool studioInsertScenes(StudioProject &,
                                      const QVector<StudioAsset> &newAssets,
                                      const QVector<StudioClip> &newClips,
                                      qsizetype insertionIndex, QString &error);
/** beforeClipId == 0 appends; otherwise move immediately before that scene. */
[[nodiscard]] bool studioMoveClip(StudioProject &, quint64 clipId,
                                  quint64 beforeClipId, QString &error);
[[nodiscard]] bool studioDuplicateClip(StudioProject &, quint64 clipId,
                                       quint64 newClipId, QString &error);
[[nodiscard]] bool studioTrimClip(StudioProject &, quint64 clipId,
                                  qint64 sourceInMs, qint64 sourceOutMs,
                                  QString &error);

struct StudioEditState {
  StudioProject project;
  quint64 selectedClip = 0;
  quint64 selectedCue = 0;
  qint64 rangeIn = -1;
  qint64 rangeOut = -1;
  qint64 positionMs = 0;
  bool operator==(const StudioEditState &) const = default;
};
/** Immutable value snapshots form one session history. Caller pushes only at
 * the end of a gesture, so a drag is one edit; selection/playhead travel with
 * it. */
class StudioHistory {
public:
  void reset(const StudioEditState &state);
  void push(const StudioEditState &state);
  [[nodiscard]] const StudioEditState &current() const;
  [[nodiscard]] bool canUndo() const;
  [[nodiscard]] bool canRedo() const;
  bool undo();
  bool redo();
  void setCursor(quint64 selectedClip, quint64 selectedCue, qint64 rangeIn,
                 qint64 rangeOut, qint64 positionMs);

private:
  QVector<StudioEditState> states_{StudioEditState{}};
  qsizetype index_ = 0;
};
