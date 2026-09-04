/** @fileoverview The Studio preview surface (see studio-preview.hpp). */
#include "studio-preview.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QTransform>

namespace {

const QColor kLetterbox(10, 10, 12);
const QColor kMarker(120, 170, 255);
const QColor kMarkerRing(255, 255, 255, 200);
const QColor kHint(255, 255, 255, 120);

constexpr qreal kMarkerRadius = 7.0;

} // namespace

StudioPreview::StudioPreview(QWidget *parent) : QWidget(parent) {
  setMouseTracking(true);
  setCursor(Qt::CrossCursor);
  setMinimumSize(320, 180);
}

QSize StudioPreview::sizeHint() const { return {960, 540}; }

void StudioPreview::setFrame(const QImage &frame) {
  // QVideoFrame::toImage() hands back the coded picture and drops the
  // display rotation, while ffmpeg applies it before the zoom filter. Undo
  // that difference here, or a portrait phone recording previews sideways
  // and every normalized target addresses the wrong axis.
  frame_ = rotation_ == 0 ? frame
                          : frame.transformed(QTransform().rotate(rotation_),
                                              Qt::SmoothTransformation);
  update();
}

void StudioPreview::setRotation(int degrees) {
  const int normalized = ((degrees % 360) + 360) % 360;
  if (rotation_ == normalized)
    return;
  rotation_ = normalized;
  update();
}

void StudioPreview::setPickable(bool pickable) {
  if (pickable_ == pickable)
    return;
  pickable_ = pickable;
  setCursor(pickable ? Qt::CrossCursor : Qt::ArrowCursor);
  update();
}

void StudioPreview::setTrack(const ZoomTrack *track) {
  track_ = track;
  update();
}

void StudioPreview::setPosition(qint64 milliseconds) {
  if (positionMs_ == milliseconds)
    return;
  positionMs_ = milliseconds;
  update();
}

void StudioPreview::setTargetMarker(bool shown, const QPointF &target) {
  markerShown_ = shown;
  marker_ = target;
  update();
}

QRectF StudioPreview::frameRect() const {
  if (frame_.isNull() || frame_.height() <= 0)
    return {};
  // The frame keeps its aspect and is centred; the zoom happens inside it,
  // so the window never changes shape as a cue ramps.
  const qreal aspect = static_cast<qreal>(frame_.width()) / frame_.height();
  qreal drawWidth = width();
  qreal drawHeight = drawWidth / aspect;
  if (drawHeight > height()) {
    drawHeight = height();
    drawWidth = drawHeight * aspect;
  }
  return {(width() - drawWidth) / 2.0, (height() - drawHeight) / 2.0,
          drawWidth, drawHeight};
}

std::optional<QPointF> StudioPreview::sourceAt(const QPointF &position) const {
  const QRectF drawn = frameRect();
  if (drawn.isEmpty() || !drawn.contains(position))
    return std::nullopt;
  const QPointF inside((position.x() - drawn.left()) / drawn.width(),
                       (position.y() - drawn.top()) / drawn.height());
  // Through the visible window, not straight to the frame: while zoomed in,
  // the pointer is over a small part of the source and that is the part the
  // user means.
  const QRectF window =
      track_ ? zoomSourceRect(*track_, positionMs_) : QRectF(0, 0, 1, 1);
  return QPointF(window.x() + inside.x() * window.width(),
                 window.y() + inside.y() * window.height());
}

void StudioPreview::mousePressEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton || !pickable_)
    return;
  if (const std::optional<QPointF> target = sourceAt(event->position()))
    emit targetPicked(*target);
}

void StudioPreview::mouseMoveEvent(QMouseEvent *event) {
  hover_ = event->position();
  update();
}

void StudioPreview::leaveEvent(QEvent *event) {
  QWidget::leaveEvent(event);
  hover_ = {-1, -1};
  update();
}

void StudioPreview::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.fillRect(rect(), kLetterbox);
  const QRectF drawn = frameRect();
  if (drawn.isEmpty())
    return;

  const QRectF window =
      track_ ? zoomSourceRect(*track_, positionMs_) : QRectF(0, 0, 1, 1);
  const QRectF source(window.x() * frame_.width(), window.y() * frame_.height(),
                      window.width() * frame_.width(),
                      window.height() * frame_.height());
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage(drawn, frame_, source);

  if (markerShown_) {
    // Where the selected cue is aimed, in the visible window's coordinates,
    // so it sits on the thing it targets as the camera moves.
    const QPointF spot(
        drawn.left() + (marker_.x() - window.x()) / window.width() *
                           drawn.width(),
        drawn.top() + (marker_.y() - window.y()) / window.height() *
                          drawn.height());
    if (drawn.contains(spot)) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(kMarkerRing, 2));
      painter.drawEllipse(spot, kMarkerRadius, kMarkerRadius);
      painter.setPen(Qt::NoPen);
      painter.setBrush(kMarker);
      painter.drawEllipse(spot, 3.0, 3.0);
    }
  }

  if (pickable_ && drawn.contains(hover_)) {
    painter.setPen(kHint);
    painter.setFont(font());
    painter.drawText(drawn.adjusted(0, 0, -10, -8),
                     Qt::AlignRight | Qt::AlignBottom,
                     QStringLiteral("click to aim the zoom"));
  }
}
