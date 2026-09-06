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
struct StudioProject {
  static constexpr int kSchema = 1;
  QVector<StudioAsset> assets;
  QVector<StudioClip> clips;
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
