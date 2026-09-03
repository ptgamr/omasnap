/** @fileoverview The Studio window and its trim timeline. */
#include "studio.hpp"

#include "overlay-chrome.hpp"

#include <QAudioOutput>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

#include <cmath>

namespace {

const QColor kWindowBackground(16, 16, 19);
const QColor kTrack(255, 255, 255, 28);
const QColor kKept(120, 170, 255, 120);
const QColor kHandle(196, 214, 255);
const QColor kPlayhead(255, 255, 255, 232);
const QColor kText(226, 226, 232);
const QColor kMuted(150, 150, 160);

constexpr int kTimelineHeight = 46;
constexpr qreal kTrackHeight = 12.0;
constexpr qreal kHandleWidth = 8.0;
/// Pointer slack around a handle, in pixels. A 8 px bar is a small target.
constexpr qreal kGrabSlack = 7.0;
constexpr qint64 kSeekStepMs = 5000;
/// Nothing useful can be trimmed to less than this, and a zero-length export
/// is just a broken file.
constexpr qint64 kMinimumTrimMs = 100;

const auto kControlStyle = QStringLiteral(R"(
QWidget { color: #e2e2e8; }
QPushButton {
  background: rgba(255,255,255,0.07);
  border: 1px solid rgba(255,255,255,0.10);
  border-radius: 7px; padding: 5px 12px;
}
QPushButton:hover:enabled { background: rgba(255,255,255,0.13); }
QPushButton:pressed:enabled { background: rgba(255,255,255,0.20); }
QPushButton:disabled { color: rgba(226,226,232,0.35); }
)");

} // namespace

QString studioTimecode(qint64 milliseconds) {
  const qint64 total = qMax<qint64>(0, milliseconds);
  return QStringLiteral("%1:%2:%3.%4")
      .arg(total / 3600000, 2, 10, QLatin1Char('0'))
      .arg((total / 60000) % 60, 2, 10, QLatin1Char('0'))
      .arg((total / 1000) % 60, 2, 10, QLatin1Char('0'))
      .arg(total % 1000, 3, 10, QLatin1Char('0'));
}

QString studioClock(qint64 milliseconds) {
  const qint64 total = qMax<qint64>(0, milliseconds) / 1000;
  return QStringLiteral("%1:%2")
      .arg(total / 60)
      .arg(total % 60, 2, 10, QLatin1Char('0'));
}

QStringList studioExportArguments(const QString &source,
                                  const QString &destination, qint64 inPoint,
                                  qint64 outPoint) {
  if (source.isEmpty() || destination.isEmpty() || outPoint <= inPoint)
    return {};
  // -ss before -i so ffmpeg seeks rather than decoding the whole head, and
  // -t rather than -to because a duration means the same thing whichever
  // timeline it is read against.
  return {QStringLiteral("-hide_banner"),
          QStringLiteral("-loglevel"),
          QStringLiteral("error"),
          QStringLiteral("-y"),
          QStringLiteral("-ss"),
          studioTimecode(inPoint),
          QStringLiteral("-i"),
          source,
          QStringLiteral("-t"),
          studioTimecode(outPoint - inPoint),
          QStringLiteral("-c:v"),
          QStringLiteral("libx264"),
          QStringLiteral("-crf"),
          QStringLiteral("20"),
          QStringLiteral("-preset"),
          QStringLiteral("veryfast"),
          QStringLiteral("-pix_fmt"),
          QStringLiteral("yuv420p"),
          QStringLiteral("-c:a"),
          QStringLiteral("aac"),
          QStringLiteral("-map_metadata"),
          QStringLiteral("-1"),
          QStringLiteral("-movflags"),
          QStringLiteral("+faststart"),
          destination};
}

QString studioExportPath(const QString &source) {
  const QFileInfo info(source);
  const QDir directory = info.dir();
  const QString stem = info.completeBaseName();
  QString candidate =
      directory.filePath(stem + QStringLiteral("-trim.mp4"));
  for (int index = 2; QFileInfo::exists(candidate) && index < 1000; ++index)
    candidate = directory.filePath(
        QStringLiteral("%1-trim-%2.mp4").arg(stem).arg(index));
  return candidate;
}

StudioTimeline::StudioTimeline(QWidget *parent) : QWidget(parent) {
  setMouseTracking(true);
  setMinimumHeight(kTimelineHeight);
}

QSize StudioTimeline::sizeHint() const { return {480, kTimelineHeight}; }

QRectF StudioTimeline::trackRect() const {
  const qreal inset = kHandleWidth;
  return {inset, (height() - kTrackHeight) / 2.0,
          qMax<qreal>(1.0, width() - inset * 2.0), kTrackHeight};
}

qreal StudioTimeline::xForTime(qint64 milliseconds) const {
  const QRectF track = trackRect();
  if (duration_ <= 0)
    return track.left();
  const qreal fraction =
      qBound<qreal>(0.0, static_cast<qreal>(milliseconds) / duration_, 1.0);
  return track.left() + fraction * track.width();
}

qint64 StudioTimeline::timeForX(qreal x) const {
  const QRectF track = trackRect();
  const qreal fraction =
      qBound<qreal>(0.0, (x - track.left()) / track.width(), 1.0);
  return static_cast<qint64>(std::llround(fraction * duration_));
}

StudioTimeline::Grab StudioTimeline::grabAt(const QPointF &position) const {
  if (duration_ <= 0)
    return Grab::None;
  const qreal x = position.x();
  // In beats out beats the playhead when they overlap: the handles are the
  // only things that cannot be reached another way.
  if (std::abs(x - xForTime(trimIn_)) <= kGrabSlack)
    return Grab::In;
  if (std::abs(x - xForTime(trimOut_)) <= kGrabSlack)
    return Grab::Out;
  return Grab::Playhead;
}

void StudioTimeline::setDuration(qint64 milliseconds) {
  duration_ = qMax<qint64>(0, milliseconds);
  trimIn_ = 0;
  trimOut_ = duration_;
  update();
}

void StudioTimeline::setPosition(qint64 milliseconds) {
  const qint64 clamped = qBound<qint64>(0, milliseconds, duration_);
  if (clamped == position_)
    return;
  position_ = clamped;
  update();
}

void StudioTimeline::setTrim(qint64 inPoint, qint64 outPoint) {
  trimIn_ = qBound<qint64>(0, inPoint, duration_);
  trimOut_ = qBound<qint64>(0, outPoint, duration_);
  if (trimOut_ < trimIn_)
    std::swap(trimIn_, trimOut_);
  update();
}

void StudioTimeline::leaveEvent(QEvent *event) {
  QWidget::leaveEvent(event);
  if (hovered_ != Grab::None) {
    hovered_ = Grab::None;
    update();
  }
}

void StudioTimeline::mousePressEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton || duration_ <= 0)
    return;
  grabbed_ = grabAt(event->position());
  mouseMoveEvent(event);
}

void StudioTimeline::mouseMoveEvent(QMouseEvent *event) {
  if (grabbed_ == Grab::None) {
    const Grab hover = grabAt(event->position());
    if (hover != hovered_) {
      hovered_ = hover;
      setCursor(hover == Grab::In || hover == Grab::Out ? Qt::SizeHorCursor
                                                        : Qt::ArrowCursor);
      update();
    }
    return;
  }
  const qint64 time = timeForX(event->position().x());
  switch (grabbed_) {
  case Grab::In:
    // The handles never cross, and never close to nothing.
    setTrim(qMin(time, trimOut_ - kMinimumTrimMs), trimOut_);
    emit trimChanged(trimIn_, trimOut_);
    break;
  case Grab::Out:
    setTrim(trimIn_, qMax(time, trimIn_ + kMinimumTrimMs));
    emit trimChanged(trimIn_, trimOut_);
    break;
  case Grab::Playhead:
    setPosition(time);
    emit scrubbed(position_);
    break;
  case Grab::None:
    break;
  }
}

void StudioTimeline::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton)
    grabbed_ = Grab::None;
}

void StudioTimeline::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const QRectF track = trackRect();

  painter.setPen(Qt::NoPen);
  painter.setBrush(kTrack);
  painter.drawRoundedRect(track, 4, 4);

  if (duration_ <= 0)
    return;

  const qreal inX = xForTime(trimIn_);
  const qreal outX = xForTime(trimOut_);
  painter.setBrush(kKept);
  painter.drawRoundedRect(QRectF(inX, track.top(), qMax<qreal>(1.0, outX - inX),
                                 track.height()),
                          4, 4);

  const auto paintHandle = [&](qreal x, Grab which) {
    painter.setBrush(hovered_ == which || grabbed_ == which
                         ? QColor(255, 255, 255)
                         : kHandle);
    painter.drawRoundedRect(QRectF(x - kHandleWidth / 2.0, track.top() - 5.0,
                                   kHandleWidth, track.height() + 10.0),
                            3, 3);
  };
  paintHandle(inX, Grab::In);
  paintHandle(outX, Grab::Out);

  const qreal playX = xForTime(position_);
  painter.setPen(QPen(kPlayhead, 2));
  painter.drawLine(QPointF(playX, track.top() - 9.0),
                   QPointF(playX, track.bottom() + 9.0));
  painter.setPen(Qt::NoPen);
  painter.setBrush(kPlayhead);
  painter.drawEllipse(QPointF(playX, track.top() - 9.0), 3.0, 3.0);
}

StudioWindow::StudioWindow(QString path, QWidget *parent)
    : QWidget(parent), path_(std::move(path)) {
  setWindowTitle(QStringLiteral("%1 — OmaSnap Studio")
                     .arg(QFileInfo(path_).fileName()));
  setMinimumSize(640, 420);
  resize(960, 620);
  setStyleSheet(kControlStyle);
  setFont(chromeDefaultFont());

  video_ = new QVideoWidget(this);
  video_->setMinimumHeight(200);
  timeline_ = new StudioTimeline(this);

  playButton_ = new QPushButton(QStringLiteral("Play"), this);
  auto *inButton = new QPushButton(QStringLiteral("Set in"), this);
  auto *outButton = new QPushButton(QStringLiteral("Set out"), this);
  resetButton_ = new QPushButton(QStringLiteral("Reset trim"), this);
  exportButton_ = new QPushButton(QStringLiteral("Export trimmed"), this);
  timeLabel_ = new QLabel(this);
  timeLabel_->setFont(chromeMonoFont(13));
  statusLabel_ = new QLabel(this);
  statusLabel_->setFont(chromeFont(12));

  auto *controls = new QHBoxLayout;
  controls->setSpacing(8);
  controls->addWidget(playButton_);
  controls->addWidget(timeLabel_);
  controls->addSpacing(12);
  controls->addWidget(inButton);
  controls->addWidget(outButton);
  controls->addWidget(resetButton_);
  controls->addStretch(1);
  controls->addWidget(statusLabel_);
  controls->addWidget(exportButton_);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);
  layout->addWidget(video_, 1);
  layout->addWidget(timeline_);
  layout->addLayout(controls);

  audio_ = new QAudioOutput(this);
  player_ = new QMediaPlayer(this);
  player_->setAudioOutput(audio_);
  player_->setVideoOutput(video_);

  connect(playButton_, &QPushButton::clicked, this,
          &StudioWindow::togglePlayback);
  connect(inButton, &QPushButton::clicked, this, &StudioWindow::setTrimIn);
  connect(outButton, &QPushButton::clicked, this, &StudioWindow::setTrimOut);
  connect(resetButton_, &QPushButton::clicked, this, &StudioWindow::resetTrim);
  connect(exportButton_, &QPushButton::clicked, this,
          &StudioWindow::startExport);

  connect(timeline_, &StudioTimeline::scrubbed, this,
          [this](qint64 milliseconds) { player_->setPosition(milliseconds); });
  connect(timeline_, &StudioTimeline::trimChanged, this,
          [this](qint64, qint64) { refreshControls(); });

  connect(player_, &QMediaPlayer::durationChanged, this,
          [this](qint64 duration) {
            timeline_->setDuration(duration);
            refreshControls();
          });
  connect(player_, &QMediaPlayer::positionChanged, this,
          [this](qint64 position) {
            timeline_->setPosition(position);
            // Playback stops at the out point: the trim is what is being
            // reviewed, so playing past it would be reviewing the wrong cut.
            if (player_->playbackState() == QMediaPlayer::PlayingState &&
                position >= timeline_->trimOut() && timeline_->duration() > 0) {
              player_->pause();
              player_->setPosition(timeline_->trimOut());
            }
            refreshControls();
          });
  connect(player_, &QMediaPlayer::playbackStateChanged, this,
          [this](QMediaPlayer::PlaybackState) { refreshControls(); });
  connect(player_, &QMediaPlayer::errorOccurred, this,
          [this](QMediaPlayer::Error, const QString &message) {
            mediaFailed_ = true;
            setStatus(message.isEmpty()
                          ? QStringLiteral("Could not play this recording")
                          : message);
            refreshControls();
          });

  player_->setSource(QUrl::fromLocalFile(path_));
  refreshControls();
}

bool StudioWindow::hasMedia() const { return !mediaFailed_; }

void StudioWindow::setStatus(const QString &status) {
  statusLabel_->setText(status);
  statusLabel_->setStyleSheet(
      QStringLiteral("color: %1;").arg(kMuted.name()));
}

void StudioWindow::refreshControls() {
  const bool exporting = export_ != nullptr;
  const qint64 duration = timeline_->duration();
  const bool trimmed =
      duration > 0 &&
      (timeline_->trimIn() > 0 || timeline_->trimOut() < duration);
  playButton_->setText(player_->playbackState() == QMediaPlayer::PlayingState
                           ? QStringLiteral("Pause")
                           : QStringLiteral("Play"));
  playButton_->setEnabled(!mediaFailed_ && duration > 0);
  resetButton_->setEnabled(trimmed && !exporting);
  exportButton_->setEnabled(duration > 0 && !mediaFailed_ && !exporting);
  timeLabel_->setText(
      QStringLiteral("%1 / %2   ·   keep %3 – %4")
          .arg(studioClock(player_->position()), studioClock(duration),
               studioClock(timeline_->trimIn()),
               studioClock(timeline_->trimOut())));
}

void StudioWindow::togglePlayback() {
  if (player_->playbackState() == QMediaPlayer::PlayingState) {
    player_->pause();
    return;
  }
  // Starting outside the kept range would play material the export drops.
  if (player_->position() < timeline_->trimIn() ||
      player_->position() >= timeline_->trimOut())
    player_->setPosition(timeline_->trimIn());
  player_->play();
}

void StudioWindow::seekBy(qint64 milliseconds) {
  player_->setPosition(
      qBound<qint64>(0, player_->position() + milliseconds,
                     qMax<qint64>(0, timeline_->duration())));
}

void StudioWindow::setTrimIn() {
  timeline_->setTrim(qMin(player_->position(),
                          timeline_->trimOut() - kMinimumTrimMs),
                     timeline_->trimOut());
  refreshControls();
}

void StudioWindow::setTrimOut() {
  timeline_->setTrim(timeline_->trimIn(),
                     qMax(player_->position(),
                          timeline_->trimIn() + kMinimumTrimMs));
  refreshControls();
}

void StudioWindow::resetTrim() {
  timeline_->setTrim(0, timeline_->duration());
  refreshControls();
}

void StudioWindow::startExport() {
  if (export_)
    return;
  const QString ffmpeg = QStandardPaths::findExecutable(
      QStringLiteral("ffmpeg"));
  if (ffmpeg.isEmpty()) {
    setStatus(QStringLiteral("ffmpeg is not installed"));
    return;
  }
  const QString destination = studioExportPath(path_);
  const QStringList arguments = studioExportArguments(
      path_, destination, timeline_->trimIn(), timeline_->trimOut());
  if (arguments.isEmpty()) {
    setStatus(QStringLiteral("Nothing to export"));
    return;
  }

  export_ = new QProcess(this);
  export_->setProcessChannelMode(QProcess::MergedChannels);
  const auto conclude = [this, destination](bool ok) {
    if (!export_)
      return;
    export_->deleteLater();
    export_ = nullptr;
    if (ok && QFileInfo::exists(destination)) {
      QFile::setPermissions(destination,
                            QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      setStatus(QStringLiteral("Saved %1")
                    .arg(QFileInfo(destination).fileName()));
    } else {
      QFile::remove(destination);
      setStatus(QStringLiteral("Export failed"));
    }
    refreshControls();
  };
  connect(export_, &QProcess::finished, this,
          [conclude](int code, QProcess::ExitStatus status) {
            conclude(code == 0 && status == QProcess::NormalExit);
          });
  connect(export_, &QProcess::errorOccurred, this,
          [conclude] { conclude(false); });
  export_->start(ffmpeg, arguments);
  setStatus(QStringLiteral("Exporting…"));
  refreshControls();
}

void StudioWindow::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
  case Qt::Key_Space:
    togglePlayback();
    return;
  case Qt::Key_Left:
    seekBy(-kSeekStepMs);
    return;
  case Qt::Key_Right:
    seekBy(kSeekStepMs);
    return;
  case Qt::Key_I:
    setTrimIn();
    return;
  case Qt::Key_O:
    setTrimOut();
    return;
  case Qt::Key_R:
    resetTrim();
    return;
  case Qt::Key_E:
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
      startExport();
      return;
    }
    break;
  case Qt::Key_Escape:
    close();
    return;
  case Qt::Key_W:
    if (event->modifiers().testFlag(Qt::ControlModifier)) {
      close();
      return;
    }
    break;
  default:
    break;
  }
  QWidget::keyPressEvent(event);
}

void StudioWindow::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.fillRect(rect(), kWindowBackground);
}
