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
  /** Audio-only sources (songs, voiceovers): no picture, but a countable
   *  duration and at least one audio stream. */
  [[nodiscard]] bool usableAudio() const {
    return durationMs > 0 && audioStreams > 0;
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
enum class StudioTransitionKind {
  Crossfade,
  FadeBlack,
  WipeLeft,
  WipeRight,
  WipeUp,
  WipeDown,
  SlideLeft,
  SlideRight,
  SlideUp,
  SlideDown
};
/** One scene's presentation in normalized canonical-canvas coordinates.
 * clip is half-open [x,x+width) × [y,y+height), after applying offset. */
struct StudioTransitionLayer {
  double opacity = 1;
  QPointF offset;
  QRectF clip{0, 0, 1, 1};
};
[[nodiscard]] StudioTransitionLayer
studioTransitionLayer(StudioTransitionKind kind, double progress,
                      bool incoming);
/** Stable serialized name; empty for an invalid enum value. */
[[nodiscard]] QString studioTransitionName(StudioTransitionKind kind);
struct StudioTransition {
  quint64 outgoingClipId = 0;
  quint64 incomingClipId = 0;
  StudioTransitionKind kind = StudioTransitionKind::Crossfade;
  qint64 durationMs = 0;
  bool operator==(const StudioTransition &) const = default;
};
/**
 * Optional background music: one audio file under the whole composition,
 * from composition zero, trimmed to fit. Empty path means silence.
 * Volume is percent; durationMs is probed at import, never on the GUI
 * thread.
 */
struct StudioMusic {
  QString path;
  int volume = 20;
  qint64 durationMs = 0;
  bool operator==(const StudioMusic &) const = default;
};
/**
 * One sound on the audio lane: an independent end-to-end sequence on the
 * shared composition clock, arranged like video scenes but never shifted
 * by video edits (documented, like music). Gain is percent.
 */
struct StudioAudioClip {
  quint64 id = 0;
  quint64 assetId = 0;
  qint64 inMs = 0;
  qint64 outMs = 0;
  double speed = 1.0;
  int gain = 100;
  bool operator==(const StudioAudioClip &) const = default;
};
/** One laid-out sound: half-open [startMs, endMs) on the composition. */
struct StudioAudioSpan {
  quint64 clipId = 0;
  quint64 assetId = 0;
  qint64 startMs = 0;
  qint64 endMs = 0;
  qint64 inMs = 0;
  qint64 outMs = 0;
  double speed = 1.0;
  int gain = 100;
};
struct StudioProject {
  static constexpr int kSchema = 2;
  QVector<StudioAsset> assets;
  QVector<StudioClip> clips;
  QVector<StudioTransition> transitions;
  QVector<StudioAudioClip> audioClips;
  ZoomTrack zoom; // Edited composition time, never source-file time.
  StudioStyle style;
  StudioMusic music;
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
/** The audio lane laid end to end from composition zero, in clip order. */
[[nodiscard]] QVector<StudioAudioSpan> studioAudioComposition(
    const StudioProject &);
/** Min/max waveform buckets for lane painting, from mono 16-bit PCM. */
[[nodiscard]] QVector<QPair<qint16, qint16>> studioBucketAudioPeaks(
    const QByteArray &pcm, int buckets);
/**
 * Decodes `path` to low-rate mono and buckets it for the lane. Empty on
 * missing files, audioless inputs, absurd sizes, or timeouts. Blocking;
 * bounded; worker-only, never inline in a GUI callback.
 */
[[nodiscard]] QVector<QPair<qint16, qint16>> studioDecodeAudioPeaks(
    const QString &path, int buckets = 1024);
/** The canvas after the style aspect expands it: Original returns the
 *  source size, anything else grows one side to the ratio (even, never
 *  cropping). Preview, playback sizing, and export all use this. */
[[nodiscard]] QSize studioEffectiveCanvas(const StudioProject &);
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
/** Appends sounds to the lane end. The lane carries no transitions or
 *  review range, so this validates and swaps directly. */
[[nodiscard]] bool studioAppendAudioClips(
    StudioProject &, const QVector<StudioAsset> &newAssets,
    const QVector<StudioAudioClip> &newClips, QString &error);
/** beforeClipId == 0 appends; otherwise move immediately before that scene. */
[[nodiscard]] bool studioMoveClip(StudioProject &, quint64 clipId,
                                  quint64 beforeClipId, QString &error);
[[nodiscard]] bool studioDuplicateClip(StudioProject &, quint64 clipId,
                                       quint64 newClipId, QString &error);
[[nodiscard]] bool studioTrimClip(StudioProject &, quint64 clipId,
                                   qint64 sourceInMs, qint64 sourceOutMs,
                                   QString &error);
/**
 * Audio-lane structural edits. The lane carries no transitions, zooms, or
 * review range, so these validate and swap directly instead of routing
 * through the scene change machinery. False with empty error is an exact
 * no-op; beforeClipId 0 appends.
 */
[[nodiscard]] bool studioMoveAudioClip(StudioProject &, quint64 clipId,
                                       quint64 beforeClipId, QString &error);
[[nodiscard]] bool studioDuplicateAudioClip(StudioProject &, quint64 clipId,
                                            quint64 newClipId, QString &error);
[[nodiscard]] bool studioTrimAudioClip(StudioProject &, quint64 clipId,
                                       qint64 sourceInMs, qint64 sourceOutMs,
                                       QString &error);
[[nodiscard]] bool studioSplitAudioClip(StudioProject &, qint64 atMs,
                                        quint64 newClipId, QString &error);
[[nodiscard]] bool studioDeleteAudioClip(StudioProject &, quint64 clipId,
                                         QString &error);
/** Scalar retunes: gain is percent, speed follows the scene 0.125..8
 *  range. Unchanged values are exact no-ops. */
[[nodiscard]] bool studioSetAudioClipGain(StudioProject &, quint64 clipId,
                                          int gain, QString &error);
[[nodiscard]] bool studioSetAudioClipSpeed(StudioProject &, quint64 clipId,
                                           double speed, QString &error);
/** Retimes one scene: duration becomes (out-in)/speed with source-anchored
 *  zoom remapping via finishSceneChange. Speeds outside 0.125..8 or
 *  non-finite values are rejected; unchanged speeds are a no-op. */
[[nodiscard]] bool studioSetClipSpeed(StudioProject &, quint64 clipId,
                                      double speed, QString &error);

struct StudioEditState {
  StudioProject project;
  quint64 selectedClip = 0;
  quint64 selectedAudioClip = 0;
  quint64 selectedTransition = 0;
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
  void setCursor(quint64 selectedClip, quint64 selectedAudioClip,
                   quint64 selectedTransition, quint64 selectedCue,
                   qint64 rangeIn, qint64 rangeOut, qint64 positionMs);

private:
  QVector<StudioEditState> states_{StudioEditState{}};
  qsizetype index_ = 0;
};
