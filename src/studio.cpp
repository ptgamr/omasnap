/** @fileoverview The Studio window and its trim timeline. */
#include "studio.hpp"

#include "overlay-chrome.hpp"
#include "studio-preview.hpp"

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
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimer>
#include <QSignalBlocker>
#include <QSlider>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>

#include <cmath>

namespace {

const QColor kWindowBackground(16, 16, 19);
const QColor kTrack(255, 255, 255, 28);
const QColor kKept(120, 170, 255, 120);
const QColor kHandle(196, 214, 255);
const QColor kPlayhead(255, 255, 255, 232);
const QColor kText(226, 226, 232);
const QColor kMuted(150, 150, 160);
const QColor kCue(120, 170, 255, 150);
const QColor kCueSelected(150, 195, 255, 220);
const QColor kCueEdge(220, 235, 255);
const QColor kLaneLabel(255, 255, 255, 90);

constexpr int kTimelineHeight = 84;
/// The trim bar's row, then the cue lane under it.
constexpr qreal kLaneGap = 10.0;
constexpr qreal kCueLaneHeight = 22.0;
constexpr qreal kCueEdgeGrab = 6.0;
constexpr qreal kTrackHeight = 12.0;
constexpr qreal kHandleWidth = 8.0;
/// Pointer slack around a handle, in pixels. A 8 px bar is a small target.
constexpr qreal kGrabSlack = 7.0;
constexpr qint64 kSeekStepMs = 5000;
/// Nothing useful can be trimmed to less than this, and a zero-length export
/// is just a broken file.
constexpr qint64 kMinimumTrimMs = 100;
/// Reading container metadata is fast and bounded.
constexpr int kProbeTimeoutMs = 5000;
/// How long a cue runs when it is dropped rather than dragged.
constexpr qint64 kDefaultCueMs = 2500;
/// How long dragging settles before the cues are written out.
constexpr int kSaveDebounceMs = 400;

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

StudioSource probeStudioSource(const QString &path) {
  StudioSource media;
  const QString probe = QStandardPaths::findExecutable(
      QStringLiteral("ffprobe"));
  if (probe.isEmpty() || path.isEmpty())
    return media;
  QProcess ffprobe;
  ffprobe.start(probe,
                {QStringLiteral("-v"), QStringLiteral("error"),
                 QStringLiteral("-select_streams"), QStringLiteral("v:0"),
                 QStringLiteral("-show_entries"),
                 QStringLiteral("stream=width,height,r_frame_rate"),
                 QStringLiteral("-show_entries"),
                 QStringLiteral("stream_side_data=rotation"),
                 QStringLiteral("-of"),
                 QStringLiteral("default=noprint_wrappers=1"), path});
  if (!ffprobe.waitForFinished(kProbeTimeoutMs)) {
    ffprobe.kill();
    ffprobe.waitForFinished(1000);
    return media;
  }
  int width = 0;
  int height = 0;
  const QStringList lines =
      QString::fromLatin1(ffprobe.readAllStandardOutput()).split(
          QLatin1Char('\n'), Qt::SkipEmptyParts);
  for (const QString &line : lines) {
    const qsizetype split = line.indexOf(QLatin1Char('='));
    if (split < 0)
      continue;
    const QString key = line.left(split).trimmed();
    const QString value = line.mid(split + 1).trimmed();
    if (key == QStringLiteral("width"))
      width = value.toInt();
    else if (key == QStringLiteral("height"))
      height = value.toInt();
    else if (key == QStringLiteral("rotation"))
      // ffprobe reports av_display_rotation_get: the angle by which the
      // matrix turns the coded frame *counter-clockwise* for display. Qt's
      // QTransform::rotate is clockwise, so what gets stored here is the
      // clockwise angle to apply -- the negation is the conversion, not a
      // mistake. A phone portrait file reports -90 and is displayed by
      // turning the stored landscape pixels 90 clockwise.
      media.rotation = ((-value.toInt() % 360) + 360) % 360;
    else if (key == QStringLiteral("r_frame_rate")) {
      const QStringList rate = value.split(QLatin1Char('/'));
      media.fpsNumerator = rate.value(0).toInt();
      media.fpsDenominator = rate.size() > 1 ? rate.at(1).toInt() : 1;
    }
  }
  // ffmpeg autorotates before the filter chain, so the size the zoom filter
  // has to produce is the rotated one.
  media.size = media.rotation == 90 || media.rotation == 270
                   ? QSize(height, width)
                   : QSize(width, height);
  return media;
}

QStringList studioExportArguments(const QString &source,
                                  const QString &destination, qint64 inPoint,
                                  qint64 outPoint, const ZoomTrack &zoom,
                                  const StudioSource &media) {
  if (source.isEmpty() || destination.isEmpty() || outPoint <= inPoint)
    return {};
  const ZoomPanExpressions camera =
      media.usable() ? zoomPanExpressions(zoom, media.fpsNumerator,
                                          media.fpsDenominator, inPoint)
                     : ZoomPanExpressions{};
  QStringList filter;
  if (!camera.identity) {
    filter << QStringLiteral("-vf")
           << QStringLiteral("zoompan=z='%1':x='%2':y='%3':d=1:s=%4x%5:fps=%6/%7")
                  .arg(camera.z, camera.x, camera.y,
                       QString::number(media.size.width()),
                       QString::number(media.size.height()),
                       QString::number(media.fpsNumerator),
                       QString::number(media.fpsDenominator));
  }
  // -ss before -i so ffmpeg seeks rather than decoding the whole head, and
  // -t rather than -to because a duration means the same thing whichever
  // timeline it is read against.
  return QStringList{QStringLiteral("-hide_banner"),
          QStringLiteral("-loglevel"),
          QStringLiteral("error"),
          QStringLiteral("-y"),
          QStringLiteral("-ss"),
          studioTimecode(inPoint),
          QStringLiteral("-i"),
          source,
          QStringLiteral("-t"),
          studioTimecode(outPoint - inPoint)} +
         filter +
         QStringList{
          // Explicit, because ffmpeg's default selection keeps a single
          // audio stream and a recording made with --audio and --mic has
          // two. `?` so a silent recording is not an error.
          QStringLiteral("-map"),
          QStringLiteral("0:v:0"),
          QStringLiteral("-map"),
          QStringLiteral("0:a?"),
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

void StudioTimeline::setTrack(const ZoomTrack *track) {
  track_ = track;
  update();
}

void StudioTimeline::setCuesEditable(bool editable) {
  if (cuesEditable_ == editable)
    return;
  cuesEditable_ = editable;
  if (!editable) {
    grabbedCue_ = 0;
    if (grabbed_ == Grab::CueBody || grabbed_ == Grab::CueStart ||
        grabbed_ == Grab::CueEnd)
      grabbed_ = Grab::None;
  }
  update();
}

void StudioTimeline::setSelectedCue(quint64 id) {
  if (selected_ == id)
    return;
  selected_ = id;
  update();
}

QRectF StudioTimeline::trackRect() const {
  const qreal inset = kHandleWidth;
  return {inset, 18.0, qMax<qreal>(1.0, width() - inset * 2.0), kTrackHeight};
}

QRectF StudioTimeline::cueLaneRect() const {
  const QRectF track = trackRect();
  return {track.left(), track.bottom() + kLaneGap, track.width(),
          kCueLaneHeight};
}

QRectF StudioTimeline::cueRect(const ZoomCue &cue) const {
  const QRectF lane = cueLaneRect();
  const qreal left = xForTime(cue.startMs);
  const qreal right = xForTime(cue.endMs);
  return {left, lane.top(), qMax<qreal>(3.0, right - left), lane.height()};
}

quint64 StudioTimeline::cueAt(const QPointF &position, Grab *edge) const {
  if (edge)
    *edge = Grab::CueBody;
  if (!track_ || !cueLaneRect().contains(position))
    return 0;
  for (const ZoomCue &cue : track_->cues) {
    const QRectF rect = cueRect(cue);
    if (!rect.contains(position))
      continue;
    if (edge) {
      // Wide enough to grab, but never so wide on a short cue that the body
      // becomes unreachable.
      const qreal grab = qMin<qreal>(kCueEdgeGrab, rect.width() / 3.0);
      if (position.x() - rect.left() <= grab)
        *edge = Grab::CueStart;
      else if (rect.right() - position.x() <= grab)
        *edge = Grab::CueEnd;
    }
    return cue.id;
  }
  return 0;
}

qreal StudioTimeline::xForTime(qint64 milliseconds) const {
  const QRectF track = trackRect();
  if (duration_ <= 0)
    return track.left();
  const qreal fraction = qBound<qreal>(
      0.0, static_cast<qreal>(milliseconds) / static_cast<qreal>(duration_),
      1.0);
  return track.left() + fraction * track.width();
}

qint64 StudioTimeline::timeForX(qreal x) const {
  const QRectF track = trackRect();
  const qreal fraction =
      qBound<qreal>(0.0, (x - track.left()) / track.width(), 1.0);
  return static_cast<qint64>(
      std::llround(fraction * static_cast<qreal>(duration_)));
}

StudioTimeline::Grab StudioTimeline::grabAt(const QPointF &position) const {
  if (duration_ <= 0)
    return Grab::None;
  // The cue lane is its own row, so a click there never means the playhead.
  if (cueLaneRect().contains(position)) {
    Grab edge = Grab::CueBody;
    return cueAt(position, &edge) != 0 ? edge : Grab::None;
  }
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
  if (cueLaneRect().contains(event->position())) {
    Grab edge = Grab::CueBody;
    const quint64 cue = cueAt(event->position(), &edge);
    grabbedCue_ = cuesEditable_ ? cue : 0;
    if (cue != 0 && track_) {
      for (const ZoomCue &entry : track_->cues) {
        if (entry.id == cue)
          grabOffsetMs_ = timeForX(event->position().x()) - entry.startMs;
      }
    }
    setSelectedCue(cue);
    emit cueSelected(cue);
    return; // A cue lane click never moves the playhead.
  }
  mouseMoveEvent(event);
}

void StudioTimeline::mouseMoveEvent(QMouseEvent *event) {
  if (grabbed_ == Grab::None) {
    const Grab hover = grabAt(event->position());
    if (hover != hovered_) {
      hovered_ = hover;
      const bool resizes = hover == Grab::In || hover == Grab::Out ||
                           hover == Grab::CueStart || hover == Grab::CueEnd;
      setCursor(resizes            ? Qt::SizeHorCursor
                : hover == Grab::CueBody ? Qt::OpenHandCursor
                                         : Qt::ArrowCursor);
      update();
    }
    return;
  }
  const qint64 time = timeForX(event->position().x());
  if (grabbed_ == Grab::CueBody || grabbed_ == Grab::CueStart ||
      grabbed_ == Grab::CueEnd) {
    if (grabbedCue_ == 0 || !track_)
      return;
    for (const ZoomCue &cue : track_->cues) {
      if (cue.id != grabbedCue_)
        continue;
      qint64 start = cue.startMs;
      qint64 end = cue.endMs;
      // Every bound is ordered before use: on a clip shorter than a cue the
      // natural expressions invert, and qBound is undefined there.
      if (grabbed_ == Grab::CueBody) {
        const qint64 length = end - start;
        const qint64 latest = qMax<qint64>(0, duration_ - length);
        start = qBound<qint64>(0, time - grabOffsetMs_, latest);
        end = start + length;
      } else if (grabbed_ == Grab::CueStart) {
        start = qBound<qint64>(0, time, qMax<qint64>(0, end - kMinCueMs));
      } else {
        end = qBound<qint64>(start + kMinCueMs, time,
                             qMax<qint64>(start + kMinCueMs, duration_));
      }
      emit cueMoved(grabbedCue_, start, end);
      return;
    }
    return;
  }
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
  case Grab::CueBody:
  case Grab::CueStart:
  case Grab::CueEnd:
    break;
  }
}

void StudioTimeline::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton)
    return;
  grabbed_ = Grab::None;
  grabbedCue_ = 0;
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

  // The cue lane, under the trim bar: each cue is a block you can drag by
  // its body and resize by either edge.
  const QRectF lane = cueLaneRect();
  painter.setPen(Qt::NoPen);
  painter.setBrush(QColor(255, 255, 255, 14));
  painter.drawRoundedRect(lane, 4, 4);
  painter.setPen(kLaneLabel);
  painter.setFont(chromeFont(10));
  painter.drawText(QRectF(0, lane.top(), lane.left() - 4.0, lane.height()),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("zoom"));
  if (track_) {
    for (const ZoomCue &cue : track_->cues) {
      const QRectF rect = cueRect(cue);
      const bool chosen = cue.id == selected_;
      painter.setPen(Qt::NoPen);
      painter.setBrush(chosen ? kCueSelected : kCue);
      painter.drawRoundedRect(rect, 4, 4);
      if (chosen) {
        painter.setBrush(kCueEdge);
        painter.drawRoundedRect(
            QRectF(rect.left(), rect.top(), 2.5, rect.height()), 1, 1);
        painter.drawRoundedRect(
            QRectF(rect.right() - 2.5, rect.top(), 2.5, rect.height()), 1, 1);
      }
      if (rect.width() > 34.0) {
        painter.setPen(QColor(16, 20, 30));
        painter.setFont(chromeMonoFont(10));
        painter.drawText(rect, Qt::AlignCenter,
                         QStringLiteral("%1x").arg(cue.scale, 0, 'f', 1));
      }
    }
  }

  // The playhead spans both rows, so a cue's position against it is legible.
  const qreal playX = xForTime(position_);
  painter.setPen(QPen(kPlayhead, 2));
  painter.drawLine(QPointF(playX, track.top() - 9.0),
                   QPointF(playX, lane.bottom() + 4.0));
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

  preview_ = new StudioPreview(this);
  timeline_ = new StudioTimeline(this);
  timeline_->setTrack(&zoom_);

  playButton_ = new QPushButton(QStringLiteral("Play"), this);
  auto *inButton = new QPushButton(QStringLiteral("Set in"), this);
  auto *outButton = new QPushButton(QStringLiteral("Set out"), this);
  resetButton_ = new QPushButton(QStringLiteral("Reset trim"), this);
  addZoomButton_ = new QPushButton(QStringLiteral("Add zoom"), this);
  removeZoomButton_ = new QPushButton(QStringLiteral("Remove zoom"), this);
  zoomSlider_ = new QSlider(Qt::Horizontal, this);
  // Tenths of a times, so the slider is integral without feeling steppy.
  zoomSlider_->setRange(static_cast<int>(kMinZoomScale * 10) + 1,
                        static_cast<int>(kMaxZoomScale * 10));
  zoomSlider_->setFixedWidth(120);
  zoomLabel_ = new QLabel(this);
  zoomLabel_->setFont(chromeMonoFont(12));
  exportButton_ = new QPushButton(QStringLiteral("Export"), this);
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

  auto *zoomControls = new QHBoxLayout;
  zoomControls->setSpacing(8);
  zoomControls->addWidget(addZoomButton_);
  zoomControls->addWidget(removeZoomButton_);
  zoomControls->addWidget(zoomSlider_);
  zoomControls->addWidget(zoomLabel_);
  zoomControls->addStretch(1);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setSpacing(8);
  layout->addWidget(preview_, 1);
  layout->addWidget(timeline_);
  layout->addLayout(zoomControls);
  layout->addLayout(controls);

  audio_ = new QAudioOutput(this);
  sink_ = new QVideoSink(this);
  player_ = new QMediaPlayer(this);
  player_->setAudioOutput(audio_);
  // Frames rather than a video widget: the preview draws them through the
  // zoom model, which is what makes it show what the export will produce.
  player_->setVideoSink(sink_);
  connect(sink_, &QVideoSink::videoFrameChanged, this,
          [this](const QVideoFrame &frame) {
            const QImage image = frame.toImage();
            if (image.isNull())
              return;
            // The frame's own timestamp, not whatever position last arrived:
            // the two signals have no ordering contract, so during playback
            // or a seek the widget could otherwise crop one frame using the
            // time of its neighbour.
            if (frame.startTime() >= 0)
              preview_->setPosition(frame.startTime() / 1000);
            preview_->setFrame(image);
          });

  connect(playButton_, &QPushButton::clicked, this,
          &StudioWindow::togglePlayback);
  connect(inButton, &QPushButton::clicked, this, &StudioWindow::setTrimIn);
  connect(outButton, &QPushButton::clicked, this, &StudioWindow::setTrimOut);
  connect(resetButton_, &QPushButton::clicked, this, &StudioWindow::resetTrim);
  connect(exportButton_, &QPushButton::clicked, this,
          &StudioWindow::startExport);
  connect(addZoomButton_, &QPushButton::clicked, this,
          &StudioWindow::addZoomAtPlayhead);
  connect(removeZoomButton_, &QPushButton::clicked, this,
          &StudioWindow::removeSelectedZoom);
  connect(zoomSlider_, &QSlider::valueChanged, this, [this](int value) {
    setSelectedZoomScale(value / 10.0);
  });
  connect(preview_, &StudioPreview::targetPicked, this, &StudioWindow::aimZoom);
  connect(timeline_, &StudioTimeline::cueSelected, this, [this](quint64 id) {
    // Move the playhead into the cue that was clicked. Without this the
    // marker and slider describe one cue while the picture shows another,
    // and a click on the picture edits a third.
    for (const ZoomCue &cue : zoom_.cues) {
      if (cue.id != id)
        continue;
      if (player_->position() < cue.startMs || player_->position() >= cue.endMs)
        player_->setPosition(cue.startMs + (cue.endMs - cue.startMs) / 2);
      break;
    }
    refreshControls();
  });
  connect(timeline_, &StudioTimeline::cueMoved, this,
          [this](quint64 id, qint64 startMs, qint64 endMs) {
            for (ZoomCue &cue : zoom_.cues) {
              if (cue.id != id)
                continue;
              cue.startMs = startMs;
              cue.endMs = endMs;
              zoomChanged();
              return;
            }
          });

  connect(timeline_, &StudioTimeline::scrubbed, this,
          [this](qint64 milliseconds) { player_->setPosition(milliseconds); });
  connect(timeline_, &StudioTimeline::trimChanged, this,
          [this](qint64, qint64) { refreshControls(); });

  connect(player_, &QMediaPlayer::durationChanged, this,
          [this](qint64 duration) {
            timeline_->setDuration(duration);
            refreshControls();
          });
  connect(player_, &QMediaPlayer::mediaStatusChanged, this,
          [this](QMediaPlayer::MediaStatus status) {
            // A player that has never played has decoded nothing, so the
            // window would open on an empty rectangle. Coax one frame out of
            // it and stop again, at the start.
            const bool ready = status == QMediaPlayer::LoadedMedia ||
                               status == QMediaPlayer::BufferedMedia;
            if (!ready || primed_)
              return;
            primed_ = true;
            player_->play();
            player_->pause();
            player_->setPosition(0);
          });
  connect(player_, &QMediaPlayer::positionChanged, this,
          [this](qint64 position) {
            timeline_->setPosition(position);
            preview_->setPosition(position);
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

  preview_->setTrack(&zoom_);
  saveTimer_ = new QTimer(this);
  saveTimer_->setSingleShot(true);
  saveTimer_->setInterval(kSaveDebounceMs);
  connect(saveTimer_, &QTimer::timeout, this, &StudioWindow::saveZoom);
  // ffprobe rather than the player: the export needs the source's exact
  // frame rate, and a wrong one slides every cue. It also reports the
  // display rotation, which ffmpeg applies before the zoom filter and Qt
  // drops when converting a frame.
  media_ = probeStudioSource(path_);
  preview_->setRotation(media_.rotation);
  loadZoom();
  player_->setSource(QUrl::fromLocalFile(path_));
  refreshControls();
}

const ZoomCue *StudioWindow::activeCue() const {
  if (const quint64 selected = timeline_->selectedCue()) {
    for (const ZoomCue &cue : zoom_.cues) {
      if (cue.id == selected)
        return &cue;
    }
  }
  return zoomCueAt(zoom_, player_->position());
}

void StudioWindow::zoomChanged() {
  preview_->update();
  timeline_->update();
  // Coalesced: a drag emits this on every mouse move, and writing the file
  // each time is foreground I/O on a path that may be a network mount.
  saveTimer_->start();
  refreshControls();
}

void StudioWindow::aimZoom(const QPointF &target) {
  // Clicking aims the cue you are inside; outside one it makes a new cue
  // there, because that is what "click where I want to zoom" means when
  // nothing is happening yet.
  if (const ZoomCue *current = zoomCueAt(zoom_, player_->position())) {
    for (ZoomCue &cue : zoom_.cues) {
      if (cue.id != current->id)
        continue;
      cue.target = target;
      timeline_->setSelectedCue(cue.id);
      zoomChanged();
      return;
    }
  }
  const qreal scale = zoomSlider_->value() / 10.0;
  const quint64 id = addZoomCue(zoom_, player_->position(), target, scale,
                                kDefaultCueMs, player_->duration());
  if (id == 0) {
    setStatus(zoom_.cues.size() >= kMaxZoomCues
                  ? QStringLiteral("That is as many zooms as one clip takes")
                  : QStringLiteral("No room for a zoom here"));
    return;
  }
  timeline_->setSelectedCue(id);
  zoomChanged();
}

void StudioWindow::addZoomAtPlayhead() {
  const ZoomCue *current = activeCue();
  const QPointF target = current ? current->target : QPointF(0.5, 0.5);
  const quint64 id =
      addZoomCue(zoom_, player_->position(), target,
                 zoomSlider_->value() / 10.0, kDefaultCueMs,
                 player_->duration());
  if (id == 0) {
    setStatus(zoom_.cues.size() >= kMaxZoomCues
                  ? QStringLiteral("That is as many zooms as one clip takes")
                  : QStringLiteral("No room for a zoom here"));
    return;
  }
  timeline_->setSelectedCue(id);
  zoomChanged();
}

void StudioWindow::removeSelectedZoom() {
  const ZoomCue *cue = activeCue();
  if (!cue)
    return;
  removeZoomCue(zoom_, cue->id);
  timeline_->setSelectedCue(0);
  zoomChanged();
}

void StudioWindow::setSelectedZoomScale(qreal scale) {
  const ZoomCue *current = activeCue();
  if (!current || qFuzzyCompare(current->scale, scale))
    return;
  for (ZoomCue &cue : zoom_.cues) {
    if (cue.id != current->id)
      continue;
    cue.scale = qBound(kMinZoomScale, scale, kMaxZoomScale);
    zoomChanged();
    return;
  }
}

QString StudioWindow::zoomSidecarPath() const {
  return path_ + QStringLiteral(".omasnap-zoom.json");
}

void StudioWindow::loadZoom() {
  const QString path = zoomSidecarPath();
  if (QFileInfo(path).isSymLink()) {
    setStatus(QStringLiteral("Ignoring a symlinked zoom sidecar"));
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return;
  QString error;
  if (!readZoomTrack(QJsonDocument::fromJson(file.readAll()).object(), zoom_,
                     error))
    setStatus(error);
}

void StudioWindow::saveZoom() {
  const QString path = zoomSidecarPath();
  // Never through a link. The path is predictable and sits next to a file the
  // user cares about, so a symlink there pointed a truncating write at
  // whatever it named -- including the recording itself.
  if (QFileInfo(path).isSymLink()) {
    setStatus(QStringLiteral("Refusing to write the zoom cues through a "
                             "symlink"));
    return;
  }
  // Beside the recording rather than inside it: the master stays untouched,
  // and reopening the file brings the cues back. QSaveFile writes a
  // temporary and renames it into place, so an interrupted save leaves the
  // previous cues rather than half a file.
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) {
    setStatus(QStringLiteral("Could not save the zoom cues"));
    return;
  }
  file.write(QJsonDocument(writeZoomTrack(zoom_)).toJson(
      QJsonDocument::Indented));
  if (!file.commit())
    setStatus(QStringLiteral("Could not save the zoom cues"));
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

  const ZoomCue *cue = activeCue();
  // Editing is off while an export runs: ffmpeg already has its expressions,
  // so a cue moved now would change the preview and the sidecar while the
  // file being written keeps the old framing -- and it would still be
  // announced as saved. It is also off when the source could not be probed,
  // because the export cannot apply a zoom it has no frame rate for, and
  // letting cues be built that silently vanish is worse than not offering
  // them.
  const bool editable = duration > 0 && !mediaFailed_ && !exporting &&
                        media_.usable();
  addZoomButton_->setEnabled(editable);
  removeZoomButton_->setEnabled(editable && cue != nullptr);
  zoomSlider_->setEnabled(editable && cue != nullptr);
  preview_->setPickable(editable);
  timeline_->setCuesEditable(editable);
  {
    // Reflecting the cue must not look like the user moved the slider.
    const QSignalBlocker quiet(zoomSlider_);
    zoomSlider_->setValue(
        static_cast<int>(qRound((cue ? cue->scale : 2.0) * 10)));
  }
  zoomLabel_->setText(cue ? QStringLiteral("%1x").arg(cue->scale, 0, 'f', 1)
                          : QStringLiteral("—"));
  preview_->setTargetMarker(cue != nullptr,
                            cue ? cue->target : QPointF(0.5, 0.5));
  if (!media_.usable())
    zoomLabel_->setText(
        QStringLiteral("no zoom: ffprobe could not read this file"));
  else if (exporting)
    zoomLabel_->setText(QStringLiteral("exporting…"));
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
  const QStringList arguments =
      studioExportArguments(path_, destination, timeline_->trimIn(),
                            timeline_->trimOut(), zoom_, media_);
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
