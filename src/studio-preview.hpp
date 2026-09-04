/** @fileoverview Declares the Studio's preview surface: the decoded frame,
 *  drawn through the zoom model rather than straight to the screen. */
#pragma once

#include "zoom-track.hpp"

#include <QImage>
#include <QWidget>

#include <optional>

/**
 * Paints the part of the current frame the zoom model says is on camera.
 *
 * Not a QVideoWidget: that draws the whole frame and gives nowhere to apply
 * the zoom, and the point of this window is that what you see here is what
 * the export produces. It takes frames from the player's video sink and
 * draws `zoomSourceRect()` of them, which is the same rectangle the export's
 * ffmpeg expressions describe.
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
  /** Borrowed; the window owns the track and outlives this widget. */
  void setTrack(const ZoomTrack *track);
  void setPosition(qint64 milliseconds);
  /** Draws a marker at `target` (normalized) while a cue is selected. */
  void setTargetMarker(bool shown, const QPointF &target = {});
  [[nodiscard]] QSize sizeHint() const override;

signals:
  /** A point on the source frame, normalized, that the user aimed at. */
  void targetPicked(const QPointF &target);

protected:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  /** Where the frame is drawn inside the widget, keeping its aspect. */
  [[nodiscard]] QRectF frameRect() const;
  /** Widget point to a normalized point on the source frame, or nothing
   *  when the click missed the frame. */
  [[nodiscard]] std::optional<QPointF> sourceAt(const QPointF &position) const;

  QImage frame_;
  const ZoomTrack *track_ = nullptr;
  qint64 positionMs_ = 0;
  QPointF marker_;
  QPointF hover_{-1, -1};
  bool markerShown_ = false;
};
