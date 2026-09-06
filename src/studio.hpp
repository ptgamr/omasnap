/** @fileoverview Declares the Studio window: what a finished recording opens
 *  into. Playback, a scrubbable timeline, a trim range, and an export. */
#pragma once

#include "studio-project.hpp"
#include "studio-style.hpp"
#include "studio-theme.hpp"
#include "zoom-track.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QSet>
#include <QSize>
#include <QString>
#include <QWidget>

class QAudioOutput;
class QMediaPlayer;
class QProcess;
class QVideoSink;
class StudioPreview;
class StudioPlayback;

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
  void setProject(const StudioProject *project) {
    project_ = project;
    update();
  }
  void setRangeMode(bool enabled);
  void setRange(qint64 start, qint64 end);
  void setSelectedClip(quint64 id);
  void clearSelection();
  [[nodiscard]] bool rangeMode() const { return rangeMode_; }
  [[nodiscard]] qint64 rangeIn() const { return rangeIn_; }
  [[nodiscard]] qint64 rangeOut() const { return rangeOut_; }
  [[nodiscard]] quint64 selectedClip() const { return selectedClip_; }
  /** Drop location in the ordered scene list; zero means append. */
  [[nodiscard]] quint64 insertionBefore(qreal x) const;
  void showInsertion(quint64 before, bool visible);
  void setChrome(const StudioChrome &chrome) {
    chrome_ = chrome;
    update();
  }
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
  void selectionChanged();
  void splitRequested();
  void deleteRequested();
  void sceneMoveRequested(quint64 id, quint64 before);
  void sceneTrimRequested(quint64 id, qint64 inMs, qint64 outMs);

protected:
  void leaveEvent(QEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;

private:
  enum class Grab {
    None,
    In,
    Out,
    Playhead,
    CueBody,
    CueStart,
    CueEnd,
    RangeStart,
    RangeEnd,
    RangeNew,
    SceneStart,
    SceneEnd,
    SceneMove
  };

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
  const StudioProject *project_ = nullptr;
  bool rangeMode_ = false;
  qint64 rangeIn_ = -1;
  qint64 rangeOut_ = -1;
  qint64 rangeAnchor_ = 0;
  quint64 selectedClip_ = 0;
  QPointF scenePress_;
  StudioClip grabbedScene_;
  qint64 sceneStartMs_ = 0;
  qint64 sceneDurationMs_ = 0;
  bool insertionVisible_ = false;
  quint64 insertionBefore_ = 0;
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
  StudioChrome chrome_;
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
  explicit StudioWindow(QString path, QWidget *parent = nullptr,
                        QString themePath = {});
  ~StudioWindow() override;

  /** False when the file could not be opened at all. */
  [[nodiscard]] bool hasMedia() const;

protected:
  bool eventFilter(QObject *object, QEvent *event) override;
  void closeEvent(QCloseEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dragMoveEvent(QDragMoveEvent *event) override;
  void dragLeaveEvent(QDragLeaveEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  void applyChrome();
  StudioTheme *theme_ = nullptr;
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
  void setStatus(const QString &status, bool error = false);
  void refreshControls();
  /** Clicking the preview aims the cue under the playhead, or makes one. */
  void aimZoom(const QPointF &target);
  void addZoomAtPlayhead();
  void removeSelectedZoom();
  void setSelectedZoomScale(qreal scale);
  /** The selected cue the inspector controls act on. */
  [[nodiscard]] const ZoomCue *activeCue() const;
  void zoomChanged();
  void saveProject();
  void applyProject(bool resetHistory = false, qint64 position = -1);
  void relinkAsset();
  void refreshThumbnails();
  void captureCursor();
  void splitAtPlayhead();
  void deleteSelection();
  void finishCompositionEdit(qint64 position);
  void setupScenes(class QVBoxLayout *controls);
  void chooseScenes();
  void importScenes(const QStringList &paths, quint64 before = 0);
  void moveScene(quint64 id, quint64 before);
  void duplicateScene();
  void trimScene(quint64 id, qint64 inMs, qint64 outMs);
  void refreshSceneControls();
  [[nodiscard]] bool scenesEditable() const;
  bool importing_ = false;
  quint64 nextAssetId_ = 1;
  StudioProject gestureProject_;
  QFutureWatcher<StudioProjectLoad> importWatcher_;
  class QPushButton *importButton_ = nullptr;
  class QPushButton *duplicateButton_ = nullptr;
  class QPushButton *earlierButton_ = nullptr;
  class QPushButton *laterButton_ = nullptr;
  class QLabel *sceneLabel_ = nullptr;
  class QSpinBox *sceneIn_ = nullptr;
  class QSpinBox *sceneOut_ = nullptr;
  quint64 nextClipId_ = 1;
  class QPushButton *rangeButton_ = nullptr;
  class QPushButton *selectButton_ = nullptr;
  class QPushButton *splitButton_ = nullptr;
  class QPushButton *deleteButton_ = nullptr;
  [[nodiscard]] StudioEditState editState() const;

  QString path_;
  StudioPlayback *player_ = nullptr;
  QAudioOutput *audio_ = nullptr;
  StudioPreview *preview_ = nullptr;
  StudioTimeline *timeline_ = nullptr;
  bool export_ = false;
  struct ExportResult {
    QString destination;
    QString error;
  };
  QFutureWatcher<ExportResult> exportWatcher_;
  ZoomTrack zoom_;
  StudioStyle style_;
  StudioSource media_;
  StudioProject project_;
  StudioHistory history_;
  QString projectPath_;
  QVector<quint64> missingAssets_;
  QSet<QString> missingPaths_;
  bool relinking_ = false;
  class QPushButton *relinkButton_ = nullptr;
  QFutureWatcher<StudioProjectLoad> relinkWatcher_;
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
  StudioComboBox *background_ = nullptr;
  class QSlider *padding_ = nullptr;
  class QSlider *radius_ = nullptr;
  class QTimer *scrubTimer_ = nullptr;
  qint64 pendingSeek_ = -1;
  bool editGesture_ = false;
  bool restoring_ = false;
  bool loaded_ = false;
  bool closing_ = false;
  struct LoadedSource {
    StudioProject project;
    QVector<quint64> missingAssets;
    QString error;
  };
  QFutureWatcher<LoadedSource> loadWatcher_;
  QFutureWatcher<QString> saveWatcher_;
  QFutureWatcher<QVector<QImage>> thumbnailWatcher_;
  StudioProject thumbnailProject_;
  bool thumbnailPending_ = false;
  bool thumbnailsStarted_ = false;
  bool saving_ = false;
  bool savePending_ = false;
  bool mediaFailed_ = false;
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
