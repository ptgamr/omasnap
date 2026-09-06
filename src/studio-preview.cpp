/** @fileoverview The Studio preview surface (see studio-preview.hpp). */
#include "studio-preview.hpp"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QtConcurrentRun>
#include <utility>

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
  connect(
      &frameWatcher_, &QFutureWatcher<StudioVideoFrame>::finished, this,
      [this] {
        StudioVideoFrame frame = frameWatcher_.result();
        preparing_ = false;
        if (preparingGeneration_ == generation_ && !frame.size.isEmpty()) {
          videoSize_ = rotation_ % 180 ? frame.size.transposed() : frame.size;
          positionMs_ = frame.positionMs;
          if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
            const QImage image(
                reinterpret_cast<const uchar *>(frame.planes[0].constData()),
                frame.size.width(), frame.size.height(),
                static_cast<qsizetype>(frame.textures[0].width()) * 4,
                QImage::Format_RGBA8888);
            setFrame(image.copy());
            preparePendingFrame();
            return;
          }
          if (!surface_) {
            surface_ = new StudioVideoSurface(this);
            surface_->setGeometry(rect());
            surface_->overlay = [this](QPainter &painter) {
              paintOverlay(painter);
            };
            surface_->failed = [this](const QString &error) {
              emit previewFailed(error);
            };
            surface_->show();
          }
          surface_->setFrame(std::move(frame));
          refreshSurface();
        }
        preparePendingFrame();
      });
}

QSize StudioPreview::sizeHint() const { return {960, 540}; }

void StudioPreview::setVideoFrame(const QVideoFrame &frame) {
  if (!frame.isValid())
    return;
  pendingFrame_ = frame;
  preparePendingFrame();
}

void StudioPreview::invalidatePendingFrames() {
  ++generation_;
  pendingFrame_ = {};
}

void StudioPreview::preparePendingFrame() {
  if (preparing_ || !pendingFrame_.isValid())
    return;
  QVideoFrame frame = std::exchange(pendingFrame_, {});
  preparingGeneration_ = generation_;
  preparing_ = true;
  const bool offscreen =
      QGuiApplication::platformName() == QStringLiteral("offscreen");
  frameWatcher_.setFuture(QtConcurrent::run([frame, offscreen] {
    return prepareStudioVideoFrame(frame, offscreen);
  }));
}

void StudioPreview::setCanvasInset(int inset) {
  canvasInset_ = qMax(0, inset);
  refreshSurface();
}

void StudioPreview::setStyle(const StudioStyle &style) {
  style_ = style;
  refreshSurface();
}

void StudioPreview::refreshSurface() {
  if (surface_) {
    surface_->drawn = frameRect();
    surface_->canvas = canvasRect();
    surface_->background = style_.color();
    surface_->radius = style_.radius * canvasRect().height() / 1080.0;
    surface_->source =
        track_ ? zoomSourceRect(*track_, positionMs_) : QRectF(0, 0, 1, 1);
    surface_->rotation = rotation_;
    surface_->update();
  }
  update();
}

void StudioPreview::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  if (surface_)
    surface_->setGeometry(rect());
  refreshSurface();
}

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
  refreshSurface();
}

void StudioPreview::setPickable(bool pickable) {
  if (pickable_ == pickable)
    return;
  pickable_ = pickable;
  setCursor(pickable ? Qt::CrossCursor : Qt::ArrowCursor);
  refreshSurface();
}

void StudioPreview::setTrack(const ZoomTrack *track) {
  track_ = track;
  refreshSurface();
}

void StudioPreview::setPosition(qint64 milliseconds) {
  if (positionMs_ == milliseconds)
    return;
  positionMs_ = milliseconds;
  refreshSurface();
}

void StudioPreview::setTargetMarker(bool shown, const QPointF &target) {
  if (markerShown_ == shown && marker_ == target)
    return;
  markerShown_ = shown;
  marker_ = target;
  refreshSurface();
}

QRectF StudioPreview::canvasRect() const {
  const QSize size = surface_ ? videoSize_ : frame_.size();
  if (size.isEmpty())
    return {};
  // The frame keeps its aspect and is centred; the zoom happens inside it,
  // so the window never changes shape as a cue ramps.
  const qreal aspect = static_cast<qreal>(size.width()) / size.height();
  qreal drawWidth = qMax(1, width() - 2 * canvasInset_);
  qreal drawHeight = drawWidth / aspect;
  if (drawHeight > height() - 2 * canvasInset_) {
    drawHeight = qMax(1, height() - 2 * canvasInset_);
    drawWidth = drawHeight * aspect;
  }
  return {(width() - drawWidth) / 2.0, (height() - drawHeight) / 2.0, drawWidth,
          drawHeight};
}

QRectF StudioPreview::frameRect() const {
  const QRectF canvas = canvasRect();
  return canvas.adjusted(canvas.width() * style_.padding / 100.0,
                         canvas.height() * style_.padding / 100.0,
                         -canvas.width() * style_.padding / 100.0,
                         -canvas.height() * style_.padding / 100.0);
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
  const bool wasInside = frameRect().contains(hover_);
  hover_ = event->position();
  if (wasInside != frameRect().contains(hover_))
    refreshSurface();
}

void StudioPreview::leaveEvent(QEvent *event) {
  QWidget::leaveEvent(event);
  hover_ = {-1, -1};
  refreshSurface();
}

void StudioPreview::paintEvent(QPaintEvent *) {
  if (surface_)
    return;
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
  painter.fillRect(canvasRect(), style_.color());
  painter.save();
  if (style_.radius > 0) {
    QPainterPath clip;
    const qreal radius = style_.radius * canvasRect().height() / 1080;
    clip.addRoundedRect(drawn, radius, radius);
    painter.setClipPath(clip);
  }
  painter.drawImage(drawn, frame_, source);
  painter.restore();
  paintOverlay(painter);
}

void StudioPreview::paintOverlay(QPainter &painter) const {
  const QRectF drawn = frameRect();
  const QRectF window =
      track_ ? zoomSourceRect(*track_, positionMs_) : QRectF(0, 0, 1, 1);
  painter.setRenderHint(QPainter::Antialiasing);
  if (markerShown_) {
    // Where the selected cue is aimed, in the visible window's coordinates,
    // so it sits on the thing it targets as the camera moves.
    const QPointF spot(drawn.left() + (marker_.x() - window.x()) /
                                          window.width() * drawn.width(),
                       drawn.top() + (marker_.y() - window.y()) /
                                         window.height() * drawn.height());
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
