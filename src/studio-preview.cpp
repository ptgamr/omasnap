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

constexpr qreal kMarkerRadius = 7.0;

} // namespace

StudioPreview::StudioPreview(QWidget *parent) : QWidget(parent) {
  setMouseTracking(true);
  setCursor(Qt::CrossCursor);
  setMinimumSize(320, 180);
  for (int index = 0; index < 2; ++index)
    connect(
        &preparations_[index].watcher,
        &QFutureWatcher<StudioVideoFrame>::finished, this, [this, index] {
          auto &slot = preparations_[index];
          StudioVideoFrame frame = slot.watcher.result();
          slot.preparing = false;
          if (slot.preparingGeneration == slot.generation &&
              !frame.size.isEmpty()) {
            slot.size =
                slot.rotation % 180 ? frame.size.transposed() : frame.size;
            slot.ready = true;
            if (!composition_) {
              videoSize_ = slot.size;
              positionMs_ = frame.positionMs;
            }
            if (QGuiApplication::platformName() ==
                QStringLiteral("offscreen")) {
              const QImage image(
                  reinterpret_cast<const uchar *>(frame.planes[0].constData()),
                  frame.size.width(), frame.size.height(),
                  static_cast<qsizetype>(frame.textures[0].width()) * 4,
                  QImage::Format_RGBA8888);
              slot.image =
                  slot.rotation == 0
                      ? image.copy()
                      : image.transformed(QTransform().rotate(slot.rotation),
                                          Qt::SmoothTransformation);
              if (!composition_)
                frame_ = slot.image;
            } else {
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
              surface_->setFrame(index, std::move(frame));
            }
            refreshSurface();
            emit videoFrameReady(index);
          }
          preparePendingFrame(index);
        });
}

QSize StudioPreview::sizeHint() const { return {960, 540}; }

void StudioPreview::setVideoFrame(const QVideoFrame &frame) {
  if (!frame.isValid())
    return;
  composition_ = false;
  primary_ = 0;
  secondary_ = -1;
  primaryOpacity_ = 1;
  secondaryOpacity_ = 0;
  layers_ = {};
  auto &slot = preparations_[0];
  slot.pending = frame;
  slot.rotation = rotation_;
  slot.pendingPosition.reset();
  preparePendingFrame(0);
}

void StudioPreview::setVideoFrame(const QVideoFrame &frame, qint64 timelineMs) {
  if (!frame.isValid())
    return;
  setVideoFrame(frame);
  // Store the explicit clock with the submitted frame, even when preparation
  // was started synchronously by the legacy overload.
  positionMs_ = timelineMs;
  composition_ = true;
  refreshSurface();
}

void StudioPreview::setVideoFrame(int index, const QVideoFrame &frame,
                                  int rotation) {
  if (!frame.isValid())
    return;
  composition_ = true;
  auto &slot = preparations_[index];
  slot.pending = frame;
  slot.pendingPosition.reset();
  slot.rotation = rotation;
  preparePendingFrame(index);
}

void StudioPreview::setComposition(int primary, int secondary,
                                   StudioTransitionKind kind, double progress,
                                   qint64 timelineMs) {
  composition_ = true;
  primary_ = primary;
  secondary_ = secondary;
  layers_[primary] = secondary >= 0
                         ? studioTransitionLayer(kind, progress, false)
                         : StudioTransitionLayer{};
  if (secondary >= 0)
    layers_[secondary] = studioTransitionLayer(kind, progress, true);
  primaryOpacity_ = layers_[primary].opacity;
  secondaryOpacity_ = secondary >= 0 ? layers_[secondary].opacity : 0;
  positionMs_ = timelineMs;
  refreshSurface();
}

bool StudioPreview::videoSlotReady(int index) const {
  return preparations_[index].ready;
}

void StudioPreview::clearVideoSlot(int index) {
  auto &slot = preparations_[index];
  ++slot.generation;
  slot.pending = {};
  slot.pendingPosition.reset();
  slot.ready = false;
  slot.image = {};
  slot.size = {};
  if (surface_)
    surface_->setFrame(index, {});
  refreshSurface();
}

void StudioPreview::setCanvasSize(const QSize &size) {
  canvasSize_ = size;
  refreshSurface();
}

void StudioPreview::clearFrame() {
  for (int index = 0; index < 2; ++index)
    clearVideoSlot(index);
  frame_ = {};
  videoSize_ = {};
  refreshSurface();
}

void StudioPreview::invalidatePendingFrames(int index) {
  auto &slot = preparations_[index];
  ++slot.generation;
  slot.pending = {};
  slot.pendingPosition.reset();
}

void StudioPreview::preparePendingFrame(int index) {
  auto &slot = preparations_[index];
  if (slot.preparing || !slot.pending.isValid())
    return;
  QVideoFrame frame = std::exchange(slot.pending, {});
  const auto position = std::exchange(slot.pendingPosition, {});
  slot.preparingGeneration = slot.generation;
  slot.preparing = true;
  const bool offscreen =
      QGuiApplication::platformName() == QStringLiteral("offscreen");
  slot.watcher.setFuture(QtConcurrent::run([frame, offscreen, position] {
    auto prepared = prepareStudioVideoFrame(frame, offscreen);
    if (position)
      prepared.positionMs = *position;
    return prepared;
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

void StudioPreview::setChrome(const StudioChrome &chrome) {
  chrome_ = chrome;
  refreshSurface();
}

void StudioPreview::refreshSurface() {
  if (composition_)
    videoSize_ = preparations_[primary_].size;
  if (surface_) {
    surface_->primary = primary_;
    surface_->secondary = secondary_;
    surface_->primaryOpacity = primaryOpacity_;
    surface_->secondaryOpacity = secondaryOpacity_;
    surface_->drawn = frameRect();
    surface_->canvas = canvasRect();
    surface_->background = style_.color();
    surface_->workspace = chrome_.background;
    surface_->radius = style_.radius * canvasRect().height() / 1080.0;
    surface_->source =
        track_ ? zoomSourceRect(*track_, positionMs_) : QRectF(0, 0, 1, 1);
    for (int index = 0; index < 2; ++index) {
      surface_->offsets[index] = layers_[index].offset;
      surface_->clips[index] = layers_[index].clip;
      surface_->rotations[index] =
          composition_ ? preparations_[index].rotation : rotation_;
      QSizeF fitted = preparations_[index].size;
      if (canvasSize_.isValid() && !fitted.isEmpty())
        fitted.scale(QSizeF(canvasSize_), Qt::KeepAspectRatio);
      surface_->fits[index] =
          canvasSize_.isValid() && !fitted.isEmpty()
              ? QRectF((1 - fitted.width() / canvasSize_.width()) / 2,
                       (1 - fitted.height() / canvasSize_.height()) / 2,
                       fitted.width() / canvasSize_.width(),
                       fitted.height() / canvasSize_.height())
              : QRectF(0, 0, 1, 1);
    }
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
  composition_ = false;
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
  const QSize size = canvasSize_.isValid() ? canvasSize_
                     : surface_            ? videoSize_
                                           : frame_.size();
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
  painter.fillRect(rect(), chrome_.background);
  const QRectF drawn = frameRect();
  if (drawn.isEmpty())
    return;

  const QRectF window =
      track_ ? zoomSourceRect(*track_, positionMs_) : QRectF(0, 0, 1, 1);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.fillRect(canvasRect(), style_.color());
  painter.save();
  if (style_.radius > 0) {
    QPainterPath clip;
    const qreal radius = style_.radius * canvasRect().height() / 1080;
    clip.addRoundedRect(drawn, radius, radius);
    painter.setClipPath(clip);
  }
  painter.setClipRect(drawn, Qt::IntersectClip);
  painter.fillRect(drawn, Qt::black);
  if (composition_ &&
      (!videoSlotReady(primary_) || (secondary_ >= 0 && secondaryOpacity_ > 0 &&
                                     !videoSlotReady(secondary_)))) {
    painter.restore();
    return;
  }
  const auto draw = [&](const QImage &image,
                        const StudioTransitionLayer &layer) {
    if (image.isNull())
      return;
    painter.save();
    painter.setOpacity(layer.opacity);
    const auto projected = [&](const QRectF &canonical) {
      return QRectF(drawn.left() + (canonical.x() - window.x()) /
                                       window.width() * drawn.width(),
                    drawn.top() + (canonical.y() - window.y()) /
                                      window.height() * drawn.height(),
                    canonical.width() / window.width() * drawn.width(),
                    canonical.height() / window.height() * drawn.height());
    };
    painter.setClipRect(projected(layer.clip), Qt::IntersectClip);
    QRectF fit(0, 0, 1, 1);
    if (canvasSize_.isValid()) {
      // Match export: source is fitted into a black canonical canvas before
      // the global camera is evaluated. No full-size intermediate allocation.
      QSizeF fitted = image.size();
      fitted.scale(QSizeF(canvasSize_), Qt::KeepAspectRatio);
      fit = QRectF((1 - fitted.width() / canvasSize_.width()) / 2,
                   (1 - fitted.height() / canvasSize_.height()) / 2,
                   fitted.width() / canvasSize_.width(),
                   fitted.height() / canvasSize_.height());
    }
    painter.drawImage(projected(fit.translated(layer.offset)), image);
    painter.restore();
  };
  if (composition_) {
    draw(preparations_[primary_].image, layers_[primary_]);
    if (secondary_ >= 0) {
      painter.setCompositionMode(QPainter::CompositionMode_Plus);
      draw(preparations_[secondary_].image, layers_[secondary_]);
    }
  } else {
    draw(frame_, StudioTransitionLayer{});
  }
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
      painter.setPen(QPen(chrome_.foreground, 2));
      painter.drawEllipse(spot, kMarkerRadius, kMarkerRadius);
      painter.setPen(Qt::NoPen);
      painter.setBrush(chrome_.accent);
      painter.drawEllipse(spot, 3.0, 3.0);
    }
  }

  if (pickable_ && drawn.contains(hover_)) {
    QColor hintBackground = chrome_.background;
    hintBackground.setAlphaF(0.5);
    painter.fillRect(drawn.adjusted(0, drawn.height() - 26, 0, 0),
                     hintBackground);
    painter.setPen(chrome_.foreground);
    painter.setFont(font());
    painter.drawText(drawn.adjusted(0, 0, -10, -8),
                     Qt::AlignRight | Qt::AlignBottom,
                     QStringLiteral("click to aim the zoom"));
  }
}
