/** @fileoverview The Studio window and its trim timeline. */
#include "studio.hpp"

#include "overlay-chrome.hpp"
#include "studio-preview.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QAudioOutput>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <QtConcurrentRun>

#include <cmath>

namespace {

constexpr int kTimelineHeight = 176;
/// The trim bar's row, then the cue lane under it.
constexpr qreal kLaneGap = 10.0;
constexpr qreal kCueLaneHeight = 30.0;
constexpr qreal kCueEdgeGrab = 6.0;
constexpr qreal kTrackHeight = 44.0;
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
  const QString probe =
      QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
  if (probe.isEmpty() || path.isEmpty())
    return media;
  QProcess ffprobe;
  ffprobe.start(probe, {QStringLiteral("-v"), QStringLiteral("error"),
                        QStringLiteral("-select_streams"),
                        QStringLiteral("v:0"), QStringLiteral("-show_entries"),
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
  const QStringList lines = QString::fromLatin1(ffprobe.readAllStandardOutput())
                                .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
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
    {
      // A right angle is a transpose, which the preview can reproduce
      // exactly. Anything else -- ffmpeg takes a general rotation path
      // that resizes the canvas differently -- is refused rather than
      // framed differently in the two places.
      const double degrees = value.toDouble();
      const double snapped = qRound(degrees / 90.0) * 90.0;
      if (std::abs(degrees - snapped) > 0.5)
        media.unsupportedTransform = true;
      media.rotation = ((static_cast<int>(-snapped) % 360) + 360) % 360;
    } else if (key == QStringLiteral("r_frame_rate")) {
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
                                  const StudioSource &media,
                                  const StudioStyle &style) {
  if (source.isEmpty() || destination.isEmpty() || outPoint <= inPoint)
    return {};
  const ZoomPanExpressions camera =
      media.usable() ? zoomPanExpressions(zoom, media.fpsNumerator,
                                          media.fpsDenominator, inPoint)
                     : ZoomPanExpressions{};
  QStringList filter;
  if (!camera.identity) {
    filter << QStringLiteral("-vf")
           << QStringLiteral(
                  "zoompan=z='%1':x='%2':y='%3':d=1:s=%4x%5:fps=%6/%7")
                  .arg(camera.z, camera.x, camera.y,
                       QString::number(media.size.width()),
                       QString::number(media.size.height()),
                       QString::number(media.fpsNumerator),
                       QString::number(media.fpsDenominator));
  }
  QString mappedVideo = QStringLiteral("0:v:0");
  if ((style.padding > 0 || style.radius > 0) && media.usable()) {
    const int width = media.size.width();
    const int height = media.size.height();
    const int cardWidth =
        qMax(2, qRound(width * (1 - 2 * style.padding / 100.0)) / 2 * 2);
    const int cardHeight =
        qMax(2, qRound(height * (1 - 2 * style.padding / 100.0)) / 2 * 2);
    const qreal radius = style.radius * height / 1080.0;
    QString card =
        camera.identity ? QString() : filter.last() + QLatin1Char(',');
    card += QStringLiteral("scale=%1:%2,format=rgba")
                .arg(cardWidth)
                .arg(cardHeight);
    if (radius > 0) {
      card += QStringLiteral(",geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*"
                             "clip(%1+0.5-hypot(max(abs(X-(W-1)/2)-(W/"
                             "2-%1),0),max(abs(Y-(H-1)/2)-(H/2-%1),0)),0,1)'")
                  .arg(radius, 0, 'f', 4);
    }
    const QString graph =
        QStringLiteral("[0:v:0]%1[card];color=c=%2:s=%3x%4:r=%5/%6[canvas];"
                       "[canvas][card]overlay=x=(W-w)/2:y=(H-h)/"
                       "2:shortest=1:format=auto,format=yuv420p[composed]")
            .arg(card, style.color().name())
            .arg(width)
            .arg(height)
            .arg(media.fpsNumerator)
            .arg(media.fpsDenominator);
    filter = {QStringLiteral("-filter_complex"), graph};
    mappedVideo = QStringLiteral("[composed]");
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
             mappedVideo,
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
  QString candidate = directory.filePath(stem + QStringLiteral("-trim.mp4"));
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
  const qreal inset = 64;
  return {inset, 38.0, qMax<qreal>(1.0, width() - inset - 16), kTrackHeight};
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

void StudioTimeline::setThumbnails(QVector<QImage> thumbnails) {
  thumbnails_ = std::move(thumbnails);
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
  emit editStarted();
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
      setCursor(resizes                  ? Qt::SizeHorCursor
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
  emit editFinished();
}

void StudioTimeline::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const QRectF track = trackRect();
  painter.fillRect(rect(), chrome_.background);
  painter.setFont(chromeMonoFont(10));
  painter.setPen(chrome_.mutedText());
  for (int tick = 0; tick <= 10; ++tick) {
    const qreal x = track.left() + track.width() * tick / 10;
    painter.drawLine(QPointF(x, 26), QPointF(x, 31));
    painter.drawText(QRectF(x - 22, 4, 44, 18), Qt::AlignCenter,
                     studioClock(duration_ * tick / 10));
  }
  painter.setFont(chromeMonoFont(10));
  painter.drawText(QRectF(0, track.top(), 55, track.height()),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("VIDEO"));

  painter.setPen(Qt::NoPen);
  painter.setBrush(chrome_.surface());
  painter.drawRect(track);
  if (!thumbnails_.isEmpty()) {
    painter.save();
    QPainterPath clip;
    clip.addRect(track);
    painter.setClipPath(clip);
    const qreal cellWidth =
        track.width() / static_cast<qreal>(thumbnails_.size());
    for (qsizetype i = 0; i < thumbnails_.size(); ++i)
      painter.drawImage(QRectF(track.left() + static_cast<qreal>(i) * cellWidth,
                               track.top(), cellWidth, track.height()),
                        thumbnails_.at(i));
    QColor veil = chrome_.background;
    veil.setAlphaF(0.72);
    painter.fillRect(track, veil);
    painter.restore();
  }

  if (duration_ <= 0)
    return;

  const qreal inX = xForTime(trimIn_);
  const qreal outX = xForTime(trimOut_);
  QColor kept = chrome_.accent;
  kept.setAlphaF(0.18);
  painter.setBrush(kept);
  painter.setPen(QPen(chrome_.border(), StudioChrome::borderWidth));
  painter.drawRect(
      QRectF(inX, track.top(), qMax<qreal>(1.0, outX - inX), track.height()));
  painter.setPen(chrome_.foreground);
  painter.setFont(chromeMonoFont(11));
  painter.drawText(track.adjusted(14, 0, -14, 0), Qt::AlignVCenter,
                   QStringLiteral("Screen recording"));
  painter.setPen(Qt::NoPen);

  const auto paintHandle = [&](qreal x, Grab which) {
    painter.setBrush(hovered_ == which || grabbed_ == which ? chrome_.foreground
                                                            : chrome_.accent);
    painter.drawRect(QRectF(x - kHandleWidth / 2.0, track.top() - 5.0,
                            kHandleWidth, track.height() + 10.0));
  };
  paintHandle(inX, Grab::In);
  paintHandle(outX, Grab::Out);

  // The cue lane, under the trim bar: each cue is a block you can drag by
  // its body and resize by either edge.
  const QRectF lane = cueLaneRect();
  painter.setPen(Qt::NoPen);
  painter.setBrush(chrome_.surface());
  painter.drawRect(lane);
  painter.setPen(chrome_.mutedText());
  painter.setFont(chromeMonoFont(10));
  painter.drawText(QRectF(0, lane.top(), lane.left() - 4.0, lane.height()),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("zoom"));
  if (track_) {
    for (const ZoomCue &cue : track_->cues) {
      const QRectF rect = cueRect(cue);
      const bool chosen = cue.id == selected_;
      painter.setPen(Qt::NoPen);
      painter.setBrush(chosen ? chrome_.accent : chrome_.selected());
      painter.drawRect(rect);
      if (chosen) {
        painter.setBrush(chrome_.onAccent());
        painter.drawRect(QRectF(rect.left(), rect.top(), 2.5, rect.height()));
        painter.drawRect(
            QRectF(rect.right() - 2.5, rect.top(), 2.5, rect.height()));
      }
      if (rect.width() > 34.0) {
        painter.setPen(chosen ? chrome_.onAccent() : chrome_.foreground);
        painter.setFont(chromeMonoFont(10));
        painter.drawText(rect, Qt::AlignCenter,
                         QStringLiteral("%1x").arg(cue.scale, 0, 'f', 1));
      }
    }
  }

  // The playhead spans both rows, so a cue's position against it is legible.
  const qreal playX = xForTime(position_);
  painter.setPen(QPen(chrome_.foreground, 2));
  painter.drawLine(QPointF(playX, track.top() - 9.0),
                   QPointF(playX, lane.bottom() + 4.0));
  painter.setPen(Qt::NoPen);
  painter.setBrush(chrome_.foreground);
  painter.drawRect(QRectF(playX - 3, track.top() - 12, 6, 6));
}

StudioWindow::StudioWindow(QString path, QWidget *parent, QString themePath)
    : QWidget(parent), path_(std::move(path)) {
  theme_ = new StudioTheme(this, std::move(themePath));
  setWindowTitle(
      QStringLiteral("%1 — OmaSnap Studio").arg(QFileInfo(path_).fileName()));
  setMinimumSize(980, 680);
  resize(1280, 820);
  setStyleSheet(theme_->chrome().styleSheet());
  setFont(chromeMonoFont(12));

  preview_ = new StudioPreview(this);
  preview_->setCanvasInset(28);
  timeline_ = new StudioTimeline(this);
  timeline_->setTrack(&zoom_);
  applyChrome();
  connect(theme_, &StudioTheme::changed, this, &StudioWindow::applyChrome);

  const auto button = [this](const QString &text, const QString &hint) {
    auto *result = new QPushButton(text, this);
    result->setToolTip(hint);
    result->setAccessibleName(hint);
    return result;
  };
  playButton_ =
      button(QStringLiteral("Play"), QStringLiteral("Play / pause · Space"));
  playButton_->setObjectName(QStringLiteral("play"));
  auto *inButton = new QPushButton(QStringLiteral("Set in"), this);
  auto *outButton = new QPushButton(QStringLiteral("Set out"), this);
  resetButton_ = new QPushButton(QStringLiteral("Reset trim"), this);
  addZoomButton_ = new QPushButton(QStringLiteral("Add zoom"), this);
  removeZoomButton_ = new QPushButton(QStringLiteral("Remove zoom"), this);
  zoomSlider_ = new QSlider(Qt::Horizontal, this);
  // Tenths of a times, so the slider is integral without feeling steppy.
  zoomSlider_->setRange(static_cast<int>(kMinZoomScale * 10) + 1,
                        static_cast<int>(kMaxZoomScale * 10));
  zoomSlider_->setObjectName(QStringLiteral("zoomScale"));
  zoomSlider_->setToolTip(QStringLiteral("Zoom magnification"));
  zoomLabel_ = new QLabel(this);
  zoomLabel_->setFont(chromeMonoFont(12));
  exportButton_ = new QPushButton(QStringLiteral("Export"), this);
  exportButton_->setObjectName(QStringLiteral("primary"));
  exportButton_->setToolTip(QStringLiteral("Export MP4 · Ctrl+E"));
  timeLabel_ = new QLabel(this);
  timeLabel_->setFont(chromeMonoFont(13));
  statusLabel_ = new QLabel(this);
  statusLabel_->setFont(chromeMonoFont(12));

  auto *header = new QWidget(this);
  header->setObjectName(QStringLiteral("studioHeader"));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(StudioChrome::panelPadding, 12,
                                   StudioChrome::panelPadding, 12);
  headerLayout->setSpacing(StudioChrome::gap);
  auto *brand = new QLabel(QStringLiteral("OmaSnap  /  Studio"), header);
  brand->setFont(chromeMonoFont(14));
  headerLayout->addWidget(brand);
  undoButton_ = button(QStringLiteral("Undo"), QStringLiteral("Undo · Ctrl+Z"));
  redoButton_ =
      button(QStringLiteral("Redo"), QStringLiteral("Redo · Ctrl+Shift+Z"));
  headerLayout->addSpacing(18);
  headerLayout->addWidget(undoButton_);
  headerLayout->addWidget(redoButton_);
  headerLayout->addStretch();
  auto *title = new QLabel(QFileInfo(path_).fileName(), header);
  fileLabel_ = title;
  title->setObjectName(QStringLiteral("muted"));
  title->setMaximumWidth(360);
  title->setToolTip(path_);
  headerLayout->addWidget(title);
  headerLayout->addStretch();
  auto *help =
      button(QStringLiteral("?"), QStringLiteral("Keyboard shortcuts · ?"));
  auto *inspectorToggle =
      button(QStringLiteral("Inspector"),
             QStringLiteral("Show / hide inspector · Ctrl+\\"));
  headerLayout->addWidget(help);
  headerLayout->addWidget(inspectorToggle);
  headerLayout->addWidget(exportButton_);

  inspector_ = new QWidget(this);
  inspector_->setObjectName(QStringLiteral("studioInspector"));
  inspector_->setMinimumWidth(260);
  inspector_->setMaximumWidth(340);
  auto *inspectorLayout = new QVBoxLayout(inspector_);
  inspectorLayout->setContentsMargins(
      StudioChrome::panelPadding, StudioChrome::panelPadding,
      StudioChrome::panelPadding, StudioChrome::panelPadding);
  inspectorLayout->setSpacing(StudioChrome::gap);
  auto *inspectorTitle = new QLabel(QStringLiteral("Inspector"), inspector_);
  inspectorTitle->setFont(chromeMonoFont(16));
  inspectorLayout->addWidget(inspectorTitle);
  auto *tabs = new QTabWidget(inspector_);
  inspectorLayout->addWidget(tabs, 1);
  auto *zoomPage = new QWidget(tabs);
  auto *zoomControls = new QVBoxLayout(zoomPage);
  zoomControls->setContentsMargins(0, 22, 0, 0);
  zoomControls->setSpacing(14);
  cueLabel_ = new QLabel(QStringLiteral("Select a zoom"), zoomPage);
  cueLabel_->setWordWrap(true);
  cueLabel_->setFont(chromeMonoFont(13));
  zoomControls->addWidget(cueLabel_);
  auto *zoomHint = new QLabel(
      QStringLiteral("Click the preview to place or aim a zoom. Drag a cue on "
                     "the timeline to move or resize it."),
      zoomPage);
  zoomHint->setObjectName(QStringLiteral("muted"));
  zoomHint->setWordWrap(true);
  zoomControls->addWidget(zoomHint);
  auto *scaleRow = new QHBoxLayout;
  scaleRow->addWidget(new QLabel(QStringLiteral("Magnification"), zoomPage));
  scaleRow->addStretch();
  scaleRow->addWidget(zoomLabel_);
  zoomControls->addLayout(scaleRow);
  zoomControls->addWidget(zoomSlider_);
  easeIn_ = new QSpinBox(zoomPage);
  easeOut_ = new QSpinBox(zoomPage);
  for (QSpinBox *spin : {easeIn_, easeOut_}) {
    spin->setRange(60, 3000);
    spin->setSingleStep(50);
    spin->setSuffix(QStringLiteral(" ms"));
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
  }
  const auto timingRow = [zoomControls, zoomPage](const QString &label,
                                                  QSpinBox *spin) {
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(label, zoomPage));
    row->addStretch();
    row->addWidget(spin);
    zoomControls->addLayout(row);
  };
  timingRow(QStringLiteral("Ease in"), easeIn_);
  timingRow(QStringLiteral("Ease out"), easeOut_);
  zoomControls->addWidget(addZoomButton_);
  zoomControls->addWidget(removeZoomButton_);
  addZoomButton_->setToolTip(QStringLiteral("Add a zoom at the playhead · Z"));
  removeZoomButton_->setToolTip(
      QStringLiteral("Remove selected zoom · Delete"));
  zoomControls->addStretch();
  auto *clipPage = new QWidget(tabs);
  auto *clipControls = new QVBoxLayout(clipPage);
  clipControls->setContentsMargins(0, 22, 0, 0);
  clipControls->setSpacing(14);
  sourceLabel_ = new QLabel(QStringLiteral("Reading recording…"), clipPage);
  sourceLabel_->setWordWrap(true);
  sourceLabel_->setFont(chromeMonoFont(12));
  clipControls->addWidget(sourceLabel_);
  clipControls->addWidget(inButton);
  clipControls->addWidget(outButton);
  clipControls->addWidget(resetButton_);
  inButton->setToolTip(QStringLiteral("Set trim start at playhead · I"));
  outButton->setToolTip(QStringLiteral("Set trim end at playhead · O"));
  resetButton_->setToolTip(QStringLiteral("Keep the complete recording · R"));
  auto *clipHint = new QLabel(
      QStringLiteral("Drag the handles on the video lane to choose the range "
                     "to export. The original recording stays untouched."),
      clipPage);
  clipHint->setWordWrap(true);
  clipHint->setObjectName(QStringLiteral("muted"));
  clipControls->addWidget(clipHint);
  clipControls->addStretch();
  tabs->addTab(zoomPage, QStringLiteral("Zoom"));
  tabs->addTab(clipPage, QStringLiteral("Clip"));
  auto *stylePage = new QWidget(tabs);
  auto *styleControls = new QVBoxLayout(stylePage);
  styleControls->setContentsMargins(0, 22, 0, 0);
  styleControls->setSpacing(14);
  styleControls->addWidget(
      new QLabel(QStringLiteral("Canvas background"), stylePage));
  background_ = new StudioComboBox(stylePage);
  background_->setChrome(theme_->chrome());
  background_->addItems({QStringLiteral("Midnight"), QStringLiteral("Lavender"),
                         QStringLiteral("Sand"), QStringLiteral("Pearl")});
  styleControls->addWidget(background_);
  padding_ = new QSlider(Qt::Horizontal, stylePage);
  padding_->setRange(0, 20);
  padding_->setObjectName(QStringLiteral("canvasPadding"));
  radius_ = new QSlider(Qt::Horizontal, stylePage);
  radius_->setRange(0, 64);
  const auto styleSlider = [styleControls, stylePage](const QString &text,
                                                      QSlider *slider,
                                                      const QString &unit) {
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(text, stylePage));
    row->addStretch();
    auto *value = new QLabel(QStringLiteral("0") + unit, stylePage);
    value->setFont(chromeMonoFont(12));
    row->addWidget(value);
    styleControls->addLayout(row);
    styleControls->addWidget(slider);
    QObject::connect(
        slider, &QSlider::valueChanged, value,
        [value, unit](int n) { value->setText(QString::number(n) + unit); });
  };
  styleSlider(QStringLiteral("Padding"), padding_, QStringLiteral("%"));
  styleSlider(QStringLiteral("Corner radius"), radius_, QStringLiteral(" px"));
  auto *styleHint = new QLabel(
      QStringLiteral("Add breathing room around the recording. Canvas styling "
                     "is included in your exported video."),
      stylePage);
  styleHint->setWordWrap(true);
  styleHint->setObjectName(QStringLiteral("muted"));
  styleControls->addWidget(styleHint);
  auto *resetStyle =
      button(QStringLiteral("Reset canvas"),
             QStringLiteral("Remove padding and rounded corners"));
  styleControls->addWidget(resetStyle);
  styleControls->addStretch();
  tabs->insertTab(0, stylePage, QStringLiteral("Canvas"));
  tabs->setCurrentIndex(1);
  connect(background_, &QComboBox::currentIndexChanged, this,
          &StudioWindow::styleChanged);
  for (QSlider *slider : {padding_, radius_}) {
    connect(slider, &QSlider::valueChanged, this, &StudioWindow::styleChanged);
    connect(slider, &QSlider::sliderPressed, this, &StudioWindow::beginEdit);
    connect(slider, &QSlider::sliderReleased, this, &StudioWindow::endEdit);
  }
  connect(resetStyle, &QPushButton::clicked, this, [this] {
    if (!background_->isEnabled())
      return;
    beginEdit();
    padding_->setValue(0);
    radius_->setValue(0);
    endEdit();
  });

  auto *timelinePanel = new QWidget(this);
  timelinePanel->setObjectName(QStringLiteral("timelinePanel"));
  auto *timelineLayout = new QVBoxLayout(timelinePanel);
  timelineLayout->setContentsMargins(16, 12, 16, 8);
  auto *transport = new QHBoxLayout;
  transport->setSpacing(StudioChrome::gap);
  auto *back =
      button(QStringLiteral("|‹"), QStringLiteral("Go to trim start · Home"));
  auto *forward =
      button(QStringLiteral("›|"), QStringLiteral("Go to trim end · End"));
  auto *previous =
      button(QStringLiteral("‹"), QStringLiteral("Previous frame · Left"));
  auto *next =
      button(QStringLiteral("›"), QStringLiteral("Next frame · Right"));
  transport->addWidget(timeLabel_);
  transport->addStretch();
  transport->addWidget(back);
  transport->addWidget(previous);
  transport->addWidget(playButton_);
  transport->addWidget(next);
  transport->addWidget(forward);
  transport->addStretch();
  auto *volume = new QSlider(Qt::Horizontal, timelinePanel);
  volume->setRange(0, 100);
  volume->setValue(100);
  volume->setFixedWidth(72);
  volume->setToolTip(QStringLiteral("Preview volume"));
  auto *mute = button(QStringLiteral("Mute"),
                      QStringLiteral("Mute / unmute preview · M"));
  mute->setCheckable(true);
  transport->addWidget(mute);
  transport->addWidget(volume);
  timelineLayout->addLayout(transport);
  timelineLayout->addWidget(timeline_);
  auto *footer = new QHBoxLayout;
  auto *legend =
      new QLabel(QStringLiteral("SPACE  Play / pause     ← →  Frame     SHIFT "
                                "+ ← →  5 seconds     Z  Add zoom"),
                 timelinePanel);
  shortcutLegend_ = legend;
  legend->setFont(chromeMonoFont(10));
  legend->setObjectName(QStringLiteral("muted"));
  footer->addWidget(legend);
  footer->addStretch();
  footer->addWidget(statusLabel_);
  timelineLayout->addLayout(footer);
  auto *workspace = new QWidget(this);
  auto *workspaceLayout = new QVBoxLayout(workspace);
  workspaceLayout->setContentsMargins(0, 0, 0, 0);
  workspaceLayout->setSpacing(0);
  workspaceLayout->addWidget(preview_, 1);
  workspaceLayout->addWidget(timelinePanel);
  auto *splitter = new QSplitter(Qt::Horizontal, this);
  splitter->addWidget(workspace);
  splitter->addWidget(inspector_);
  splitter->setChildrenCollapsible(false);
  splitter->setStretchFactor(0, 1);
  splitter->setSizes({970, 290});
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(1);
  layout->addWidget(header);
  layout->addWidget(splitter, 1);

  audio_ = new QAudioOutput(this);
  sink_ = new QVideoSink(this);
  player_ = new QMediaPlayer(this);
  player_->setAudioOutput(audio_);
  connect(preview_, &StudioPreview::previewFailed, this,
          [this](const QString &error) {
            player_->pause();
            mediaFailed_ = true;
            setStatus(error, true);
            refreshControls();
          });
  connect(help, &QPushButton::clicked, this, &StudioWindow::showShortcuts);
  connect(inspectorToggle, &QPushButton::clicked, this,
          &StudioWindow::toggleInspector);
  connect(undoButton_, &QPushButton::clicked, this, &StudioWindow::undoEdit);
  connect(redoButton_, &QPushButton::clicked, this, &StudioWindow::redoEdit);
  connect(back, &QPushButton::clicked, this, [this] {
    player_->pause();
    seekTo(timeline_->trimIn());
  });
  connect(forward, &QPushButton::clicked, this, [this] {
    player_->pause();
    seekTo(timeline_->trimOut());
  });
  connect(previous, &QPushButton::clicked, this, [this] { stepFrame(-1); });
  connect(next, &QPushButton::clicked, this, [this] { stepFrame(1); });
  connect(volume, &QSlider::valueChanged, this, [this](int value) {
    audio_->setVolume(static_cast<float>(value) / 100.0F);
  });
  connect(mute, &QPushButton::toggled, audio_, &QAudioOutput::setMuted);
  connect(audio_, &QAudioOutput::mutedChanged, mute, &QPushButton::setChecked);
  connect(easeIn_, &QSpinBox::valueChanged, this,
          [this](int ms) { setSelectedZoomTiming(true, ms); });
  connect(easeOut_, &QSpinBox::valueChanged, this,
          [this](int ms) { setSelectedZoomTiming(false, ms); });
  connect(zoomSlider_, &QSlider::sliderPressed, this, &StudioWindow::beginEdit);
  connect(zoomSlider_, &QSlider::sliderReleased, this, &StudioWindow::endEdit);
  connect(timeline_, &StudioTimeline::editStarted, this,
          &StudioWindow::beginEdit);
  connect(timeline_, &StudioTimeline::editFinished, this,
          &StudioWindow::endEdit);
  qApp->installEventFilter(this);
  // Frames rather than a video widget: the preview draws them through the
  // zoom model, which is what makes it show what the export will produce.
  player_->setVideoSink(sink_);
  connect(sink_, &QVideoSink::videoFrameChanged, this,
          [this](const QVideoFrame &frame) {
            // The frame's own timestamp, not whatever position last arrived:
            // the two signals have no ordering contract, so during playback
            // or a seek the widget could otherwise crop one frame using the
            // time of its neighbour.
            preview_->setVideoFrame(frame);
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
  connect(zoomSlider_, &QSlider::valueChanged, this,
          [this](int value) { setSelectedZoomScale(value / 10.0); });
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
          [this](qint64 milliseconds) {
            player_->pause();
            pendingSeek_ = milliseconds;
            if (!scrubTimer_->isActive())
              scrubTimer_->start();
          });
  connect(timeline_, &StudioTimeline::trimChanged, this,
          [this](qint64, qint64) {
            rememberEdit();
            refreshControls();
          });

  connect(
      player_, &QMediaPlayer::durationChanged, this, [this](qint64 duration) {
        timeline_->setDuration(duration);
        // The clip length is only known now, so a sidecar loaded before
        // it can hold cues past the end.
        normalizeZoomTrack(zoom_, duration);
        if (editIndex_ < 0 && duration > 0) {
          timeline_->setTrim(initialTrimIn_,
                             initialTrimOut_ < 0 ? duration : initialTrimOut_);
          rememberEdit();
        }
        if (!thumbnailsStarted_ && duration > 0) {
          thumbnailsStarted_ = true;
          const QString source = path_;
          thumbnailWatcher_.setFuture(QtConcurrent::run([source, duration] {
            QVector<QImage> thumbnails;
            const QString ffmpeg =
                QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
            if (ffmpeg.isEmpty())
              return thumbnails;
            for (int i = 0; i < 8; ++i) {
              QProcess process;
              process.start(
                  ffmpeg,
                  {QStringLiteral("-v"), QStringLiteral("error"),
                   QStringLiteral("-ss"), studioTimecode(duration * i / 8),
                   QStringLiteral("-i"), source, QStringLiteral("-frames:v"),
                   QStringLiteral("1"), QStringLiteral("-vf"),
                   QStringLiteral("scale=160:90:force_original_aspect_ratio="
                                  "decrease,pad=160:90:(ow-iw)/2:(oh-ih)/2"),
                   QStringLiteral("-threads"), QStringLiteral("1"),
                   QStringLiteral("-f"), QStringLiteral("image2pipe"),
                   QStringLiteral("-c:v"), QStringLiteral("png"),
                   QStringLiteral("-")});
              if (!process.waitForFinished(5000)) {
                process.kill();
                process.waitForFinished(1000);
                break;
              }
              QImage image;
              if (!image.loadFromData(process.readAllStandardOutput(), "PNG"))
                break;
              thumbnails.push_back(image);
            }
            // Partial sampling must not stretch a clip's head across its
            // entire timeline. Fall back to the plain lane on a failure.
            return thumbnails.size() == 8 ? thumbnails : QVector<QImage>();
          }));
        }
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
            // Not the preview: its clock comes from the frame being painted,
            // and two writers with no ordering between them meant whichever
            // signal arrived last decided the crop.
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
                          : message,
                      true);
            refreshControls();
          });

  preview_->setTrack(&zoom_);
  connect(&thumbnailWatcher_, &QFutureWatcher<QVector<QImage>>::finished, this,
          [this] { timeline_->setThumbnails(thumbnailWatcher_.result()); });
  saveTimer_ = new QTimer(this);
  saveTimer_->setSingleShot(true);
  saveTimer_->setInterval(kSaveDebounceMs);
  connect(saveTimer_, &QTimer::timeout, this, &StudioWindow::saveZoom);
  scrubTimer_ = new QTimer(this);
  scrubTimer_->setSingleShot(true);
  scrubTimer_->setInterval(35);
  connect(scrubTimer_, &QTimer::timeout, this, [this] {
    if (pendingSeek_ >= 0) {
      const qint64 position = pendingSeek_;
      pendingSeek_ = -1;
      seekTo(position);
    }
  });
  // ffprobe rather than the player: the export needs the source's exact
  // frame rate, and a wrong one slides every cue. It also reports the
  // display rotation, which ffmpeg applies before the zoom filter and Qt
  // drops when converting a frame.
  connect(&loadWatcher_, &QFutureWatcher<LoadedSource>::finished, this, [this] {
    const LoadedSource source = loadWatcher_.result();
    media_ = source.media;
    zoom_ = source.zoom;
    style_ = source.style;
    preview_->setStyle(style_);
    initialTrimIn_ = source.in;
    initialTrimOut_ = source.out;
    loaded_ = true;
    preview_->setRotation(media_.rotation);
    sourceLabel_->setText(
        QStringLiteral("%1 × %2\n%3 fps\n\n%4")
            .arg(media_.size.width())
            .arg(media_.size.height())
            .arg(media_.fpsNumerator /
                     static_cast<double>(qMax(1, media_.fpsDenominator)),
                 0, 'f', 2)
            .arg(QFileInfo(path_).fileName()));
    if (!source.error.isEmpty())
      setStatus(source.error, true);
    player_->setSource(QUrl::fromLocalFile(path_));
    refreshControls();
  });
  const QString sourcePath = path_;
  const QString sidecar = zoomSidecarPath();
  loadWatcher_.setFuture(QtConcurrent::run([sourcePath, sidecar] {
    LoadedSource source;
    source.media = probeStudioSource(sourcePath);
    if (QFileInfo(sidecar).isSymLink()) {
      source.error = QStringLiteral("Ignoring a symlinked zoom sidecar");
      return source;
    }
    QFile file(sidecar);
    if (file.open(QIODevice::ReadOnly)) {
      const QJsonObject object =
          QJsonDocument::fromJson(file.readAll()).object();
      if (readZoomTrack(object, source.zoom, source.error)) {
        source.in = object.value(QStringLiteral("trimInMs")).toInteger(0);
        source.out = object.value(QStringLiteral("trimOutMs")).toInteger(-1);
        const QJsonObject style =
            object.value(QStringLiteral("canvas")).toObject();
        source.style.background =
            qBound(0, style.value(QStringLiteral("background")).toInt(), 3);
        source.style.padding =
            qBound(0, style.value(QStringLiteral("padding")).toInt(), 20);
        source.style.radius =
            qBound(0, style.value(QStringLiteral("radius")).toInt(), 64);
      }
    }
    return source;
  }));
  connect(&saveWatcher_, &QFutureWatcher<QString>::finished, this, [this] {
    saving_ = false;
    const QString error = saveWatcher_.result();
    if (!error.isEmpty()) {
      closing_ = false;
      setStatus(error, true);
    }
    if (savePending_) {
      savePending_ = false;
      saveZoom();
    } else if (closing_ && error.isEmpty()) {
      close();
    }
  });
  refreshControls();
}

const ZoomCue *StudioWindow::activeCue() const {
  if (const quint64 selected = timeline_->selectedCue()) {
    for (const ZoomCue &cue : zoom_.cues) {
      if (cue.id == selected)
        return &cue;
    }
  }
  return nullptr;
}

void StudioWindow::zoomChanged() {
  // Normalize what is stored, not just what is rendered: the timeline and
  // the editing lookups read this list, and an overlap otherwise left the
  // lane drawing and editing cue tails no renderer would ever show.
  normalizeZoomTrack(zoom_, player_->duration());
  preview_->setTrack(&zoom_);
  timeline_->update();
  // Coalesced: a drag emits this on every mouse move, and writing the file
  // each time is foreground I/O on a path that may be a network mount.
  saveTimer_->start();
  rememberEdit();
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
  const quint64 id = addZoomCue(zoom_, player_->position(), target,
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

void StudioWindow::setSelectedZoomTiming(bool easeIn, int milliseconds) {
  const ZoomCue *current = activeCue();
  if (!current || !addZoomButton_->isEnabled())
    return;
  for (ZoomCue &cue : zoom_.cues) {
    if (cue.id != current->id)
      continue;
    if (easeIn)
      cue.easeInMs = milliseconds;
    else
      cue.easeOutMs = milliseconds;
    zoomChanged();
    return;
  }
}

void StudioWindow::beginEdit() { editGesture_ = true; }

void StudioWindow::styleChanged() {
  if (restoring_ || !background_->isEnabled())
    return;
  const StudioStyle style{background_->currentIndex(), padding_->value(),
                          radius_->value()};
  if (style == style_)
    return;
  style_ = style;
  preview_->setStyle(style_);
  rememberEdit();
  saveTimer_->start();
  refreshControls();
}

void StudioWindow::endEdit() {
  editGesture_ = false;
  rememberEdit();
  refreshControls();
}

void StudioWindow::rememberEdit() {
  if (restoring_ || editGesture_ || !loaded_ || timeline_->duration() <= 0)
    return;
  const EditState state{zoom_, timeline_->trimIn(), timeline_->trimOut(),
                        style_, timeline_->selectedCue()};
  if (editIndex_ >= 0 && edits_.at(editIndex_) == state)
    return;
  edits_.resize(editIndex_ + 1);
  edits_.push_back(state);
  ++editIndex_;
  if (editIndex_ > 0)
    saveTimer_->start();
}

void StudioWindow::undoEdit() {
  if (!undoButton_->isEnabled() || editGesture_)
    return;
  --editIndex_;
  restoreEdit();
}

void StudioWindow::redoEdit() {
  if (!redoButton_->isEnabled() || editGesture_)
    return;
  ++editIndex_;
  restoreEdit();
}

void StudioWindow::restoreEdit() {
  restoring_ = true;
  const EditState &state = edits_.at(editIndex_);
  zoom_ = state.zoom;
  style_ = state.style;
  preview_->setStyle(style_);
  timeline_->setTrim(state.in, state.out);
  timeline_->setSelectedCue(state.selection);
  preview_->setTrack(&zoom_);
  timeline_->update();
  restoring_ = false;
  saveTimer_->start();
  refreshControls();
}

QString StudioWindow::zoomSidecarPath() const {
  return path_ + QStringLiteral(".omasnap-zoom.json");
}

void StudioWindow::saveZoom() {
  if (!loaded_ || editIndex_ < 0)
    return;
  if (saving_) {
    savePending_ = true;
    return;
  }
  saving_ = true;
  const QString path = zoomSidecarPath();
  QJsonObject object = writeZoomTrack(zoom_);
  object.insert(QStringLiteral("trimInMs"), timeline_->trimIn());
  object.insert(QStringLiteral("trimOutMs"), timeline_->trimOut());
  object.insert(QStringLiteral("canvas"),
                QJsonObject{{QStringLiteral("background"), style_.background},
                            {QStringLiteral("padding"), style_.padding},
                            {QStringLiteral("radius"), style_.radius}});
  saveWatcher_.setFuture(QtConcurrent::run([path, object]() -> QString {
    if (QFileInfo(path).isSymLink())
      return QStringLiteral("Refusing to save through a symlink");
    QSaveFile file(path);
    const QByteArray data =
        QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() ||
        !file.commit())
      return QStringLiteral("Could not save the edits");
    return {};
  }));
}

StudioWindow::~StudioWindow() = default;

void StudioWindow::closeEvent(QCloseEvent *event) {
  if (export_) {
    setStatus(QStringLiteral("Export is still running"));
    event->ignore();
    return;
  }
  if (saving_ || saveTimer_->isActive()) {
    event->ignore();
    closing_ = true;
    player_->pause();
    if (saveTimer_->isActive()) {
      saveTimer_->stop();
      saveZoom();
    }
    return;
  }
  QWidget::closeEvent(event);
}

bool StudioWindow::hasMedia() const { return !mediaFailed_; }

void StudioWindow::setStatus(const QString &status, bool error) {
  statusLabel_->setText(status);
  statusLabel_->setProperty("studioError", error);
  statusLabel_->setStyleSheet(
      QStringLiteral("color: %1;")
          .arg((error ? theme_->chrome().urgent : theme_->chrome().mutedText())
                   .name()));
}

void StudioWindow::refreshControls() {
  const bool exporting = export_ != nullptr;
  const qint64 duration = timeline_->duration();
  const bool trimmed = duration > 0 && (timeline_->trimIn() > 0 ||
                                        timeline_->trimOut() < duration);
  playButton_->setText(player_->playbackState() == QMediaPlayer::PlayingState
                           ? QStringLiteral("Pause")
                           : QStringLiteral("Play"));
  playButton_->setEnabled(!mediaFailed_ && duration > 0);
  resetButton_->setEnabled(trimmed && !exporting);
  // Export is refused rather than quietly producing an unzoomed file: when
  // ffprobe could not read the source there is no frame rate to build the
  // zoom from, and the preview is showing cues the export would drop.
  const bool zoomWouldBeLost = !zoom_.cues.isEmpty() && !media_.usable();
  exportButton_->setEnabled(duration > 0 && !mediaFailed_ && !exporting &&
                            !zoomWouldBeLost);
  timeLabel_->setText(
      QStringLiteral("%1 / %2").arg(studioTimecode(player_->position()).mid(3),
                                    studioTimecode(duration).mid(3)));
  timeLabel_->setToolTip(QStringLiteral("Export range: %1 – %2")
                             .arg(studioTimecode(timeline_->trimIn()),
                                  studioTimecode(timeline_->trimOut())));
  undoButton_->setEnabled(!exporting && editIndex_ > 0);
  redoButton_->setEnabled(!exporting && editIndex_ + 1 < edits_.size());

  const ZoomCue *cue = activeCue();
  // Editing is off while an export runs: ffmpeg already has its expressions,
  // so a cue moved now would change the preview and the sidecar while the
  // file being written keeps the old framing -- and it would still be
  // announced as saved. It is also off when the source could not be probed,
  // because the export cannot apply a zoom it has no frame rate for, and
  // letting cues be built that silently vanish is worse than not offering
  // them.
  const bool editable =
      duration > 0 && !mediaFailed_ && !exporting && media_.usable();
  background_->setEnabled(editable);
  padding_->setEnabled(editable);
  radius_->setEnabled(editable);
  {
    const QSignalBlocker quietBackground(background_);
    background_->setCurrentIndex(style_.background);
    // Signals update the value labels; restoring the model must not create
    // another edit. styleChanged compares the complete state first.
    const bool previous = restoring_;
    restoring_ = true;
    padding_->setValue(style_.padding);
    radius_->setValue(style_.radius);
    restoring_ = previous;
  }
  addZoomButton_->setEnabled(editable);
  removeZoomButton_->setEnabled(editable && cue != nullptr);
  zoomSlider_->setEnabled(editable && cue != nullptr);
  easeIn_->setEnabled(editable && cue != nullptr);
  easeOut_->setEnabled(editable && cue != nullptr);
  cueLabel_->setText(cue ? QStringLiteral("Zoom  %1\n%2 — %3")
                               .arg(cue->id)
                               .arg(studioTimecode(cue->startMs).mid(3),
                                    studioTimecode(cue->endMs).mid(3))
                         : QStringLiteral("No zoom selected"));
  {
    const QSignalBlocker quietIn(easeIn_), quietOut(easeOut_);
    easeIn_->setValue(cue ? static_cast<int>(cue->easeInMs) : 400);
    easeOut_->setValue(cue ? static_cast<int>(cue->easeOutMs) : 400);
  }
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
  if (!media_.usable()) {
    const QString why =
        media_.unsupportedTransform
            ? QStringLiteral("this file is rotated by something other than a "
                             "right angle")
            : QStringLiteral("ffprobe could not read this file");
    zoomLabel_->setText(zoom_.cues.isEmpty()
                            ? QStringLiteral("no zoom: %1").arg(why)
                            : QStringLiteral("cannot export: %1, so these "
                                             "zooms cannot be applied")
                                  .arg(why));
  } else if (exporting)
    zoomLabel_->setText(QStringLiteral("exporting…"));
}

void StudioWindow::togglePlayback() {
  if (pendingSeek_ >= 0)
    seekTo(pendingSeek_);
  if (player_->playbackState() == QMediaPlayer::PlayingState) {
    player_->pause();
    return;
  }
  // Starting outside the kept range would play material the export drops.
  if (player_->position() < timeline_->trimIn() ||
      player_->position() >= timeline_->trimOut())
    seekTo(timeline_->trimIn());
  player_->play();
}

void StudioWindow::seekBy(qint64 milliseconds) {
  seekTo(player_->position() + milliseconds);
}

void StudioWindow::seekTo(qint64 milliseconds) {
  scrubTimer_->stop();
  pendingSeek_ = -1;
  preview_->invalidatePendingFrames();
  player_->setPosition(qBound<qint64>(0, milliseconds, timeline_->duration()));
}

void StudioWindow::stepFrame(int direction) {
  if (!playButton_->isEnabled() || !media_.usable())
    return;
  player_->pause();
  const double frameMs = 1000.0 * media_.fpsDenominator / media_.fpsNumerator;
  const qint64 index =
      qRound64(static_cast<double>(player_->position()) / frameMs);
  seekTo(qRound64(static_cast<double>(index + direction) * frameMs));
}

void StudioWindow::setTrimIn() {
  if (!loaded_ || export_ || timeline_->duration() <= 0)
    return;
  timeline_->setTrim(
      qMin(player_->position(), timeline_->trimOut() - kMinimumTrimMs),
      timeline_->trimOut());
  rememberEdit();
  refreshControls();
}

void StudioWindow::setTrimOut() {
  if (!loaded_ || export_)
    return;
  timeline_->setTrim(
      timeline_->trimIn(),
      qMax(player_->position(), timeline_->trimIn() + kMinimumTrimMs));
  rememberEdit();
  refreshControls();
}

void StudioWindow::resetTrim() {
  if (!loaded_ || export_)
    return;
  timeline_->setTrim(0, timeline_->duration());
  rememberEdit();
  refreshControls();
}

void StudioWindow::startExport() {
  if (!exportButton_->isEnabled() || export_)
    return;
  const QString ffmpeg =
      QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  if (ffmpeg.isEmpty()) {
    setStatus(QStringLiteral("ffmpeg is not installed"), true);
    return;
  }
  const QString destination = studioExportPath(path_);
  const QStringList arguments =
      studioExportArguments(path_, destination, timeline_->trimIn(),
                            timeline_->trimOut(), zoom_, media_, style_);
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
      setStatus(
          QStringLiteral("Saved %1").arg(QFileInfo(destination).fileName()));
    } else {
      QFile::remove(destination);
      setStatus(QStringLiteral("Export failed"), true);
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

bool StudioWindow::handleShortcut(QKeyEvent *event, bool activate) {
  const int key = event->key();
  const auto mods = event->modifiers();
  const bool plain = mods == Qt::NoModifier;
  const bool shift = mods == Qt::ShiftModifier;
  const bool ctrl = mods == Qt::ControlModifier;
  const bool redo = mods == (Qt::ControlModifier | Qt::ShiftModifier);
  std::function<void()> action;
  bool repeat = false;
  if (key == Qt::Key_Space && plain)
    action = [this] {
      if (playButton_->isEnabled())
        togglePlayback();
    };
  else if ((key == Qt::Key_Left || key == Qt::Key_Right) && (plain || shift)) {
    repeat = true;
    action = [this, key, shift] {
      if (!playButton_->isEnabled())
        return;
      player_->pause();
      const int direction = key == Qt::Key_Left ? -1 : 1;
      if (shift)
        seekBy(direction * kSeekStepMs);
      else
        stepFrame(direction);
    };
  } else if (key == Qt::Key_Home && plain)
    action = [this] {
      player_->pause();
      seekTo(timeline_->trimIn());
    };
  else if (key == Qt::Key_End && plain)
    action = [this] {
      player_->pause();
      seekTo(timeline_->trimOut());
    };
  else if (key == Qt::Key_I && plain)
    action = [this] { setTrimIn(); };
  else if (key == Qt::Key_O && plain)
    action = [this] { setTrimOut(); };
  else if (key == Qt::Key_R && plain)
    action = [this] { resetTrim(); };
  else if (key == Qt::Key_Z && plain)
    action = [this] {
      if (addZoomButton_->isEnabled())
        addZoomAtPlayhead();
    };
  else if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && plain)
    action = [this] {
      if (removeZoomButton_->isEnabled())
        removeSelectedZoom();
    };
  else if (key == Qt::Key_Z && ctrl)
    action = [this] { undoEdit(); };
  else if ((key == Qt::Key_Z && redo) || (key == Qt::Key_Y && ctrl))
    action = [this] { redoEdit(); };
  else if (key == Qt::Key_E && ctrl)
    action = [this] { startExport(); };
  else if (key == Qt::Key_S && ctrl)
    action = [this] {
      saveTimer_->stop();
      saveZoom();
    };
  else if (key == Qt::Key_M && plain)
    action = [this] { audio_->setMuted(!audio_->isMuted()); };
  else if (key == Qt::Key_Backslash && ctrl)
    action = [this] { toggleInspector(); };
  else if ((key == Qt::Key_Question && (plain || shift)) ||
           (key == Qt::Key_F1 && plain))
    action = [this] { showShortcuts(); };
  else if (key == Qt::Key_Escape && plain)
    action = [this] {
      timeline_->setSelectedCue(0);
      preview_->setTargetMarker(false);
      refreshControls();
      setFocus();
    };
  else if (key == Qt::Key_W && ctrl)
    action = [this] { close(); };
  if (!action)
    return false;
  if (activate && (repeat || !event->isAutoRepeat()))
    action();
  return true;
}

bool StudioWindow::eventFilter(QObject *object, QEvent *event) {
  auto *widget = qobject_cast<QWidget *>(object);
  if (!widget || widget->window() != this ||
      (event->type() != QEvent::KeyPress &&
       event->type() != QEvent::ShortcutOverride))
    return QWidget::eventFilter(object, event);
  auto *key = static_cast<QKeyEvent *>(event);
  // Numeric/text editing retains its normal cursor/delete/undo behavior.
  // Space still transports from a numeric field, button, tab, or slider.
  const bool textField = qobject_cast<QLineEdit *>(widget) != nullptr;
  if (textField && !(key->key() == Qt::Key_Space &&
                     qobject_cast<QAbstractSpinBox *>(widget->parentWidget())))
    return false;
  if (handleShortcut(key, event->type() == QEvent::KeyPress)) {
    event->accept();
    return true;
  }
  return false;
}

void StudioWindow::keyPressEvent(QKeyEvent *event) {
  if (!handleShortcut(event, true))
    QWidget::keyPressEvent(event);
}

void StudioWindow::showShortcuts() {
  auto *dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(QStringLiteral("Studio shortcuts"));
  dialog->setObjectName(QStringLiteral("studioShortcuts"));
  auto *layout = new QVBoxLayout(dialog);
  layout->setContentsMargins(28, 24, 28, 24);
  auto *label = new QLabel(
      QStringLiteral("Space                 Play / pause\n"
                     "Left / Right          Previous / next frame\n"
                     "Shift + Left / Right  Back / forward 5 seconds\n"
                     "Home / End            Trim start / end\n"
                     "I / O                 Set trim start / end\n"
                     "R                     Reset trim\n"
                     "Z                     Add zoom at playhead\n"
                     "Delete / Backspace    Remove selected zoom\n"
                     "Ctrl + Z              Undo\n"
                     "Ctrl+Shift+Z / Ctrl+Y  Redo\n"
                     "M                     Mute / unmute preview\n"
                     "Ctrl + S              Save edits\n"
                     "Ctrl + E              Export MP4\n"
                     "Ctrl + \\              Show / hide inspector\n"
                     "Escape                Clear selection\n"
                     "Ctrl + W              Close Studio\n"
                     "? / F1                Keyboard shortcuts"),
      dialog);
  label->setFont(chromeMonoFont(13));
  layout->addWidget(label);
  auto *done = new QPushButton(QStringLiteral("Done"), dialog);
  connect(done, &QPushButton::clicked, dialog, &QDialog::accept);
  layout->addWidget(done);
  dialog->open();
}

void StudioWindow::applyChrome() {
  const StudioChrome &chrome = theme_->chrome();
  setStyleSheet(chrome.styleSheet());
  preview_->setChrome(chrome);
  timeline_->setChrome(chrome);
  if (background_)
    background_->setChrome(chrome);
  if (statusLabel_)
    setStatus(statusLabel_->text(),
              statusLabel_->property("studioError").toBool());
  update();
}

void StudioWindow::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.fillRect(rect(), theme_->chrome().background);
}

void StudioWindow::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  if (!inspector_)
    return;
  inspector_->setVisible(inspectorWanted_ && width() >= 960);
  fileLabel_->setVisible(width() >= 1100);
  shortcutLegend_->setText(
      width() >= 1150 ? QStringLiteral("SPACE  Play / pause     ← →  Frame     "
                                       "SHIFT + ← →  5 seconds     Z  Add zoom")
                      : QStringLiteral("SPACE  Play / pause     ← →  Frame     "
                                       "Z  Zoom     ?  Shortcuts"));
}

void StudioWindow::toggleInspector() {
  inspectorWanted_ = !inspector_->isVisible();
  inspector_->setVisible(inspectorWanted_);
}
