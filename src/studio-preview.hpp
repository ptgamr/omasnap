/** @fileoverview Declares the Studio's preview surface: the decoded frame,
 *  drawn through the zoom model rather than straight to the screen. */
#pragma once

#include "studio-style.hpp"
#include "studio-theme.hpp"
#include "studio-video.hpp"
#include "zoom-track.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QWidget>

#include <optional>

/**
 * Paints the part of the current frame the zoom model says is on camera.
 *
 * Each of two decoder slots prepares frames with one in-flight worker and one
 * newest pending frame, then uploads planes to a GPU surface. The shader
 * applies color conversion, rotation, zoomSourceRect(), and canvas styling.
 * Static images and the offscreen test platform use the matching QPainter path.
 *
 * Clicking picks a point on the *source*, not on the visible window, so
 * clicking while already zoomed in aims at the thing under the pointer
 * rather than somewhere else.
 */
class StudioPreview final : public QWidget {
  Q_OBJECT
public:
  explicit StudioPreview(QWidget *parent = nullptr);

  void setFrame(const QImage &frame);
  void setVideoFrame(const QVideoFrame &frame);
  /** Timestamp is composition time, independent of source-file PTS. */
  void setVideoFrame(const QVideoFrame &frame, qint64 timelineMs);
  /** Physical decoder slots retain independently prepared frames. */
  void setVideoFrame(int slot, const QVideoFrame &frame, int rotation);
  void clearVideoSlot(int slot);
  [[nodiscard]] bool videoSlotReady(int slot) const;
  void setComposition(int primary, int secondary, double primaryOpacity,
                      double secondaryOpacity, qint64 timelineMs);
  void setCanvasSize(const QSize &size);
  void clearFrame();
  void invalidatePendingFrames();
  void setCanvasInset(int inset);
  void setStyle(const StudioStyle &style);
  void setChrome(const StudioChrome &chrome);
  /** Borrowed; the window owns the track and outlives this widget. */
  void setTrack(const ZoomTrack *track);
  void setPosition(qint64 milliseconds);
  /** Draws a marker at `target` (normalized) while a cue is selected. */
  void setTargetMarker(bool shown, const QPointF &target = {});
  /** Whether clicking aims a zoom. Off while exporting, and off when the
   *  export could not read the source and so could apply no zoom at all. */
  void setPickable(bool pickable);
  /**
   * Clockwise rotation to apply to incoming frames, in degrees.
   *
   * ffmpeg rotates a phone recording before the zoom filter sees it, and
   * QVideoFrame::toImage() hands back the coded picture with that dropped,
   * so a preview that skipped this would show a different picture from the
   * export and a click would mean a different point. The caller converts the
   * container's counter-clockwise display angle into this clockwise one.
   */
  void setRotation(int degrees);
  [[nodiscard]] QSize sizeHint() const override;

signals:
  void videoFrameReady(int slot);
  void previewFailed(const QString &error);
  /** A point on the source frame, normalized, that the user aimed at. */
  void targetPicked(const QPointF &target);

protected:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

private:
  /** Where the frame is drawn inside the widget, keeping its aspect. */
  [[nodiscard]] QRectF frameRect() const;
  [[nodiscard]] QRectF canvasRect() const;
  /** Widget point to a normalized point on the source frame, or nothing
   *  when the click missed the frame. */
  [[nodiscard]] std::optional<QPointF> sourceAt(const QPointF &position) const;
  void preparePendingFrame(int slot);
  void refreshSurface();
  void paintOverlay(QPainter &painter) const;

  QImage frame_;
  QSize videoSize_;
  StudioVideoSurface *surface_ = nullptr;
  struct Preparation {
    QFutureWatcher<StudioVideoFrame> watcher;
    QVideoFrame pending;
    std::optional<qint64> pendingPosition;
    QSize size;
    QImage image;
    int rotation = 0;
    quint64 generation = 0;
    quint64 preparingGeneration = 0;
    bool preparing = false;
    bool ready = false;
  };
  std::array<Preparation, 2> preparations_;
  QSize canvasSize_;
  int primary_ = 0;
  int secondary_ = -1;
  double primaryOpacity_ = 1;
  double secondaryOpacity_ = 0;
  bool composition_ = false;
  int canvasInset_ = 0;
  StudioStyle style_;
  StudioChrome chrome_;
  const ZoomTrack *track_ = nullptr;
  qint64 positionMs_ = 0;
  QPointF marker_;
  QPointF hover_{-1, -1};
  int rotation_ = 0;
  bool markerShown_ = false;
  bool pickable_ = true;
};
