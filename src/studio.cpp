/** @fileoverview The Studio window and its trim timeline. */
#include "studio.hpp"

#include "overlay-chrome.hpp"
#include "studio-composition.hpp"
#include "studio-export.hpp"
#include "studio-playback.hpp"
#include "studio-preview.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QAudioOutput>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMenu>
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
#include <QScrollBar>
#include <QWheelEvent>
#include <QtConcurrentRun>

#include <cmath>
#include <algorithm>
#include <limits>

namespace {

constexpr int kTimelineHeight = 176;
// True while typing in a value field, whether the key event targets the
// field itself or an ancestor it bubbled to after the field ignored it.
// Spinbox focus lands on the spinbox rather than its inner line edit, so
// checking QLineEdit alone leaks keys into video edits either way.
bool studioHotkeysSuspended(QObject *target) {
  const auto isInput = [](QWidget *candidate) {
    return qobject_cast<QLineEdit *>(candidate) != nullptr ||
           qobject_cast<QAbstractSpinBox *>(candidate) != nullptr ||
           qobject_cast<QComboBox *>(candidate) != nullptr;
  };
  return isInput(qobject_cast<QWidget *>(target)) ||
         isInput(QApplication::focusWidget());
}
class TimelineActionButton final : public QPushButton {
public:
  enum class Symbol { Split, Delete, Keep, Play, Pause, SoundOn, SoundOff };
  TimelineActionButton(Symbol symbol, const QString &name, const QString &hint,
                       StudioTheme *theme, QWidget *parent)
      : QPushButton(parent), symbol_(symbol), theme_(theme) {
    setAccessibleName(name);
    setToolTip(hint);
    setFixedSize(28, 28);
  }
  void setSymbol(Symbol symbol) {
    if (symbol_ != symbol) {
      symbol_ = symbol;
      update();
    }
  }
  [[nodiscard]] Symbol symbol() const { return symbol_; }
protected:
  void paintEvent(QPaintEvent *event) override {
    QPushButton::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate((width() - 18) / 2.0, (height() - 18) / 2.0);
    painter.scale(18.0 / 24.0, 18.0 / 24.0);
    const QColor color = isEnabled() ? theme_->chrome().foreground
                                     : theme_->chrome().mutedText();
    painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.setBrush(Qt::NoBrush);
    if (symbol_ == Symbol::Split) {
      painter.drawRect(QRectF(2, 7, 6, 13));
      painter.drawRect(QRectF(16, 7, 6, 13));
      painter.drawLine(QPointF(12, 5), QPointF(12, 22));
      painter.drawLine(QPointF(9, 2), QPointF(12, 5));
      painter.drawLine(QPointF(15, 2), QPointF(12, 5));
    } else if (symbol_ == Symbol::Delete) {
      painter.drawLine(QPointF(3, 6), QPointF(21, 6));
      painter.drawRect(QRectF(9, 3, 6, 3));
      painter.drawRect(QRectF(6, 6, 12, 15));
      painter.drawLine(QPointF(10, 10), QPointF(10, 17));
      painter.drawLine(QPointF(14, 10), QPointF(14, 17));
    } else if (symbol_ == Symbol::Play) {
      QPainterPath triangle;
      triangle.moveTo(8, 5);
      triangle.lineTo(18, 12);
      triangle.lineTo(8, 19);
      triangle.closeSubpath();
      painter.setBrush(color);
      painter.setPen(Qt::NoPen);
      painter.drawPath(triangle);
    } else if (symbol_ == Symbol::Pause) {
      painter.setBrush(color);
      painter.setPen(Qt::NoPen);
      painter.drawRect(QRectF(7, 5, 4, 14));
      painter.drawRect(QRectF(13, 5, 4, 14));
    } else if (symbol_ == Symbol::SoundOn || symbol_ == Symbol::SoundOff) {
      QPainterPath speaker;
      speaker.moveTo(4, 9);
      speaker.lineTo(8, 9);
      speaker.lineTo(13, 4);
      speaker.lineTo(13, 20);
      speaker.lineTo(8, 15);
      speaker.lineTo(4, 15);
      speaker.closeSubpath();
      painter.setBrush(color);
      painter.setPen(Qt::NoPen);
      painter.drawPath(speaker);
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      if (symbol_ == Symbol::SoundOn) {
        painter.drawArc(QRectF(14, 8, 4, 8), -60 * 16, 120 * 16);
        painter.drawArc(QRectF(14, 5, 8, 14), -60 * 16, 120 * 16);
      } else {
        painter.drawLine(QPointF(15, 9), QPointF(21, 15));
        painter.drawLine(QPointF(21, 9), QPointF(15, 15));
      }
    } else {
      painter.drawEllipse(QPointF(6, 6), 3, 3);
      painter.drawEllipse(QPointF(6, 18), 3, 3);
      painter.drawLine(QPointF(8.2, 8.2), QPointF(21, 21));
      painter.drawLine(QPointF(8.2, 15.8), QPointF(21, 3));
    }
  }
private:
  Symbol symbol_;
  StudioTheme *theme_;
};
// View-only magnification: clip/selection coordinates continue to use the
// same timeline time map. No decoder or project state changes on zoom/pan.
class TimelineViewport final : public QScrollArea {
public:
  explicit TimelineViewport(StudioTimeline *timeline) : timeline_(timeline) {
    setObjectName(QStringLiteral("timelineViewport"));
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFixedHeight(kTimelineHeight + 24);
    setWidget(timeline);
    timeline->installEventFilter(this);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { updateThumbnailViewport(); });
  }
  void zoomBy(double factor, int anchor = -1) {
    if (anchor < 0)
      anchor = viewport()->width() / 2;
    const double oldWidth = qMax(1, timeline_->width() - 80);
    const double fraction = (horizontalScrollBar()->value() + anchor - 64) / oldWidth;
    zoom_ = qBound(1.0, zoom_ * factor, 16.0);
    resizeTimeline();
    horizontalScrollBar()->setValue(qRound(64 + fraction * (timeline_->width() - 80) - anchor));
  }
protected:
  void resizeEvent(QResizeEvent *event) override {
    QScrollArea::resizeEvent(event);
    resizeTimeline();
  }
  void wheelEvent(QWheelEvent *event) override {
    const QPoint delta = event->pixelDelta().isNull() ? event->angleDelta() : event->pixelDelta();
    const int amount = delta.y() ? delta.y() : delta.x();
    if (amount)
      zoomBy(std::pow(1.25, amount / 120.0),
             viewport()->mapFromGlobal(event->globalPosition().toPoint()).x());
    event->accept();
  }
  bool eventFilter(QObject *object, QEvent *event) override {
    if (object != timeline_)
      return QScrollArea::eventFilter(object, event);
    if (event->type() == QEvent::Wheel) {
      wheelEvent(static_cast<QWheelEvent *>(event));
      return true;
    }
    if (event->type() == QEvent::ContextMenu) {
      const auto *menu = static_cast<QContextMenuEvent *>(event);
      if (menu->reason() == QContextMenuEvent::Mouse && !openingMenu_)
        return true; // Open only on a completed right-click, never on a pan.
    }
    if (event->type() == QEvent::MouseButtonPress) {
      auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::RightButton) {
        panPressed_ = true;
        panDragged_ = false;
        panOrigin_ = mouse->globalPosition().toPoint();
        panScroll_ = horizontalScrollBar()->value();
        return true;
      }
    }
    if (event->type() == QEvent::MouseMove && panPressed_) {
      auto *mouse = static_cast<QMouseEvent *>(event);
      const QPoint delta = mouse->globalPosition().toPoint() - panOrigin_;
      panDragged_ = panDragged_ || delta.manhattanLength() >= QApplication::startDragDistance();
      if (panDragged_) {
        timeline_->setCursor(Qt::ClosedHandCursor);
        horizontalScrollBar()->setValue(panScroll_ - delta.x());
      }
      return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && panPressed_) {
      auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::RightButton) {
        panPressed_ = false;
        timeline_->unsetCursor();
        if (!panDragged_) {
          openingMenu_ = true;
          QContextMenuEvent menu(QContextMenuEvent::Mouse, mouse->position().toPoint(),
                                 mouse->globalPosition().toPoint());
          QApplication::sendEvent(timeline_, &menu);
          openingMenu_ = false;
        }
        return true;
      }
    }
    return QScrollArea::eventFilter(object, event);
  }
private:
  void resizeTimeline() {
    timeline_->setFixedSize(qMax(80, qRound((viewport()->width() - 80) * zoom_ + 80)),
                            kTimelineHeight);
    updateThumbnailViewport();
  }
  void updateThumbnailViewport() {
    timeline_->setThumbnailViewport(QRectF(horizontalScrollBar()->value(), 0,
                                            viewport()->width(), kTimelineHeight));
  }
  StudioTimeline *timeline_;
  double zoom_ = 1;
  QPoint panOrigin_;
  int panScroll_ = 0;
  bool panPressed_ = false;
  bool panDragged_ = false;
  bool openingMenu_ = false;
};
/// The trim bar's row, then the cue lane under it.
constexpr qreal kLaneGap = 10.0;
constexpr qreal kCueLaneHeight = 30.0;
constexpr qreal kCueEdgeGrab = 6.0;
constexpr qreal kTrackHeight = 44.0;
/// Pointer slack around a handle, in pixels. A 8 px bar is a small target.
constexpr qreal kGrabSlack = 7.0;
constexpr qint64 kSeekStepMs = 5000;
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
  QProcess process;
  process.start(probe, {QStringLiteral("-v"), QStringLiteral("error"),
                        QStringLiteral("-show_streams"),
                        QStringLiteral("-show_format"), QStringLiteral("-of"),
                        QStringLiteral("json"), path});
  if (!process.waitForFinished(kProbeTimeoutMs)) {
    process.kill();
    process.waitForFinished(1000);
    return media;
  }
  if (process.exitCode() != 0)
    return media;
  const auto root =
      QJsonDocument::fromJson(process.readAllStandardOutput()).object();
  bool foundVideo = false;
  for (const auto &value : root.value(QStringLiteral("streams")).toArray()) {
    const auto stream = value.toObject();
    const auto kind = stream.value(QStringLiteral("codec_type")).toString();
    if (kind == QStringLiteral("audio")) {
      ++media.audioStreams;
      continue;
    }
    if (kind != QStringLiteral("video") || foundVideo)
      continue;
    foundVideo = true;
    media.size = QSize(stream.value(QStringLiteral("width")).toInt(),
                       stream.value(QStringLiteral("height")).toInt());
    const auto rate =
        stream.value(QStringLiteral("r_frame_rate")).toString().split('/');
    media.fpsNumerator = rate.value(0).toInt();
    media.fpsDenominator = qMax(1, rate.value(1).toInt());
    for (const auto &data :
         stream.value(QStringLiteral("side_data_list")).toArray()) {
      const auto side = data.toObject();
      if (!side.contains(QStringLiteral("rotation")))
        continue;
      const double degrees = side.value(QStringLiteral("rotation")).toDouble();
      const double snapped = qRound(degrees / 90.0) * 90.0;
      media.unsupportedTransform = std::abs(degrees - snapped) > 0.5;
      media.rotation = ((static_cast<int>(-snapped) % 360) + 360) % 360;
    }
  }
  media.durationMs = qRound64(root.value(QStringLiteral("format"))
                                  .toObject()
                                  .value(QStringLiteral("duration"))
                                  .toString()
                                  .toDouble() *
                              1000);
  if (media.rotation % 180)
    media.size.transpose();
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
  QString candidate =
      directory.filePath(stem + QStringLiteral("-omasnap-exported.mp4"));
  for (int index = 2; QFileInfo::exists(candidate) && index < 1000; ++index)
    candidate = directory.filePath(
        QStringLiteral("%1-omasnap-exported-%2.mp4").arg(stem).arg(index));
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
  if (id) {
    selectedClip_ = 0;
    rangeIn_ = rangeOut_ = -1;
  }
  if (selected_ == id)
    return;
  selected_ = id;
  update();
}

void StudioTimeline::setRange(qint64 start, qint64 end) {
  if (start < 0 || end < 0)
    rangeIn_ = rangeOut_ = -1;
  else {
    rangeIn_ = qBound<qint64>(0, qMin(start, end), duration_);
    rangeOut_ = qBound<qint64>(0, qMax(start, end), duration_);
    selected_ = selectedClip_ = 0;
  }
  update();
  emit selectionChanged();
}

void StudioTimeline::setSelectedClip(quint64 id) {
  selectedClip_ = id;
  if (id) {
    selected_ = 0;
    rangeIn_ = rangeOut_ = -1;
  }
  update();
  emit selectionChanged();
}

void StudioTimeline::clearSelection() {
  grabbed_ = Grab::None;
  draggedScenePreview_ = {};
  showInsertion(0, false);
  unsetCursor();
  selected_ = selectedClip_ = 0;
  rangeIn_ = rangeOut_ = -1;
  update();
  emit selectionChanged();
}

quint64 StudioTimeline::insertionBefore(qreal x) const {
  if (project_)
    for (const auto &span : studioComposition(*project_))
      if (x < xForTime((span.startMs + span.endMs) / 2))
        return span.clipId;
  return 0;
}

void StudioTimeline::showInsertion(quint64 before, bool visible) {
  insertionBefore_ = before;
  insertionVisible_ = visible;
  update();
}

void StudioTimeline::contextMenuEvent(QContextMenuEvent *event) {
  auto *menu = new QMenu(this);
  menu->setAttribute(Qt::WA_DeleteOnClose);
  auto *split = menu->addAction(QStringLiteral("Split at playhead  ·  S"));
  auto *remove = menu->addAction(QStringLiteral("Delete selection"));
  split->setEnabled(cuesEditable_ && duration_ > 0);
  remove->setEnabled(cuesEditable_ &&
                     (rangeOut_ > rangeIn_ || selectedClip_ || selected_));
  connect(split, &QAction::triggered, this, &StudioTimeline::splitRequested);
  connect(remove, &QAction::triggered, this, &StudioTimeline::deleteRequested);
  menu->popup(event->globalPos());
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

QRectF StudioTimeline::transitionRect(const StudioSpan &outgoing,
                                      const StudioSpan &incoming) const {
  const qreal start = xForTime(incoming.startMs);
  const qreal end = xForTime(outgoing.endMs);
  const qreal width = qMax<qreal>(24, end - start);
  return {(start + end - width) / 2, trackRect().bottom() - 14, width, 14};
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
  if (!cuesEditable_)
    return Grab::Playhead;
  if (trackRect().contains(position) && hasRange()) {
    if (rangeIn_ >= 0 &&
        std::abs(position.x() - xForTime(rangeIn_)) <= kGrabSlack)
      return Grab::RangeStart;
    if (rangeOut_ > rangeIn_ &&
        std::abs(position.x() - xForTime(rangeOut_)) <= kGrabSlack)
      return Grab::RangeEnd;
    return Grab::Playhead;
  }
  if (trackRect().contains(position) && selectedClip_ && project_)
    for (const auto &span : studioComposition(*project_))
      if (span.clipId == selectedClip_) {
        if (std::abs(position.x() - xForTime(span.startMs)) <= kGrabSlack)
          return Grab::SceneStart;
        if (std::abs(position.x() - xForTime(span.endMs)) <= kGrabSlack)
          return Grab::SceneEnd;
      }
  // The cue lane is its own row, so a click there never means the playhead.
  if (cueLaneRect().contains(position)) {
    Grab edge = Grab::CueBody;
    return cueAt(position, &edge) != 0 ? edge : Grab::None;
  }
  // Unselected video and ruler positions always scrub.
  return Grab::Playhead;
}

void StudioTimeline::setDuration(qint64 milliseconds) {
  duration_ = qMax<qint64>(0, milliseconds);
  trimIn_ = 0;
  trimOut_ = duration_;
  update();
}

void StudioTimeline::setPosition(qint64 milliseconds) {
  const qint64 clamped = hasRange()
      ? qBound(rangeIn_, milliseconds, rangeOut_ - 1)
      : qBound<qint64>(0, milliseconds, duration_);
  if (clamped == position_)
    return;
  position_ = clamped;
  update();
}

void StudioTimeline::cacheThumbnail(StudioThumbnail thumbnail) {
  for (qsizetype i = 0; i < thumbnails_.size(); ++i)
    if (thumbnails_[i].path == thumbnail.path && thumbnails_[i].sourceMs == thumbnail.sourceMs) {
      thumbnails_.removeAt(i);
      break;
    }
  // 512 fixed 160x90 RGBA images: at most about 28 MiB, shared by duplicate clips.
  if (thumbnails_.size() >= 512)
    thumbnails_.removeFirst();
  thumbnails_.push_back(std::move(thumbnail));
  update();
}

void StudioTimeline::setThumbnailViewport(const QRectF &viewport) {
  thumbnailViewport_ = viewport;
  // Width changes can alter the time-to-pixel scale even when this rectangle
  // stays unchanged (zooming at the left edge).
  emit thumbnailViewChanged();
}

QVector<StudioTimeline::ThumbnailTile> StudioTimeline::thumbnailTiles(const QRectF &region) const {
  QVector<ThumbnailTile> tiles;
  if (!project_ || duration_ <= 0)
    return tiles;
  const QRectF track = trackRect();
  constexpr qreal tileWidth = kTrackHeight * 16.0 / 9.0;
  for (const auto &span : studioComposition(*project_)) {
    const auto *asset = studioAsset(*project_, span.assetId);
    if (!asset)
      continue;
    const QRectF scene(xForTime(span.startMs), track.top(),
                       xForTime(span.endMs) - xForTime(span.startMs), track.height());
    const QRectF visible = scene.intersected(region);
    if (visible.isEmpty())
      continue;
    const int first = qMax(0, int(std::floor((visible.left() - scene.left()) / tileWidth)));
    for (int i = first; scene.left() + i * tileWidth < visible.right(); ++i) {
      const QRectF tile(scene.left() + i * tileWidth, track.top(), tileWidth, track.height());
      const qint64 time = qBound(span.startMs, timeForX(qMin(tile.center().x(), scene.right())), span.endMs - 1);
      // Quantized source keys remain reusable during small resize/zoom steps.
      const qint64 source = qBound(span.inMs,
          (span.inMs + qRound64((time - span.startMs) * span.speed)) / 100 * 100,
          span.outMs - 1);
      tiles.push_back({tile, scene, asset->path, source, span.inMs, span.outMs});
    }
  }
  return tiles;
}

const QImage *StudioTimeline::thumbnailFor(const ThumbnailTile &tile) const {
  const QImage *nearest = nullptr;
  qint64 distance = std::numeric_limits<qint64>::max();
  for (const auto &entry : thumbnails_) {
    if (entry.path != tile.path || entry.image.isNull() ||
        entry.sourceMs < tile.inMs || entry.sourceMs >= tile.outMs)
      continue;
    const qint64 delta = qAbs(entry.sourceMs - tile.sourceMs);
    if (delta < distance) {
      distance = delta;
      nearest = &entry.image;
    }
  }
  return nearest;
}

QVector<StudioThumbnail> StudioTimeline::missingThumbnails() const {
  QVector<StudioThumbnail> missing;
  const auto tiles = thumbnailTiles(thumbnailViewport_.isEmpty() ? trackRect() : thumbnailViewport_);
  QSet<QString> seen;
  for (const auto &tile : tiles) {
    if (seen.size() >= 256)
      break; // Keep the visible working set smaller than the bounded cache.
    const QString key = tile.path + QChar(0) + QString::number(tile.sourceMs);
    if (seen.contains(key))
      continue;
    seen.insert(key);
    const bool cached = std::any_of(thumbnails_.cbegin(), thumbnails_.cend(), [&](const auto &entry) {
      return entry.path == tile.path && entry.sourceMs == tile.sourceMs;
    });
    if (!cached)
      missing.push_back({tile.path, tile.sourceMs, {}});
  }
  return missing;
}

void StudioTimeline::paintThumbnails(QPainter &painter, const QRectF &region) const {
  for (const auto &tile : thumbnailTiles(region)) {
    const auto *image = thumbnailFor(tile);
    if (!image)
      continue;
    painter.save();
    painter.setClipRect(tile.clip.intersected(region), Qt::IntersectClip);
    // Tile geometry stays 16:9 at every timeline zoom. The final tile is
    // clipped by its clip boundary, never stretched to fill the remainder.
    painter.drawImage(tile.rect, *image);
    painter.restore();
  }
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
  const bool selectClip = cuesEditable_ && event->modifiers() == Qt::ControlModifier &&
                          trackRect().contains(event->position());
  const bool shiftRange = cuesEditable_ && event->modifiers().testFlag(Qt::ShiftModifier) &&
                          event->position().y() < cueLaneRect().top();
  if (project_ && cuesEditable_ && !hasRange() && !shiftRange && !selectClip) {
    const auto spans = studioComposition(*project_);
    for (qsizetype i = 0; i + 1 < project_->clips.size(); ++i)
      if (transitionRect(spans[i], spans[i + 1]).contains(event->position())) {
        setSelectedClip(project_->clips[i].id);
        emit transitionRequested(project_->clips[i].id);
        return;
      }
  }
  emit editStarted();
  grabbed_ = grabAt(event->position());
  if (selectClip)
    grabbed_ = Grab::Playhead;
  if (shiftRange)
    grabbed_ = Grab::RangeNew;
  if (grabbed_ == Grab::RangeNew || grabbed_ == Grab::RangeStart || grabbed_ == Grab::RangeEnd)
    emit rangeSelectionStarted();
  scenePress_ = event->position();
  sceneDragOffset_ = {};
  draggedScenePreview_ = {};
  reorderGesture_ = event->modifiers().testFlag(Qt::ControlModifier);
  sceneDurationMs_ = duration_;
  grabbedScene_ = {};
  if (project_ && trackRect().contains(event->position())) {
    quint64 id = selectedClip_;
    if (grabbed_ == Grab::Playhead)
      if (const auto frame = studioFrameAt(
              *project_, qMin(duration_ - 1, timeForX(event->position().x()))))
        id = frame->span.clipId;
    for (const auto &clip : project_->clips)
      if (clip.id == id)
        grabbedScene_ = clip;
    for (const auto &span : studioComposition(*project_))
      if (span.clipId == id)
        sceneStartMs_ = span.startMs;
  }
  if (grabbed_ == Grab::RangeNew) {
    rangeAnchor_ = timeForX(event->position().x());
    setRange(rangeAnchor_, rangeAnchor_);
    return;
  }
  if (grabbed_ == Grab::RangeStart)
    rangeAnchor_ = rangeOut_;
  if (grabbed_ == Grab::RangeEnd)
    rangeAnchor_ = rangeIn_;
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
  if (grabbed_ == Grab::Playhead && project_ && selectClip) {
    if (const auto frame = studioFrameAt(
            *project_, qMin(duration_ - 1, timeForX(event->position().x()))))
      setSelectedClip(frame->span.clipId);
  }
  mouseMoveEvent(event);
}

void StudioTimeline::mouseMoveEvent(QMouseEvent *event) {
  if (grabbed_ == Grab::None) {
    const Grab hover = grabAt(event->position());
    if (hover != hovered_) {
      hovered_ = hover;
      const bool resizes = hover == Grab::CueStart || hover == Grab::CueEnd ||
                           hover == Grab::RangeStart ||
                           hover == Grab::RangeEnd ||
                           hover == Grab::SceneStart || hover == Grab::SceneEnd;
      setCursor(resizes                  ? Qt::SizeHorCursor
                : hover == Grab::CueBody ? Qt::OpenHandCursor
                                         : Qt::ArrowCursor);
      update();
    }
    return;
  }
  qint64 time = timeForX(event->position().x());
  if (grabbed_ == Grab::Playhead && rangeIn_ >= 0 &&
      !event->modifiers().testFlag(Qt::AltModifier)) {
    for (const qint64 boundary : {rangeIn_, rangeOut_})
      if (std::abs(event->position().x() - xForTime(boundary)) <= kGrabSlack) {
        time = boundary;
        break;
      }
  }
  if (grabbed_ == Grab::Playhead && reorderGesture_ && cuesEditable_ &&
      grabbedScene_.id &&
      (event->position() - scenePress_).manhattanLength() >=
          QApplication::startDragDistance()) {
    draggedSceneRect_ = QRectF(
        xForTime(sceneStartMs_), trackRect().top(),
        trackRect().width() *
            (grabbedScene_.outMs - grabbedScene_.inMs) /
            grabbedScene_.speed / sceneDurationMs_,
        trackRect().height());
    // Build only the small card, once; pointer moves merely blit it. Do not
    // capture the playhead or transition badges into the floating clip.
    draggedScenePreview_ = QPixmap((draggedSceneRect_.size() * devicePixelRatioF()).toSize());
    draggedScenePreview_.setDevicePixelRatio(devicePixelRatioF());
    draggedScenePreview_.fill(chrome_.background);
    QPainter card(&draggedScenePreview_);
    card.translate(-draggedSceneRect_.topLeft());
    paintThumbnails(card, draggedSceneRect_);
    card.end();
    grabbed_ = Grab::SceneMove;
    setCursor(Qt::ClosedHandCursor);
  }
  if (grabbed_ == Grab::SceneMove) {
    sceneDragOffset_ = event->position() - scenePress_;
    showInsertion(insertionBefore(event->position().x()), true);
    return;
  }
  if ((grabbed_ == Grab::SceneStart || grabbed_ == Grab::SceneEnd) &&
      project_) {
    const auto *asset = studioAsset(*project_, grabbedScene_.assetId);
    if (!asset)
      return;
    const qint64 source =
        (grabbed_ == Grab::SceneStart ? grabbedScene_.inMs
                                      : grabbedScene_.outMs) +
        qRound64((event->position().x() - scenePress_.x()) * sceneDurationMs_ /
                 trackRect().width() * grabbedScene_.speed);
    emit sceneTrimRequested(
        grabbedScene_.id,
        grabbed_ == Grab::SceneStart
            ? qBound<qint64>(0, source, grabbedScene_.outMs - 1)
            : grabbedScene_.inMs,
        grabbed_ == Grab::SceneEnd
            ? qBound<qint64>(grabbedScene_.inMs + 1, source,
                             asset->source.durationMs)
            : grabbedScene_.outMs);
    return;
  }
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
  case Grab::RangeNew:
  case Grab::RangeStart:
  case Grab::RangeEnd:
    setRange(rangeAnchor_, time);
    break;
  case Grab::Playhead:
    if (reorderGesture_ && grabbedScene_.id)
      break; // Ctrl+click selects; only a plain drag scrubs.
    setPosition(time);
    emit scrubbed(position_);
    break;
  case Grab::None:
  case Grab::CueBody:
  case Grab::CueStart:
  case Grab::CueEnd:
  case Grab::SceneStart:
  case Grab::SceneEnd:
  case Grab::SceneMove:
    break;
  }
}

void StudioTimeline::mouseReleaseEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton)
    return;
  const bool selectingRange = grabbed_ == Grab::RangeNew || grabbed_ == Grab::RangeStart || grabbed_ == Grab::RangeEnd;
  if (selectingRange)
    mouseMoveEvent(event);
  if (grabbed_ == Grab::Playhead &&
      timeForX(event->position().x()) != position_)
    mouseMoveEvent(event);
  if (grabbed_ == Grab::SceneMove)
    emit sceneMoveRequested(grabbedScene_.id, insertionBefore_);
  showInsertion(0, false);
  grabbed_ = Grab::None;
  grabbedCue_ = 0;
  draggedScenePreview_ = {};
  unsetCursor();
  emit editFinished();
  if (selectingRange && hasRange()) {
    setPosition(rangeIn_);
    emit rangeSelected();
  }
}

void StudioTimeline::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const QRectF track = trackRect();
  painter.fillRect(rect(), chrome_.background);
  painter.setFont(chromeMonoFont(10));
  painter.setPen(chrome_.mutedText());
  const int ticks = qMax(1, qRound(track.width() / 100));
  for (int tick = 0; tick <= ticks; ++tick) {
    const qreal x = track.left() + track.width() * tick / ticks;
    painter.drawLine(QPointF(x, 26), QPointF(x, 31));
    painter.drawText(QRectF(qBound(0.0, x - 45, qMax(0.0, width() - 90.0)), 4, 90, 18), Qt::AlignCenter,
                     studioTimecode(duration_ * tick / ticks).mid(3));
  }
  painter.setFont(chromeMonoFont(10));
  painter.drawText(QRectF(0, track.top(), 55, track.height()),
                   Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("VIDEO"));

  painter.setPen(Qt::NoPen);
  painter.setBrush(chrome_.surface());
  painter.drawRect(track);
  paintThumbnails(painter, thumbnailViewport_.isEmpty() ? track : thumbnailViewport_);

  if (duration_ <= 0)
    return;

  painter.setPen(chrome_.foreground);
  painter.setFont(chromeMonoFont(11));
  if (project_) {
    for (const auto &span : studioComposition(*project_)) {
      const QRectF scene(xForTime(span.startMs), track.top(),
                         xForTime(span.endMs) - xForTime(span.startMs),
                         track.height());
      painter.save();
      painter.setClipRect(scene);
      painter.setPen(QPen(
          span.clipId == selectedClip_ ? chrome_.accent : chrome_.border(), 1));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(scene.adjusted(0.5, 0.5, -0.5, -0.5));
      if (span.clipId == selectedClip_) {
        painter.fillRect(QRectF(scene.left(), scene.top(), 3, scene.height()),
                         chrome_.accent);
        painter.fillRect(
            QRectF(scene.right() - 3, scene.top(), 3, scene.height()),
            chrome_.accent);
      }
      painter.restore();
    }
  } else
    painter.drawText(track.adjusted(14, 0, -14, 0), Qt::AlignVCenter,
                     QStringLiteral("Screen recording"));
  if (rangeIn_ >= 0 && rangeOut_ >= rangeIn_) {
    const QRectF range(xForTime(rangeIn_), track.top() - 3,
                       qMax<qreal>(1, xForTime(rangeOut_) - xForTime(rangeIn_)),
                       track.height() + 6);
    QColor selection = chrome_.accent;
    selection.setAlphaF(0.3);
    painter.fillRect(range, selection);
    painter.setPen(QPen(chrome_.accent, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(range);
    painter.fillRect(QRectF(range.left() - 3, range.top(), 6, range.height()),
                     chrome_.accent);
    painter.fillRect(QRectF(range.right() - 3, range.top(), 6, range.height()),
                     chrome_.accent);
  }
  if (insertionVisible_) {
    qint64 at = duration_;
    if (project_)
      for (const auto &span : studioComposition(*project_))
        if (span.clipId == insertionBefore_)
          at = span.startMs;
    const qreal x = xForTime(at);
    painter.fillRect(QRectF(x - 2, track.top() - 8, 4, track.height() + 16),
                     chrome_.accent);
  }
  painter.setPen(Qt::NoPen);


  if (project_) {
    painter.setFont(chromeMonoFont(9));
    const auto spans = studioComposition(*project_);
    QHash<quint64, const StudioTransition *> transitions;
    for (const auto &entry : project_->transitions)
      transitions.insert(entry.outgoingClipId, &entry);
    for (qsizetype i = 0; i + 1 < project_->clips.size(); ++i) {
      const auto *transition =
          transitions.value(project_->clips[i].id, nullptr);
      const QRectF badge = transitionRect(spans[i], spans[i + 1]);
      painter.fillRect(badge, transition ? chrome_.accent : chrome_.background);
      painter.setPen(QPen(chrome_.foreground, 1));
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(badge);
      painter.setPen(transition ? chrome_.onAccent() : chrome_.foreground);
      painter.drawText(badge, Qt::AlignCenter,
                       !transition ? QStringLiteral("+")
                       : transition->kind == StudioTransitionKind::Crossfade
                           ? QStringLiteral("F")
                       : transition->kind == StudioTransitionKind::FadeBlack
                           ? QStringLiteral("B")
                       : studioTransitionName(transition->kind)
                               .startsWith(QStringLiteral("wipe-"))
                           ? QStringLiteral("W")
                           : QStringLiteral("S"));
    }
  }

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
  if (grabbed_ == Grab::SceneMove && !draggedScenePreview_.isNull()) {
    painter.fillRect(draggedSceneRect_, chrome_.background);
    painter.setPen(QPen(chrome_.mutedText(), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(draggedSceneRect_.adjusted(1, 1, -1, -1));
    const QRectF floating = draggedSceneRect_.translated(sceneDragOffset_ + QPointF(0, -8));
    painter.fillRect(floating.translated(3, 3), chrome_.background);
    painter.drawPixmap(floating, draggedScenePreview_, QRectF(draggedScenePreview_.rect()));
    painter.setPen(QPen(chrome_.accent, 2));
    painter.drawRect(floating);
  }
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
  timeline_->setProject(&project_);
  applyChrome();
  connect(theme_, &StudioTheme::changed, this, &StudioWindow::applyChrome);

  const auto button = [this](const QString &text, const QString &hint) {
    auto *result = new QPushButton(text, this);
    result->setToolTip(hint);
    result->setAccessibleName(hint);
    return result;
  };
  playButton_ = new TimelineActionButton(
      TimelineActionButton::Symbol::Play, QStringLiteral("Play / pause"),
      QStringLiteral("Play / pause · Space"), theme_, this);
  playButton_->setObjectName(QStringLiteral("play"));
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
  relinkButton_ = button(QStringLiteral("Relink media"),
                         QStringLiteral("Locate a missing project source"));
  relinkButton_->hide();
  headerLayout->addWidget(relinkButton_);
  connect(relinkButton_, &QPushButton::clicked, this,
          &StudioWindow::relinkAsset);
  connect(&relinkWatcher_, &QFutureWatcher<StudioProjectLoad>::finished, this,
          [this] {
            relinking_ = false;
            const auto result = relinkWatcher_.result();
            if (!result.error.isEmpty()) {
              setStatus(result.error, true);
              refreshControls();
              return;
            }
            project_ = result.project;
            for (const auto &asset : project_.assets) {
              if (result.missingAssets.contains(asset.id))
                missingPaths_.insert(asset.path);
              else
                missingPaths_.remove(asset.path);
            }
            missingAssets_ = result.missingAssets;
            applyProject();
            rememberEdit();
            setStatus(
                missingAssets_.isEmpty()
                    ? QStringLiteral("Media relinked")
                    : QStringLiteral("Relink the remaining missing sources"));
          });

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
  clipPage->setObjectName(QStringLiteral("studioScenePage"));
  auto *clipControls = new QVBoxLayout(clipPage);
  clipControls->setContentsMargins(0, 22, 0, 0);
  clipControls->setSpacing(14);
  sourceLabel_ = new QLabel(QStringLiteral("Reading recording…"), clipPage);
  sourceLabel_->setWordWrap(true);
  sourceLabel_->setFont(chromeMonoFont(12));
  clipControls->addWidget(sourceLabel_);
  setupScenes(clipControls);
  auto *clipHint = new QLabel(
      QStringLiteral("Drag to scrub · Ctrl+drag to rearrange · drag a scene's "
                     "edges to trim · Shift+drag selects a range to delete or keep."),
      clipPage);
  clipHint->setWordWrap(true);
  clipHint->setObjectName(QStringLiteral("muted"));
  clipControls->addWidget(clipHint);
  clipControls->addStretch();
  tabs->addTab(zoomPage, QStringLiteral("Zoom"));
  auto *clipScroll = new QScrollArea(tabs);
  clipScroll->viewport()->setObjectName(QStringLiteral("studioScrollViewport"));
  clipScroll->setWidgetResizable(true);
  clipScroll->setFrameShape(QFrame::NoFrame);
  clipScroll->setWidget(clipPage);
  tabs->addTab(clipScroll, QStringLiteral("Clip"));
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
  transport->addWidget(timeLabel_);
  transport->addStretch();
  transport->addWidget(playButton_);
  transport->addStretch();
  auto *mute = new TimelineActionButton(
      TimelineActionButton::Symbol::SoundOn, QStringLiteral("Mute preview"),
      QStringLiteral("Mute / unmute preview · M"), theme_, timelinePanel);
  mute->setObjectName(QStringLiteral("mutePreview"));
  mute->setCheckable(true);
  transport->addWidget(mute);
  timelineLayout->addLayout(transport);
  auto *editTools = new QHBoxLayout;
  auto *timelineViewport = new TimelineViewport(timeline_);
  splitButton_ = new TimelineActionButton(TimelineActionButton::Symbol::Split,
      QStringLiteral("Split at playhead"), QStringLiteral("Split at playhead · S"), theme_, this);
  splitButton_->setObjectName(QStringLiteral("splitAtPlayhead"));
  deleteButton_ = new TimelineActionButton(TimelineActionButton::Symbol::Delete,
      QStringLiteral("Delete selection"), QStringLiteral("Delete selected range, clip, or zoom · Delete"), theme_, this);
  deleteButton_->setObjectName(QStringLiteral("deleteSelection"));
  keepButton_ = new TimelineActionButton(TimelineActionButton::Symbol::Keep,
      QStringLiteral("Keep only selection"),
      QStringLiteral("Keep only selection — remove everything outside the range · Ctrl+Z to undo"), theme_, this);
  keepButton_->setObjectName(QStringLiteral("keepSelection"));
  connect(keepButton_, &QPushButton::clicked, this, &StudioWindow::keepSelection);
  editTools->addWidget(splitButton_);
  editTools->addWidget(keepButton_);
  editTools->addStretch();
  editTools->addWidget(deleteButton_);
  timelineLayout->addLayout(editTools);
  // Tab follows the visual layout, not widget creation order: header, then
  // transport and edit tools, then the inspector pages.
  QWidget::setTabOrder(undoButton_, redoButton_);
  QWidget::setTabOrder(redoButton_, help);
  QWidget::setTabOrder(help, inspectorToggle);
  QWidget::setTabOrder(inspectorToggle, exportButton_);
  QWidget::setTabOrder(exportButton_, relinkButton_);
  QWidget::setTabOrder(relinkButton_, playButton_);
  QWidget::setTabOrder(playButton_, mute);
  QWidget::setTabOrder(mute, splitButton_);
  QWidget::setTabOrder(splitButton_, keepButton_);
  QWidget::setTabOrder(keepButton_, deleteButton_);
  QWidget::setTabOrder(deleteButton_, addZoomButton_);
  QWidget::setTabOrder(addZoomButton_, removeZoomButton_);
  QWidget::setTabOrder(removeZoomButton_, zoomSlider_);
  QWidget::setTabOrder(zoomSlider_, easeIn_);
  QWidget::setTabOrder(easeIn_, easeOut_);
  QWidget::setTabOrder(easeOut_, importButton_);
  QWidget::setTabOrder(importButton_, sceneIn_);
  QWidget::setTabOrder(sceneIn_, sceneOut_);
  QWidget::setTabOrder(sceneOut_, duplicateButton_);
  QWidget::setTabOrder(duplicateButton_, earlierButton_);
  QWidget::setTabOrder(earlierButton_, laterButton_);
  QWidget::setTabOrder(laterButton_, transitionType_);
  QWidget::setTabOrder(transitionType_, transitionDuration_);
  QWidget::setTabOrder(transitionDuration_, background_);
  QWidget::setTabOrder(background_, padding_);
  QWidget::setTabOrder(padding_, radius_);
  QWidget::setTabOrder(radius_, resetStyle);
  connect(splitButton_, &QPushButton::clicked, this,
          &StudioWindow::splitAtPlayhead);
  connect(deleteButton_, &QPushButton::clicked, this,
          &StudioWindow::deleteSelection);
  connect(timeline_, &StudioTimeline::selectionChanged, this,
          &StudioWindow::refreshControls);
  connect(timeline_, &StudioTimeline::rangeSelected, this, [this] {
    player_->pause();
    seekTo(timeline_->rangeIn());
  });
  connect(timeline_, &StudioTimeline::rangeSelectionStarted, this, [this] {
    player_->pause();
    scrubTimer_->stop();
    pendingSeek_ = -1;
  });
  connect(timeline_, &StudioTimeline::sceneMoveRequested, this,
          &StudioWindow::moveScene);
  connect(timeline_, &StudioTimeline::sceneTrimRequested, this,
          &StudioWindow::trimScene);
  connect(timeline_, &StudioTimeline::splitRequested, this,
          &StudioWindow::splitAtPlayhead);
  connect(timeline_, &StudioTimeline::deleteRequested, this,
          &StudioWindow::deleteSelection);
  timelineLayout->addWidget(timelineViewport);
  auto *footer = new QHBoxLayout;
  auto *legend =
      new QLabel(QStringLiteral("SPACE  Play / pause     SHIFT+DRAG  Range     ESC  Clear"),
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

  player_ = new StudioPlayback(preview_, this);
  audio_ = player_->audioOutput();
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
  connect(mute, &QPushButton::toggled, audio_, &QAudioOutput::setMuted);
  connect(audio_, &QAudioOutput::mutedChanged, this,
          [mute](bool muted) {
            mute->setChecked(muted);
            mute->setSymbol(muted ? TimelineActionButton::Symbol::SoundOff
                                  : TimelineActionButton::Symbol::SoundOn);
          });
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

  connect(playButton_, &QPushButton::clicked, this,
          &StudioWindow::togglePlayback);
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
            pendingSeek_ = boundedSeek(milliseconds);
            refreshSplitAction();
            if (!scrubTimer_->isActive())
              scrubTimer_->start();
          });
  connect(timeline_, &StudioTimeline::trimChanged, this,
          [this](qint64, qint64) {
            rememberEdit();
            refreshControls();
          });

  connect(player_, &StudioPlayback::durationChanged, this,
          [this](qint64 duration) {
            if (timeline_->duration() != duration)
              timeline_->setDuration(duration);
            refreshControls();
          });
  connect(player_, &StudioPlayback::positionChanged, this,
          [this](qint64 position) {
            if (pendingSeek_ < 0)
              timeline_->setPosition(position);
            // Not the preview: its clock comes from the frame being painted,
            // and two writers with no ordering between them meant whichever
            // signal arrived last decided the crop.
            // Playback stops at the out point: the trim is what is being
            // reviewed, so playing past it would be reviewing the wrong cut.
            const bool range = timeline_->hasRange();
            const qint64 stop = range ? timeline_->rangeOut() : timeline_->trimOut();
            if (range && (position < timeline_->rangeIn() || position >= stop)) {
              player_->pause();
              player_->setPosition(boundedSeek(position));
            } else if (!range && player_->playbackState() == QMediaPlayer::PlayingState &&
                       position >= stop && timeline_->duration() > 0) {
              player_->pause();
              player_->setPosition(stop);
            }
            refreshSplitAction();
            // Scene/transition inspectors depend on edits and selection, not
            // time. Rebuilding all their controls on every frame/timer tick
            // needlessly competes with the GPU frame-preparation pipeline.
            timeLabel_->setText(QStringLiteral("%1 / %2").arg(
                studioTimecode(player_->position()).mid(3),
                studioTimecode(timeline_->duration()).mid(3)));
          });
  connect(player_, &StudioPlayback::playbackStateChanged, this,
          [this](QMediaPlayer::PlaybackState) { refreshControls(); });
  connect(player_, &StudioPlayback::errorOccurred, this,
          [this](const QString &message) {
            mediaFailed_ = true;
            setStatus(message.isEmpty()
                          ? QStringLiteral("Could not play this recording")
                          : message,
                      true);
            refreshControls();
          });

  preview_->setTrack(&zoom_);
  thumbnailTimer_ = new QTimer(this);
  thumbnailTimer_->setSingleShot(true);
  thumbnailTimer_->setInterval(120);
  connect(thumbnailTimer_, &QTimer::timeout, this, &StudioWindow::refreshThumbnails);
  connect(timeline_, &StudioTimeline::thumbnailViewChanged, thumbnailTimer_,
          [this] { thumbnailTimer_->start(); });
  connect(&thumbnailWatcher_, &QFutureWatcher<StudioThumbnail>::finished, this, [this] {
    timeline_->cacheThumbnail(thumbnailWatcher_.result());
    thumbnailBusy_ = false;
    // Results are source-keyed, so an old view's work remains useful after
    // edits. Always choose the next request from the latest visible clips.
    if (!thumbnailTimer_->isActive())
      refreshThumbnails();
  });
  saveTimer_ = new QTimer(this);
  saveTimer_->setSingleShot(true);
  saveTimer_->setInterval(kSaveDebounceMs);
  connect(saveTimer_, &QTimer::timeout, this, &StudioWindow::saveProject);
  scrubTimer_ = new QTimer(this);
  scrubTimer_->setSingleShot(true);
  scrubTimer_->setInterval(35);
  connect(scrubTimer_, &QTimer::timeout, this, [this] {
    if (pendingSeek_ >= 0) {
      // Do not restart decoding faster than it can produce a prepared frame.
      // Keep one latest target while the in-flight seek completes.
      if (!player_->canScrub()) {
        scrubTimer_->start();
        return;
      }
      const qint64 position = pendingSeek_;
      pendingSeek_ = -1;
      player_->scrubTo(boundedSeek(position));
    }
  });
  // Probe and deserialize on a worker. The original media is never modified.
  projectPath_ = path_.endsWith(QStringLiteral(".omasnap.json"))
                     ? path_
                     : path_ + QStringLiteral(".omasnap.json");
  connect(&loadWatcher_, &QFutureWatcher<LoadedSource>::finished, this, [this] {
    const LoadedSource source = loadWatcher_.result();
    if (!source.error.isEmpty()) {
      setStatus(source.error, true);
      mediaFailed_ = true;
      refreshControls();
      return;
    }
    project_ = source.project;
    missingAssets_ = source.missingAssets;
    for (const auto id : missingAssets_)
      if (const auto *asset = studioAsset(project_, id))
        missingPaths_.insert(asset->path);
    loaded_ = true;
    applyProject(true);
    if (!missingAssets_.isEmpty())
      setStatus(QStringLiteral(
                    "Missing media — use Relink media to locate the source."),
                true);
  });
  const QString sourcePath = path_;
  const QString document = projectPath_;
  loadWatcher_.setFuture(QtConcurrent::run([sourcePath, document] {
    LoadedSource source;
    if (QFileInfo::exists(document)) {
      const auto loaded = loadStudioProject(document);
      source.project = loaded.project;
      source.error = loaded.error;
      source.missingAssets = loaded.missingAssets;
      return source;
    }
    if (sourcePath.endsWith(QStringLiteral(".omasnap.json"))) {
      source.error = QStringLiteral("Project file is missing");
      return source;
    }
    const StudioSource media = probeStudioSource(sourcePath);
    if (!media.usable() || media.durationMs <= 0) {
      source.error = QStringLiteral("Could not read a supported video source");
      return source;
    }
    source.project.assets.push_back(
        {1, QFileInfo(sourcePath).absoluteFilePath(), media});
    source.project.clips.push_back({1, 1, 0, media.durationMs, 1.0});
    source.project.canvas =
        QSize(media.size.width() / 2 * 2, media.size.height() / 2 * 2);
    source.project.fpsNumerator = media.fpsNumerator;
    source.project.fpsDenominator = media.fpsDenominator;
    source.error = validateStudioProject(source.project);
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
      saveProject();
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
  captureCursor();
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
  captureCursor();
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
  captureCursor();
  const ZoomCue *cue = activeCue();
  if (!cue)
    return;
  removeZoomCue(zoom_, cue->id);
  timeline_->setSelectedCue(0);
  zoomChanged();
}

void StudioWindow::setSelectedZoomScale(qreal scale) {
  captureCursor();
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
  captureCursor();
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

void StudioWindow::captureCursor() {
  if (editGesture_ || restoring_)
    return;
  if (pendingSeek_ >= 0)
    seekTo(pendingSeek_);
  history_.setCursor(timeline_->selectedClip(), timeline_->selectedCue(),
                     timeline_->rangeIn(), timeline_->rangeOut(),
                     player_->position());
}

void StudioWindow::beginEdit() {
  captureCursor();
  gestureProject_ = project_;
  editGesture_ = true;
}

void StudioWindow::styleChanged() {
  captureCursor();
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
  thumbnailTimer_->start();
}

StudioEditState StudioWindow::editState() const {
  StudioEditState state;
  state.project = project_;
  state.selectedCue = timeline_->selectedCue();
  state.selectedClip = timeline_->selectedClip();
  state.rangeIn = timeline_->rangeIn();
  state.rangeOut = timeline_->rangeOut();
  state.positionMs = player_->position();
  return state;
}

void StudioWindow::rememberEdit() {
  if (restoring_ || editGesture_ || !loaded_)
    return;
  project_.zoom = zoom_;
  project_.style = style_;
  project_.trimInMs = timeline_->trimIn();
  project_.trimOutMs =
      timeline_->trimOut() == timeline_->duration() ? -1 : timeline_->trimOut();
  history_.push(editState());
  saveTimer_->start();
}

void StudioWindow::undoEdit() {
  if (!undoButton_->isEnabled() || editGesture_ || !history_.undo())
    return;
  restoreEdit();
}

void StudioWindow::redoEdit() {
  if (!redoButton_->isEnabled() || editGesture_ || !history_.redo())
    return;
  restoreEdit();
}

void StudioWindow::restoreEdit() {
  const auto state = history_.current();
  // The outgoing selection belongs to the old composition and must not
  // constrain restoration into a shorter or differently arranged project.
  timeline_->clearSelection();
  project_ = state.project;
  applyProject(false, state.positionMs);
  timeline_->setSelectedCue(state.selectedCue);
  timeline_->setSelectedClip(state.selectedClip);
  if (state.rangeIn >= 0)
    timeline_->setRange(state.rangeIn, state.rangeOut);
  if (timeline_->hasRange())
    seekTo(state.positionMs);
  saveTimer_->start();
  refreshControls();
}

void StudioWindow::saveProject() {
  if (!loaded_)
    return;
  if (saving_) {
    savePending_ = true;
    return;
  }
  saving_ = true;
  const QString path = projectPath_;
  const StudioProject project = project_;
  saveWatcher_.setFuture(QtConcurrent::run(
      [path, project] { return saveStudioProject(path, project); }));
}

void StudioWindow::applyProject(bool resetHistory, qint64 position) {
  project_.trimInMs = 0;
  project_.trimOutMs = -1;
  scrubTimer_->stop();
  pendingSeek_ = -1;
  restoring_ = true;
  for (const auto &clip : project_.clips)
    nextClipId_ = qMax(nextClipId_, clip.id + 1);
  for (const auto &asset : project_.assets)
    nextAssetId_ = qMax(nextAssetId_, asset.id + 1);
  missingAssets_.clear();
  for (const auto &asset : project_.assets)
    if (missingPaths_.contains(asset.path))
      missingAssets_.push_back(asset.id);
  zoom_ = project_.zoom;
  style_ = project_.style;
  media_ = {};
  media_.size = project_.canvas;
  media_.fpsNumerator = project_.fpsNumerator;
  media_.fpsDenominator = project_.fpsDenominator;
  const qint64 duration = studioDuration(project_);
  timeline_->setDuration(duration);
  timeline_->setTrim(project_.trimInMs,
                     project_.trimOutMs < 0 ? duration : project_.trimOutMs);
  preview_->setStyle(style_);
  preview_->setTrack(&zoom_);
  mediaFailed_ = !missingAssets_.isEmpty();
  if (!mediaFailed_)
    player_->setProject(project_, position);
  else
    player_->pause();
  preview_->setTrack(&zoom_);
  preview_->setStyle(style_);
  sourceLabel_->setText(
      QStringLiteral("%1 scenes\n%2 × %3\n%4 fps")
          .arg(project_.clips.size())
          .arg(project_.canvas.width())
          .arg(project_.canvas.height())
          .arg(project_.fpsNumerator / double(qMax(1, project_.fpsDenominator)),
               0, 'f', 2));
  relinkButton_->setVisible(!missingAssets_.isEmpty());
  restoring_ = false;
  if (resetHistory)
    history_.reset(editState());
  refreshThumbnails();
  refreshControls();
}

void StudioWindow::refreshThumbnails() {
  if (!loaded_ || closing_ || editGesture_ || thumbnailBusy_)
    return;
  const auto missing = timeline_->missingThumbnails();
  if (missing.isEmpty())
    return;
  thumbnailBusy_ = true;
  thumbnailWatcher_.setFuture(QtConcurrent::run([request = missing.first()]() mutable {
    const QString ffmpeg =
        QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
      return request;
    QProcess process;
    process.start(
          ffmpeg,
          {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-ss"),
           studioTimecode(request.sourceMs), QStringLiteral("-threads"),
           QStringLiteral("1"), QStringLiteral("-filter_threads"),
           QStringLiteral("1"), QStringLiteral("-i"), request.path,
           QStringLiteral("-frames:v"), QStringLiteral("1"),
           QStringLiteral("-vf"),
           QStringLiteral("scale=160:90:force_original_aspect_ratio=decrease,"
                          "pad=160:90:(ow-iw)/2:(oh-ih)/2"),
           QStringLiteral("-threads"), QStringLiteral("1"),
           QStringLiteral("-f"), QStringLiteral("image2pipe"),
           QStringLiteral("-c:v"), QStringLiteral("png"), QStringLiteral("-")});
    if (!process.waitForFinished(5000)) {
      process.kill();
      process.waitForFinished(1000);
      return request;
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
      request.image.loadFromData(process.readAllStandardOutput(), "PNG");
    // Remember failed samples too, preventing an unbounded retry loop.
    return request;
  }));
}

void StudioWindow::relinkAsset() {
  if (missingAssets_.isEmpty() || export_ || importing_ ||
      relinkWatcher_.isRunning())
    return;
  auto *dialog = new QFileDialog(this, QStringLiteral("Relink missing media"));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setOption(QFileDialog::DontUseNativeDialog);
  dialog->setFileMode(QFileDialog::ExistingFile);
  connect(
      dialog, &QFileDialog::fileSelected, this, [this](const QString &path) {
        if (missingAssets_.isEmpty() || relinking_ || export_ || importing_)
          return;
        captureCursor();
        relinking_ = true;
        refreshControls();
        const quint64 id = missingAssets_.first();
        const StudioProject snapshot = project_;
        relinkWatcher_.setFuture(QtConcurrent::run([snapshot, id, path] {
          StudioProjectLoad result;
          result.project = snapshot;
          const StudioSource source = probeStudioSource(path);
          if (!source.usable() || source.durationMs <= 0) {
            result.error =
                QStringLiteral("Replacement is not a supported video");
            return result;
          }
          for (auto &asset : result.project.assets) {
            if (asset.id != id)
              continue;
            for (const auto &clip : result.project.clips)
              if (clip.assetId == id && clip.outMs > source.durationMs) {
                result.error = QStringLiteral(
                    "Replacement is shorter than the referenced clip ranges");
                return result;
              }
            asset.path = QFileInfo(path).absoluteFilePath();
            asset.source = source;
          }
          for (const auto &asset : result.project.assets)
            if (!QFileInfo::exists(asset.path))
              result.missingAssets.push_back(asset.id);
          return result;
        }));
      });
  dialog->open();
}

StudioWindow::~StudioWindow() = default;

void StudioWindow::closeEvent(QCloseEvent *event) {
  if (editGesture_)
    endEdit();
  if (relinking_ || importing_) {
    setStatus(QStringLiteral("Wait for media loading to finish"));
    event->ignore();
    return;
  }
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
      saveProject();
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
  const bool exporting = export_ || relinking_ || importing_;
  refreshSceneControls();
  const qint64 duration = timeline_->duration();
  const bool playing = player_->playbackState() == QMediaPlayer::PlayingState;
  if (auto *play = static_cast<TimelineActionButton *>(playButton_)) {
    play->setSymbol(playing ? TimelineActionButton::Symbol::Pause
                            : TimelineActionButton::Symbol::Play);
    play->setToolTip(playing ? QStringLiteral("Pause · Space")
                             : QStringLiteral("Play · Space"));
  }
  playButton_->setEnabled(!mediaFailed_ && duration > 0);
  keepButton_->setEnabled(!exporting && timeline_->hasRange() && !mediaFailed_);
  keepButton_->setVisible(timeline_->hasRange());
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
  undoButton_->setEnabled(!exporting && history_.canUndo());
  redoButton_->setEnabled(!exporting && history_.canRedo());

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
  splitButton_->setEnabled(editable);
  refreshSplitAction();
  deleteButton_->setEnabled(
      editable && (timeline_->rangeOut() > timeline_->rangeIn() ||
                   timeline_->selectedClip() || timeline_->selectedCue()));
  deleteButton_->setToolTip(deleteButton_->isEnabled()
      ? QStringLiteral("Delete the selected range, clip, or zoom · Delete")
      : QStringLiteral("Select a clip, zoom, or range to delete"));
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
    // Never wipe values being typed: timings commit on Enter, and a refresh
    // in between must not replace the fields from behind.
    const auto *inEdit = easeIn_->findChild<QLineEdit *>();
    const auto *outEdit = easeOut_->findChild<QLineEdit *>();
    const bool typingTiming =
        easeIn_->hasFocus() || easeOut_->hasFocus() ||
        (inEdit && inEdit->hasFocus()) || (outEdit && outEdit->hasFocus());
    if (!typingTiming) {
      const QSignalBlocker quietIn(easeIn_), quietOut(easeOut_);
      easeIn_->setValue(cue ? static_cast<int>(cue->easeInMs) : 400);
      easeOut_->setValue(cue ? static_cast<int>(cue->easeOutMs) : 400);
    }
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
  const qint64 start = timeline_->hasRange() ? timeline_->rangeIn() : timeline_->trimIn();
  const qint64 end = timeline_->hasRange() ? timeline_->rangeOut() - 1 : timeline_->trimOut();
  if (player_->position() < start || player_->position() >= end)
    seekTo(start);
  player_->play();
}

void StudioWindow::seekBy(qint64 milliseconds) {
  seekTo(player_->position() + milliseconds);
}

void StudioWindow::seekTo(qint64 milliseconds) {
  scrubTimer_->stop();
  pendingSeek_ = -1;
  player_->setPosition(boundedSeek(milliseconds));
}

qint64 StudioWindow::boundedSeek(qint64 milliseconds) const {
  if (timeline_->hasRange())
    return qBound(timeline_->rangeIn(), milliseconds, timeline_->rangeOut() - 1);
  return qBound<qint64>(0, milliseconds, timeline_->duration());
}

void StudioWindow::refreshSplitAction() {
  const bool editable = scenesEditable() && !mediaFailed_ && media_.usable();
  const qint64 at = pendingSeek_ >= 0 ? pendingSeek_ : player_->position();
  // Do not rebuild composition lookups on every playback frame. The action
  // itself validates the exact split; paused seeks expose boundary guidance.
  bool available = editable;
  if (player_->playbackState() != QMediaPlayer::PlayingState) {
    const auto frame = studioFrameAt(project_, at);
    available = editable && frame && at > frame->span.startMs &&
                at < frame->span.endMs && !studioBlendAt(project_, at);
  }
  splitButton_->setEnabled(available);
  splitButton_->setToolTip(available
      ? QStringLiteral("Split the scene at the playhead · S")
      : QStringLiteral("Place the playhead inside a scene, outside a transition, to split"));
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

void StudioWindow::extendRangeSelection(int direction) {
  if (!playButton_->isEnabled() || !media_.usable())
    return;
  player_->pause();
  // One-second steps: fast enough to hold across a passage, exact enough to
  // land cut points; the timeline handles stay for frame precision.
  constexpr qint64 kRangeStepMs = 1000;
  const qint64 pos = player_->position();
  const auto stepFrom = [&](qint64 ms) { return ms + direction * kRangeStepMs; };
  qint64 anchor;
  qint64 edge;
  // A range of one millisecond or less parks the playhead on both edges at
  // once; there the key direction picks the moving edge, otherwise extending
  // outward at a project boundary would clear the selection.
  const bool degenerate = timeline_->hasRange() &&
                          timeline_->rangeOut() - timeline_->rangeIn() <= 1;
  if (!timeline_->hasRange()) {
    anchor = pos;
    edge = stepFrom(pos);
  } else if (pos <= timeline_->rangeIn() && (direction < 0 || !degenerate)) {
    // Parked on the start edge: move it exactly; the playhead is the edge.
    anchor = timeline_->rangeOut();
    edge = timeline_->rangeIn() + direction * kRangeStepMs;
  } else if (pos >= timeline_->rangeOut() - 1 && (direction > 0 || !degenerate)) {
    // Parked on the end edge (the shared seek bound keeps the playhead one
    // millisecond inside): move the edge itself so steps stay exact.
    anchor = timeline_->rangeIn();
    edge = timeline_->rangeOut() + direction * kRangeStepMs;
  } else if (direction > 0) {
    // Inside the range: grow outward only, never shrink the far edge.
    anchor = timeline_->rangeIn();
    edge = qMax(timeline_->rangeOut(), stepFrom(pos));
  } else {
    anchor = timeline_->rangeOut();
    edge = qMin(timeline_->rangeIn(), stepFrom(pos));
  }
  edge = qBound<qint64>(0, edge, timeline_->duration());
  if (edge == anchor)
    timeline_->clearSelection();
  else
    timeline_->setRange(anchor, edge);
  // Parks the playhead on the moving edge; the shared seek bound keeps the
  // exclusive range end one millisecond outside, as with mouse selection.
  seekTo(edge);
}

void StudioWindow::keepSelection() {
  if (!keepButton_->isEnabled() || editGesture_)
    return;
  const qint64 start = timeline_->rangeIn();
  const qint64 end = timeline_->rangeOut();
  StudioProject kept = project_;
  // Work on a copy: if either boundary intersects a transition or cannot be
  // represented at the clip's speed, neither half of the edit is published.
  const auto cut = [&](qint64 from, qint64 to) {
    if (from == to)
      return true;
    const auto result = studioDeleteRange(kept, from, to, nextClipId_++);
    if (!result.changed)
      setStatus(result.error.isEmpty() ? QStringLiteral("Choose wider, representable boundaries")
                                      : result.error);
    return result.changed;
  };
  if (start == 0 && end == studioDuration(kept))
    return;
  if (!cut(end, studioDuration(kept)) || !cut(0, start))
    return;
  captureCursor();
  project_ = std::move(kept);
  finishCompositionEdit(0);
  setStatus(QStringLiteral("Kept selection — Ctrl+Z to undo"));
}

void StudioWindow::finishCompositionEdit(qint64 position) {
  player_->pause();
  timeline_->clearSelection();
  applyProject(false, position);
  rememberEdit();
  refreshControls();
}

void StudioWindow::splitAtPlayhead() {
  if (!splitButton_->isEnabled() || editGesture_)
    return;
  captureCursor();
  const qint64 at = player_->position();
  QString error;
  if (!studioSplitClip(project_, at, nextClipId_++, &error)) {
    setStatus(error.isEmpty()
                  ? QStringLiteral(
                        "Choose a representable point inside a scene to split")
                  : error);
    return;
  }
  finishCompositionEdit(at);
  if (const auto frame = studioFrameAt(project_, at))
    timeline_->setSelectedClip(frame->span.clipId);
  rememberEdit();
  setStatus(QStringLiteral("Scene split — Ctrl+Z to undo"));
}

void StudioWindow::deleteSelection() {
  if (!deleteButton_->isEnabled() || editGesture_)
    return;
  captureCursor();
  const auto transitions = project_.transitions;
  const auto anchor = studioFrameAt(project_, player_->position());
  const bool sceneDeletion = timeline_->rangeOut() <= timeline_->rangeIn() &&
                             timeline_->selectedClip();
  StudioCutResult result;
  if (timeline_->rangeOut() > timeline_->rangeIn() && timeline_->rangeIn() >= 0)
    result = studioDeleteRange(project_, timeline_->rangeIn(),
                               timeline_->rangeOut(), nextClipId_++);
  else if (timeline_->selectedCue()) {
    removeSelectedZoom();
    return;
  } else if (timeline_->selectedClip())
    result = studioDeleteClip(project_, timeline_->selectedClip());
  if (!result.changed) {
    setStatus(
        result.error.isEmpty()
            ? QStringLiteral(
                  "Choose a wider passage with representable scene boundaries")
            : result.error);
    return;
  }
  qint64 position =
      studioTimeAfterDelete(player_->position(), result.fromMs, result.toMs);
  if (sceneDeletion && anchor)
    position =
        studioTimelineTime(project_, anchor->span.clipId, anchor->sourceMs)
            .value_or(position);
  finishCompositionEdit(position);
  setStatus(QStringLiteral("Removed %1 — Ctrl+Z to undo")
                .arg(studioTimecode(result.removedMs).mid(3)) +
            transitionAdjustment(transitions));
}

void StudioWindow::startExport() {
  if (!exportButton_->isEnabled() || export_ || editGesture_)
    return;
  export_ = true;
  player_->pause();
  auto *dialog = new StudioExportDialog(project_, path_, this);
  connect(dialog, &StudioExportDialog::exportFinished, this,
          [this](const QString &message, bool failed) {
            export_ = false;
            setStatus(message, failed);
            refreshControls();
          });
  dialog->open();
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
  } else if ((key == Qt::Key_Left || key == Qt::Key_Right) &&
             mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
    repeat = true;
    action = [this, key] {
      extendRangeSelection(key == Qt::Key_Left ? -1 : 1);
    };
  } else if (key == Qt::Key_Home && plain)
    action = [this] {
      player_->pause();
      seekTo(timeline_->hasRange() ? timeline_->rangeIn() : timeline_->trimIn());
    };
  else if (key == Qt::Key_End && plain)
    action = [this] {
      player_->pause();
      seekTo(timeline_->hasRange() ? timeline_->rangeOut() - 1 : timeline_->trimOut());
    };
  else if (key == Qt::Key_O && ctrl)
    action = [this] { chooseScenes(); };
  else if (key == Qt::Key_D && ctrl)
    action = [this] { duplicateScene(); };
  else if (key == Qt::Key_T && plain)
    action = [this] { showTransitionEditor(timeline_->selectedClip()); };
  else if (key == Qt::Key_S && plain)
    action = [this] { splitAtPlayhead(); };
  else if (key == Qt::Key_Z && plain)
    action = [this] {
      if (addZoomButton_->isEnabled())
        addZoomAtPlayhead();
    };
  else if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && plain)
    action = [this] { deleteSelection(); };
  else if (key == Qt::Key_Z && ctrl)
    action = [this] { undoEdit(); };
  else if ((key == Qt::Key_Z && redo) || (key == Qt::Key_Y && ctrl))
    action = [this] { redoEdit(); };
  else if (key == Qt::Key_E && ctrl)
    action = [this] { startExport(); };
  else if (key == Qt::Key_S && ctrl)
    action = [this] {
      saveTimer_->stop();
      saveProject();
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
      timeline_->clearSelection();
      preview_->setTargetMarker(false);
      refreshControls();
      setFocus();
    };
  else if (key == Qt::Key_W && ctrl)
    action = [this] { close(); };
  if (!action)
    return false;
  if (activate && (repeat || !event->isAutoRepeat())) {
    captureCursor();
    action();
  }
  return true;
}

bool StudioWindow::eventFilter(QObject *object, QEvent *event) {
  auto *widget = qobject_cast<QWidget *>(object);
  if (!widget || widget->window() != this ||
      (event->type() != QEvent::KeyPress &&
       event->type() != QEvent::ShortcutOverride))
    return QWidget::eventFilter(object, event);
  auto *key = static_cast<QKeyEvent *>(event);
  // Value editing keeps its native behavior: arrows move the cursor, digits
  // replace the value, Backspace deletes text. Space still transports from
  // any control.
  if (key->key() != Qt::Key_Space && studioHotkeysSuspended(object))
    return false;
  if (handleShortcut(key, event->type() == QEvent::KeyPress)) {
    event->accept();
    return true;
  }
  return false;
}

void StudioWindow::keyPressEvent(QKeyEvent *event) {
  // Keys a focused input ignored bubble up here; hotkeys stay suspended for
  // them exactly as in the event filter above. Space still transports.
  if (event->key() != Qt::Key_Space &&
      studioHotkeysSuspended(QApplication::focusWidget())) {
    QWidget::keyPressEvent(event);
    return;
  }
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
                     "Shift + drag          Select playback range\n"
                     "Ctrl + click          Select clip\n"
                     "Ctrl + drag           Reorder scenes\n"
                     "Scroll                Zoom timeline around pointer\n"
                     "Right button + drag   Pan timeline\n"
                     "Left / Right          Previous / next frame\n"
                     "Shift + Left / Right  Back / forward 5 seconds\n"
                     "Ctrl+Shift+Left/Right Select range in 1-second steps\n"
                     "Home / End            Selection or timeline start / end\n"
                     "Z                     Add zoom at playhead\n"
                     "S                     Split scene at playhead\n"
                     "Delete / Backspace    Delete range, clip, or zoom\n"
                     "Ctrl + Z              Undo\n"
                     "Ctrl+Shift+Z / Ctrl+Y  Redo\n"
                     "Ctrl+O               Add scene files\n"
                     "Ctrl+D               Duplicate selected scene\n"
                     "T                    Edit transition to next scene\n"
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
  if (transitionType_)
    transitionType_->setChrome(chrome);
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
      width() >= 1150 ? QStringLiteral("SPACE  Play / pause     SHIFT+DRAG  Range     CTRL+DRAG  Reorder     ESC  Clear")
                      : QStringLiteral("SPACE  Play / pause     SHIFT+DRAG  Range     ESC  Clear"));
}

void StudioWindow::toggleInspector() {
  inspectorWanted_ = !inspector_->isVisible();
  inspector_->setVisible(inspectorWanted_);
}
