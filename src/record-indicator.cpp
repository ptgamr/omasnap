/** @fileoverview The recording indicator pill. */
#include "record-indicator.hpp"

#include "overlay-chrome.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <cmath>
#include <utility>

namespace {

constexpr int kHeight = 34;
constexpr qreal kPadding = 12.0;
constexpr qreal kDotRadius = 5.0;
constexpr qreal kControlSize = 22.0;
constexpr qreal kGap = 8.0;
constexpr int kTimeFontPixels = 14;
/// One pulse cycle, in ticks of the 100 ms repaint below.
constexpr int kPulsePeriod = 16;

const QColor kBackground(18, 18, 22, 236);
const QColor kBorder(255, 255, 255, 36);
const QColor kText(236, 236, 240);
const QColor kRecordRed(232, 72, 72);
const QColor kPausedAmber(232, 176, 72);
const QColor kControlHover(255, 255, 255, 28);
const QColor kControlPressed(255, 255, 255, 52);

} // namespace

QString formatRecordingTime(qint64 milliseconds) {
  const qint64 total = qMax<qint64>(0, milliseconds) / 1000;
  const qint64 hours = total / 3600;
  const qint64 minutes = (total / 60) % 60;
  const qint64 seconds = total % 60;
  if (hours > 0)
    return QStringLiteral("%1:%2:%3")
        .arg(hours)
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'));
}

RecordIndicator::RecordIndicator(QWidget *parent) : QWidget(parent) {
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setAttribute(Qt::WA_TranslucentBackground);
  setMouseTracking(true);
  setCursor(Qt::ArrowCursor);

  auto *pulse = new QTimer(this);
  pulse->setInterval(100);
  connect(pulse, &QTimer::timeout, this, [this] {
    pulseTick_ = (pulseTick_ + 1) % kPulsePeriod;
    // Only the dot changes; repainting the whole pill 10x/s would be paying
    // for text layout that has not moved.
    update(QRect(0, 0, kHeight, kHeight));
  });
  pulse->start();
}

QString RecordIndicator::timeText() const {
  if (!message_.isEmpty())
    return message_;
  switch (phase_) {
  case Phase::Starting:
    return QStringLiteral("Starting…");
  case Phase::Stopping:
    return QStringLiteral("Saving…");
  case Phase::Failed:
    return QStringLiteral("Recording failed");
  case Phase::Paused:
  case Phase::Recording:
    break;
  }
  return formatRecordingTime(elapsedMs_);
}

QSize RecordIndicator::sizeHint() const {
  const QFontMetricsF metrics(chromeMonoFont(kTimeFontPixels));
  qreal width = kPadding + kDotRadius * 2 + kGap +
                metrics.horizontalAdvance(timeText()) + kPadding;
  if (phase_ == Phase::Recording || phase_ == Phase::Paused)
    width += kGap + kControlSize * 2 + kGap;
  return {static_cast<int>(std::ceil(width)), kHeight};
}

QRectF RecordIndicator::pauseRect() const {
  if (phase_ != Phase::Recording && phase_ != Phase::Paused)
    return {};
  const qreal right = width() - kPadding;
  return {right - kControlSize * 2 - kGap, (kHeight - kControlSize) / 2.0,
          kControlSize, kControlSize};
}

QRectF RecordIndicator::stopRect() const {
  if (phase_ != Phase::Recording && phase_ != Phase::Paused)
    return {};
  const qreal right = width() - kPadding;
  return {right - kControlSize, (kHeight - kControlSize) / 2.0, kControlSize,
          kControlSize};
}

RecordIndicator::Control
RecordIndicator::controlAt(const QPointF &position) const {
  if (stopRect().contains(position))
    return Control::Stop;
  if (pauseRect().contains(position))
    return Control::Pause;
  return Control::None;
}

void RecordIndicator::applySize() {
  updateGeometry();
  const QSize wanted = sizeHint();
  resize(wanted);
  // Only when it really moved: the clock calls this once a second, and each
  // announcement is a Wayland round trip.
  if (wanted != announcedSize_) {
    announcedSize_ = wanted;
    emit desiredSizeChanged(wanted);
  }
  update();
}

void RecordIndicator::setPhase(Phase phase) {
  if (phase_ == phase)
    return;
  phase_ = phase;
  if (phase != Phase::Failed)
    message_.clear();
  hovered_ = Control::None;
  pressed_ = Control::None;
  applySize();
}

void RecordIndicator::setElapsed(qint64 milliseconds) {
  if (elapsedMs_ / 1000 == milliseconds / 1000)
    return; // The pill only shows whole seconds.
  elapsedMs_ = milliseconds;
  if (phase_ != Phase::Recording && phase_ != Phase::Paused)
    return;
  // Crossing an hour makes the clock wider; every other tick resizes to the
  // same value and costs nothing.
  applySize();
}

void RecordIndicator::setMessage(const QString &message) {
  message_ = message;
  applySize();
}

void RecordIndicator::leaveEvent(QEvent *event) {
  QWidget::leaveEvent(event);
  if (hovered_ != Control::None) {
    hovered_ = Control::None;
    update();
  }
}

void RecordIndicator::mouseMoveEvent(QMouseEvent *event) {
  const Control control = controlAt(event->position());
  if (control == hovered_)
    return;
  hovered_ = control;
  update();
}

void RecordIndicator::mousePressEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton)
    return;
  pressed_ = controlAt(event->position());
  update();
}

void RecordIndicator::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton)
    return;
  const Control released = controlAt(event->position());
  const Control pressed = std::exchange(pressed_, Control::None);
  update();
  if (released != pressed || released == Control::None)
    return;
  if (released == Control::Stop)
    emit stopRequested();
  else
    emit pauseRequested(phase_ != Phase::Paused);
}

void RecordIndicator::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const QRectF pill(0.5, 0.5, width() - 1.0, height() - 1.0);
  painter.setPen(QPen(kBorder, 1));
  painter.setBrush(kBackground);
  painter.drawRoundedRect(pill, 11, 11);

  // The dot: solid red while recording and pulsing, amber and still while
  // paused, dimmed while starting or saving.
  const QPointF dotCenter(kPadding + kDotRadius, height() / 2.0);
  QColor dot = phase_ == Phase::Paused ? kPausedAmber : kRecordRed;
  if (phase_ == Phase::Recording) {
    const qreal wave =
        0.5 + 0.5 * std::cos(2.0 * M_PI * pulseTick_ / kPulsePeriod);
    dot.setAlphaF(0.55 + 0.45 * wave);
  } else if (phase_ != Phase::Paused && phase_ != Phase::Failed) {
    dot.setAlphaF(0.4);
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(dot);
  painter.drawEllipse(dotCenter, kDotRadius, kDotRadius);

  const QRectF pause = pauseRect();
  const qreal textLeft = kPadding + kDotRadius * 2 + kGap;
  const qreal textRight =
      pause.isNull() ? width() - kPadding : pause.left() - kGap;
  const QRectF textRect(textLeft, 0, qMax<qreal>(0, textRight - textLeft),
                        height());
  painter.setFont(chromeMonoFont(kTimeFontPixels));
  painter.setPen(phase_ == Phase::Failed ? kRecordRed : kText);
  painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, timeText());

  const auto paintControl = [&](const QRectF &rect, Control control) {
    if (rect.isNull())
      return;
    if (pressed_ == control || hovered_ == control) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(pressed_ == control ? kControlPressed : kControlHover);
      painter.drawRoundedRect(rect, 6, 6);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(kText);
    const QPointF center = rect.center();
    if (control == Control::Stop) {
      painter.drawRoundedRect(QRectF(center.x() - 4.5, center.y() - 4.5, 9, 9),
                              1.5, 1.5);
      return;
    }
    if (phase_ == Phase::Paused) {
      // Resume: the triangle says what the click does next, not what the
      // recorder is doing now.
      QPainterPath play;
      play.moveTo(center.x() - 3.5, center.y() - 5.0);
      play.lineTo(center.x() + 4.5, center.y());
      play.lineTo(center.x() - 3.5, center.y() + 5.0);
      play.closeSubpath();
      painter.drawPath(play);
      return;
    }
    painter.drawRoundedRect(QRectF(center.x() - 4.5, center.y() - 5, 3.5, 10),
                            1, 1);
    painter.drawRoundedRect(QRectF(center.x() + 1, center.y() - 5, 3.5, 10), 1,
                            1);
  };
  paintControl(pause, Control::Pause);
  paintControl(stopRect(), Control::Stop);
}
