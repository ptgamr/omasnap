/** @fileoverview Declares the Studio window: what a finished recording opens
 *  into. Playback, a scrubbable timeline, a trim range, and an export. */
#pragma once

#include "zoom-track.hpp"

#include <QSize>
#include <QString>
#include <QWidget>

class QAudioOutput;
class QMediaPlayer;
class QProcess;
class QVideoWidget;

/**
 * The trim band under the video: the whole recording as one bar, the kept
 * range lit, the playhead on top. Dragging the handles trims, dragging
 * anywhere else scrubs -- there is no separate scrollbar, because at this
 * size a second one would be a smaller target for the same job.
 */
class StudioTimeline final : public QWidget {
  Q_OBJECT
public:
  explicit StudioTimeline(QWidget *parent = nullptr);

  void setDuration(qint64 milliseconds);
  void setPosition(qint64 milliseconds);
  void setTrim(qint64 inPoint, qint64 outPoint);
  [[nodiscard]] qint64 duration() const { return duration_; }
  [[nodiscard]] qint64 trimIn() const { return trimIn_; }
  [[nodiscard]] qint64 trimOut() const { return trimOut_; }
  [[nodiscard]] QSize sizeHint() const override;

signals:
  void scrubbed(qint64 milliseconds);
  void trimChanged(qint64 inPoint, qint64 outPoint);

protected:
  void leaveEvent(QEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  enum class Grab { None, In, Out, Playhead };

  [[nodiscard]] QRectF trackRect() const;
  [[nodiscard]] qreal xForTime(qint64 milliseconds) const;
  [[nodiscard]] qint64 timeForX(qreal x) const;
  [[nodiscard]] Grab grabAt(const QPointF &position) const;

  qint64 duration_ = 0;
  qint64 position_ = 0;
  qint64 trimIn_ = 0;
  qint64 trimOut_ = 0;
  Grab grabbed_ = Grab::None;
  Grab hovered_ = Grab::None;
};

/**
 * A recording, open. Deliberately a normal `xdg_toplevel` window in its own
 * executable: the screenshot binary sets layer-shell process-wide and links
 * no media libraries, and neither of those should change because video
 * exists.
 */
class StudioWindow final : public QWidget {
  Q_OBJECT
public:
  explicit StudioWindow(QString path, QWidget *parent = nullptr);

  /** False when the file could not be opened at all. */
  [[nodiscard]] bool hasMedia() const;

protected:
  void keyPressEvent(QKeyEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  void togglePlayback();
  void seekBy(qint64 milliseconds);
  void setTrimIn();
  void setTrimOut();
  void resetTrim();
  void startExport();
  void setStatus(const QString &status);
  void refreshControls();

  QString path_;
  QMediaPlayer *player_ = nullptr;
  QAudioOutput *audio_ = nullptr;
  QVideoWidget *video_ = nullptr;
  StudioTimeline *timeline_ = nullptr;
  QProcess *export_ = nullptr;
  class QLabel *statusLabel_ = nullptr;
  class QLabel *timeLabel_ = nullptr;
  class QPushButton *playButton_ = nullptr;
  class QPushButton *exportButton_ = nullptr;
  class QPushButton *resetButton_ = nullptr;
  bool mediaFailed_ = false;
};

/** `hh:mm:ss.mmm` for ffmpeg, and `m:ss` for people. */
[[nodiscard]] QString studioTimecode(qint64 milliseconds);
[[nodiscard]] QString studioClock(qint64 milliseconds);
/** What the export needs to know about the file it is reading. */
struct StudioSource {
  QSize size;
  int fps = 0;
};

/**
 * The ffmpeg argument vector that writes `[inPoint, outPoint)` of `source`
 * to `destination`, with `zoom` applied.
 *
 * The zoom comes from the same `ZoomTrack` the preview draws, turned into
 * `zoompan` expressions -- there is no second description of where the
 * camera is. Cue times are absolute in the source, so the offset of the trim
 * is handed to the expressions rather than papered over with `setpts`:
 * zoompan reads its own frame counter, not the timestamp.
 *
 * The zoom is skipped, rather than guessed at, when `media` does not carry a
 * usable size and frame rate: a wrong frame rate slides every cue.
 */
[[nodiscard]] QStringList studioExportArguments(const QString &source,
                                                const QString &destination,
                                                qint64 inPoint,
                                                qint64 outPoint,
                                                const ZoomTrack &zoom = {},
                                                const StudioSource &media = {});
/** Reads the size and frame rate the export needs. Blocking; bounded. */
[[nodiscard]] StudioSource probeStudioSource(const QString &path);
/** `<stem>-trim.mp4` beside the source, under a name nothing has taken. */
[[nodiscard]] QString studioExportPath(const QString &source);
