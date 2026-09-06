/** @fileoverview Declares the Studio window: what a finished recording opens
 *  into. Playback, a scrubbable timeline, a trim range, and an export. */
#pragma once

#include "studio-style.hpp"
#include "zoom-track.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QSize>
#include <QString>
#include <QWidget>

class QAudioOutput;
class QMediaPlayer;
class QProcess;
class QVideoSink;
class StudioPreview;

/** What the export needs to know about the file it is reading. */
struct StudioSource {
  /** Display size: already swapped when the file carries a 90/270 rotation,
   *  because that is the shape both the preview and the export produce. */
  QSize size;
  /** Frame rate as the ratio the file states, never rounded: 30000/1001 read
   *  as 30 drifts the camera against the picture. */
  int fpsNumerator = 0;
  int fpsDenominator = 1;
  /** Clockwise display rotation to apply, 0/90/180/270. ffmpeg applies the
   *  container's rotation before the zoom filter, so the preview has to
   *  apply it too or the two disagree about which way is up -- and a
   *  normalized target then means a different point on each. */
  int rotation = 0;
  /** Set when the file asks for something the zoom cannot reproduce
   *  faithfully: a rotation that is not a right angle, where ffmpeg takes a
   *  general rotation path the preview's transpose would not match. */
  bool unsupportedTransform = false;
  [[nodiscard]] bool usable() const {
    return size.isValid() && !size.isEmpty() && fpsNumerator > 0 &&
           fpsDenominator > 0 && !unsupportedTransform;
  }
};

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
  /** Borrowed; the window owns the track and outlives this widget. */
  void setTrack(const ZoomTrack *track);
  void setSelectedCue(quint64 id);
  /** Whether cues can be moved or resized; off while an export runs. */
  void setCuesEditable(bool editable);
  void setThumbnails(QVector<QImage> thumbnails);
  [[nodiscard]] quint64 selectedCue() const { return selected_; }
  [[nodiscard]] qint64 duration() const { return duration_; }
  [[nodiscard]] qint64 trimIn() const { return trimIn_; }
  [[nodiscard]] qint64 trimOut() const { return trimOut_; }
  [[nodiscard]] QSize sizeHint() const override;

signals:
  void editStarted();
  void editFinished();
  void scrubbed(qint64 milliseconds);
  void trimChanged(qint64 inPoint, qint64 outPoint);
  /** A cue was clicked, or 0 when the click landed on empty lane. */
  void cueSelected(quint64 id);
  /** A cue was dragged or resized to a new span. */
  void cueMoved(quint64 id, qint64 startMs, qint64 endMs);

protected:
  void leaveEvent(QEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  enum class Grab { None, In, Out, Playhead, CueBody, CueStart, CueEnd };

  /** The trim bar's row. */
  [[nodiscard]] QRectF trackRect() const;
  /** The zoom cues' row, under it. */
  [[nodiscard]] QRectF cueLaneRect() const;
  [[nodiscard]] QRectF cueRect(const ZoomCue &cue) const;
  [[nodiscard]] qreal xForTime(qint64 milliseconds) const;
  [[nodiscard]] qint64 timeForX(qreal x) const;
  [[nodiscard]] Grab grabAt(const QPointF &position) const;
  /** The cue under `position`, or 0. `edge` reports which end was hit. */
  [[nodiscard]] quint64 cueAt(const QPointF &position, Grab *edge) const;

  const ZoomTrack *track_ = nullptr;
  qint64 duration_ = 0;
  qint64 position_ = 0;
  qint64 trimIn_ = 0;
  qint64 trimOut_ = 0;
  quint64 selected_ = 0;
  quint64 grabbedCue_ = 0;
  /// Where in the cue the drag started, so moving one does not snap its
  /// start to the pointer.
  qint64 grabOffsetMs_ = 0;
  Grab grabbed_ = Grab::None;
  Grab hovered_ = Grab::None;
  bool cuesEditable_ = true;
  QVector<QImage> thumbnails_;
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
  ~StudioWindow() override;

  /** False when the file could not be opened at all. */
  [[nodiscard]] bool hasMedia() const;

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void closeEvent(QCloseEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  bool handleShortcut(QKeyEvent *event, bool activate);
  void toggleInspector();
  void showShortcuts();
  void seekTo(qint64 milliseconds);
  void stepFrame(int direction);
  void beginEdit();
  void endEdit();
  void rememberEdit();
  void undoEdit();
  void redoEdit();
  void restoreEdit();
  void setSelectedZoomTiming(bool easeIn, int milliseconds);
  void styleChanged();
  void togglePlayback();
  void seekBy(qint64 milliseconds);
  void setTrimIn();
  void setTrimOut();
  void resetTrim();
  void startExport();
  void setStatus(const QString &status);
  void refreshControls();
  /** Clicking the preview aims the cue under the playhead, or makes one. */
  void aimZoom(const QPointF &target);
  void addZoomAtPlayhead();
  void removeSelectedZoom();
  void setSelectedZoomScale(qreal scale);
  /** The selected cue the inspector controls act on. */
  [[nodiscard]] const ZoomCue *activeCue() const;
  void zoomChanged();
  [[nodiscard]] QString zoomSidecarPath() const;
  void saveZoom();

  QString path_;
  QMediaPlayer *player_ = nullptr;
  QAudioOutput *audio_ = nullptr;
  QVideoSink *sink_ = nullptr;
  StudioPreview *preview_ = nullptr;
  StudioTimeline *timeline_ = nullptr;
  QProcess *export_ = nullptr;
  ZoomTrack zoom_;
  StudioStyle style_;
  StudioSource media_;
  class QLabel *statusLabel_ = nullptr;
  class QLabel *timeLabel_ = nullptr;
  class QPushButton *playButton_ = nullptr;
  class QPushButton *exportButton_ = nullptr;
  class QPushButton *resetButton_ = nullptr;
  class QPushButton *addZoomButton_ = nullptr;
  class QPushButton *removeZoomButton_ = nullptr;
  class QSlider *zoomSlider_ = nullptr;
  class QLabel *zoomLabel_ = nullptr;
  class QTimer *saveTimer_ = nullptr;
  class QPushButton *undoButton_ = nullptr;
  class QPushButton *redoButton_ = nullptr;
  class QLabel *sourceLabel_ = nullptr;
  class QLabel *fileLabel_ = nullptr;
  class QLabel *shortcutLegend_ = nullptr;
  class QLabel *cueLabel_ = nullptr;
  class QSpinBox *easeIn_ = nullptr;
  class QSpinBox *easeOut_ = nullptr;
  class QWidget *inspector_ = nullptr;
  bool inspectorWanted_ = true;
  class QComboBox *background_ = nullptr;
  class QSlider *padding_ = nullptr;
  class QSlider *radius_ = nullptr;
  class QTimer *scrubTimer_ = nullptr;
  qint64 pendingSeek_ = -1;
  struct EditState {
    ZoomTrack zoom;
    qint64 in = 0;
    qint64 out = 0;
    StudioStyle style;
    quint64 selection = 0;
    bool operator==(const EditState &other) const {
      return zoom == other.zoom && in == other.in && out == other.out &&
             style == other.style;
    }
  };
  QVector<EditState> edits_;
  qsizetype editIndex_ = -1;
  bool editGesture_ = false;
  bool restoring_ = false;
  bool loaded_ = false;
  bool closing_ = false;
  qint64 initialTrimIn_ = 0;
  qint64 initialTrimOut_ = -1;
  struct LoadedSource {
    StudioSource media;
    ZoomTrack zoom;
    QString error;
    qint64 in = 0;
    qint64 out = -1;
    StudioStyle style;
  };
  QFutureWatcher<LoadedSource> loadWatcher_;
  QFutureWatcher<QString> saveWatcher_;
  QFutureWatcher<QVector<QImage>> thumbnailWatcher_;
  bool thumbnailsStarted_ = false;
  bool saving_ = false;
  bool savePending_ = false;
  bool mediaFailed_ = false;
  /// Whether a first frame has been coaxed out of the player, so opening
  /// the window shows the recording rather than an empty rectangle.
  bool primed_ = false;
};

/** `hh:mm:ss.mmm` for ffmpeg, and `m:ss` for people. */
[[nodiscard]] QString studioTimecode(qint64 milliseconds);
[[nodiscard]] QString studioClock(qint64 milliseconds);
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
                                                qint64 inPoint, qint64 outPoint,
                                                const ZoomTrack &zoom = {},
                                                const StudioSource &media = {},
                                                const StudioStyle &style = {});
/** Reads the size and frame rate the export needs. Blocking; bounded. */
[[nodiscard]] StudioSource probeStudioSource(const QString &path);
/** `<stem>-trim.mp4` beside the source, under a name nothing has taken. */
[[nodiscard]] QString studioExportPath(const QString &source);
