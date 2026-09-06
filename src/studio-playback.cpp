/** @fileoverview Project-time transport without a rendered preview proxy. */
#include "studio-playback.hpp"
#include "studio-preview.hpp"
#include <QUrl>
#include <cmath>

StudioPlayback::StudioPlayback(StudioPreview *preview, QObject *parent)
    : QObject(parent), preview_(preview), audio_(new QAudioOutput(this)) {
  for (int index = 0; index < 2; ++index) {
    auto &slot = slots_[index];
    slot.player = new QMediaPlayer(this);
    slot.sink = new QVideoSink(this);
    slot.audio = new QAudioOutput(this);
    slot.player->setVideoSink(slot.sink);
    slot.player->setAudioOutput(slot.audio);
    connect(slot.sink, &QVideoSink::videoFrameChanged, this,
            [this, index](const QVideoFrame &frame) { present(index, frame); });
    connect(slot.player, &QMediaPlayer::mediaStatusChanged, this,
            [this, index](QMediaPlayer::MediaStatus status) {
              auto &current = slots_[index];
              if (status == QMediaPlayer::LoadedMedia) {
                current.loaded = true;
                current.player->setPosition(current.seekMs);
                // Pause only once a seek frame has arrived. An immediate
                // play/pause can stop FFmpeg before it delivers the first frame
                // of a new source.
                current.priming = true;
                updateAudio();
                current.player->play();
                if (index == active_)
                  emit ready();
              } else if (status == QMediaPlayer::EndOfMedia &&
                         index == active_ &&
                         state_ == QMediaPlayer::PlayingState) {
                if (const auto *span = spanFor(current.clipId))
                  setPosition(span->endMs);
              }
            });
    connect(slot.player, &QMediaPlayer::errorOccurred, this,
            [this, index](QMediaPlayer::Error, const QString &error) {
              slots_[index].error = error;
              slots_[index].loaded = false;
              if (index == active_) {
                pause();
                emit errorOccurred(error);
              }
            });
  }
  connect(audio_, &QAudioOutput::volumeChanged, this,
          [this] { updateAudio(); });
  connect(audio_, &QAudioOutput::mutedChanged, this, [this] { updateAudio(); });
  timer_.setInterval(10);
  timer_.setTimerType(Qt::PreciseTimer);
  connect(&timer_, &QTimer::timeout, this, &StudioPlayback::tick);
  updateAudio();
}

QMediaPlayer *StudioPlayback::activePlayer() const {
  return slots_[active_].player;
}

const StudioSpan *StudioPlayback::spanFor(quint64 clipId) const {
  for (const auto &span : spans_)
    if (span.clipId == clipId)
      return &span;
  return nullptr;
}

void StudioPlayback::setProject(const StudioProject &project) {
  const QString error = validateStudioProject(project);
  if (!error.isEmpty()) {
    emit errorOccurred(error);
    return;
  }
  const bool mediaChanged =
      project_.clips != project.clips || project_.assets != project.assets;
  project_ = project;
  spans_ = studioComposition(project_);
  preview_->setTrack(&project_.zoom);
  preview_->setStyle(project_.style);
  preview_->setCanvasSize(project_.canvas);
  emit durationChanged(duration());
  if (spans_.isEmpty()) {
    stop();
    for (auto &slot : slots_) {
      slot.clipId = 0;
      slot.path.clear();
      slot.frame = {};
      slot.player->setSource({});
    }
    preview_->clearFrame();
    return;
  }
  if (mediaChanged) {
    for (auto &slot : slots_) {
      slot.player->pause();
      slot.clipId = 0;
      slot.frame = {};
    }
    setPosition(qMin(position_, duration() - 1));
  } else {
    preview_->setPosition(position_);
  }
}

void StudioPlayback::loadSlot(int index, const StudioFrame &frame) {
  auto &slot = slots_[index];
  const auto *asset = studioAsset(project_, frame.span.assetId);
  if (!asset)
    return;
  slot.player->pause();
  slot.error.clear();
  slot.clipId = frame.span.clipId;
  slot.frame = {};
  slot.seekMs = frame.sourceMs;
  slot.awaitingSeek = true;
  slot.player->setPlaybackRate(frame.span.speed * rate_);
  if (slot.path != asset->path ||
      slot.player->mediaStatus() == QMediaPlayer::NoMedia ||
      slot.player->mediaStatus() == QMediaPlayer::InvalidMedia) {
    slot.loaded = false;
    slot.path = asset->path;
    if (slot.player->mediaStatus() == QMediaPlayer::InvalidMedia)
      slot.player->setSource({});
    slot.player->setSource(QUrl::fromLocalFile(slot.path));
  } else {
    seekSlot(index, frame.sourceMs);
  }
}

void StudioPlayback::seekSlot(int index, qint64 sourceMs) {
  auto &slot = slots_[index];
  slot.seekMs = sourceMs;
  slot.awaitingSeek = true;
  slot.frame = {};
  if (slot.loaded) {
    slot.player->setPosition(sourceMs);
    slot.priming = true;
    updateAudio();
    slot.player->play();
  }
}

void StudioPlayback::setPosition(qint64 milliseconds) {
  frameClock_.invalidate();
  position_ = qBound<qint64>(0, milliseconds, duration());
  if (position_ == duration() && state_ == QMediaPlayer::PlayingState)
    pause();
  if (spans_.isEmpty()) {
    emit positionChanged(position_);
    return;
  }
  const auto frame = studioFrameAt(project_, qMin(position_, duration() - 1));
  if (!frame)
    return;
  preview_->invalidatePendingFrames();
  if (slots_[active_].clipId != frame->span.clipId) {
    slots_[active_].player->pause();
    const int incoming = 1 - active_;
    const bool preloaded = slots_[incoming].clipId == frame->span.clipId;
    active_ = incoming;
    if (!preloaded)
      loadSlot(active_, *frame);
    else if (qAbs(slots_[active_].seekMs - frame->sourceMs) > 1)
      seekSlot(active_, frame->sourceMs);
    if (const auto *asset = studioAsset(project_, frame->span.assetId))
      preview_->setRotation(asset->source.rotation);
    emit activeClipChanged(frame->span.clipId);
  } else {
    seekSlot(active_, frame->sourceMs);
  }
  preview_->setPosition(position_);
  if (!slots_[active_].error.isEmpty()) {
    pause();
    emit errorOccurred(slots_[active_].error);
  }
  updateAudio();
  if (slots_[active_].frame.isValid())
    present(active_, slots_[active_].frame);
  if (state_ == QMediaPlayer::PlayingState && slots_[active_].loaded)
    slots_[active_].player->play();
  emit positionChanged(position_);
  preload();
}

void StudioPlayback::preload() {
  const auto *span = spanFor(slots_[active_].clipId);
  if (!span)
    return;
  const auto next = studioFrameAt(project_, span->endMs);
  const int other = 1 - active_;
  if (next && slots_[other].clipId != next->span.clipId)
    loadSlot(other, *next);
  updateAudio();
}

void StudioPlayback::present(int index, const QVideoFrame &frame) {
  if (!frame.isValid())
    return;
  auto &slot = slots_[index];
  const auto *span = spanFor(slot.clipId);
  if (!span)
    return;
  const qint64 sourceMs = qMax<qint64>(0, frame.startTime() / 1000);
  const qint64 endMs = qMax(sourceMs + 1, frame.endTime() / 1000);
  // Reject queued frames from before an asynchronous seek. A containing VFR
  // frame is valid, even when its PTS is earlier than the requested instant.
  if (slot.awaitingSeek &&
      (endMs <= slot.seekMs || sourceMs > slot.seekMs + 100))
    return;
  slot.awaitingSeek = false;
  if (endMs <= span->inMs || sourceMs >= span->outMs)
    return;
  slot.frame = frame;
  if (slot.priming) {
    slot.priming = false;
    if (index != active_ || state_ != QMediaPlayer::PlayingState)
      slot.player->pause();
  }
  if (index != active_)
    return;
  const qint64 timeline =
      span->startMs +
      qRound64(static_cast<double>(qMax(span->inMs, sourceMs) - span->inMs) /
               span->speed);
  preview_->setVideoFrame(frame, timeline);
  if (state_ == QMediaPlayer::PlayingState) {
    position_ = qBound(span->startMs, timeline, span->endMs - 1);
    frameClockPosition_ = position_;
    frameClock_.start();
    emit positionChanged(position_);
  }
}

void StudioPlayback::tick() {
  if (state_ != QMediaPlayer::PlayingState)
    return;
  const auto &slot = slots_[active_];
  const auto *span = spanFor(slot.clipId);
  // QMediaPlayer positionChanged is deliberately coarse. A decoded frame's
  // presentation clock owns the cut, so audio cannot run a notification
  // interval beyond a removed passage before the next slot takes over.
  if (span && slot.loaded && !slot.awaitingSeek &&
      ((frameClock_.isValid() &&
        frameClockPosition_ +
                qRound64(static_cast<double>(frameClock_.elapsed()) * rate_) >=
            span->endMs) ||
       slot.player->position() >= span->outMs))
    setPosition(span->endMs);
}

void StudioPlayback::setState(QMediaPlayer::PlaybackState state) {
  if (state_ == state)
    return;
  state_ = state;
  updateAudio();
  if (state == QMediaPlayer::PlayingState)
    timer_.start();
  else
    timer_.stop();
  emit playbackStateChanged(state);
}

void StudioPlayback::play() {
  if (spans_.isEmpty())
    return;
  if (position_ >= duration())
    setPosition(0);
  setState(QMediaPlayer::PlayingState);
  if (slots_[active_].loaded)
    slots_[active_].player->play();
}

void StudioPlayback::pause() {
  frameClock_.invalidate();
  setState(QMediaPlayer::PausedState);
  for (auto &slot : slots_)
    if (!slot.priming)
      slot.player->pause();
}

void StudioPlayback::stop() {
  pause();
  position_ = 0;
  setState(QMediaPlayer::StoppedState);
  emit positionChanged(position_);
}

void StudioPlayback::setPlaybackRate(double rate) {
  if (!std::isfinite(rate) || rate <= 0)
    return;
  rate_ = rate;
  for (auto &slot : slots_)
    if (const auto *span = spanFor(slot.clipId))
      slot.player->setPlaybackRate(span->speed * rate_);
}

void StudioPlayback::updateAudio() {
  for (int i = 0; i < 2; ++i) {
    slots_[i].audio->setVolume(audio_->volume());
    slots_[i].audio->setMuted(audio_->isMuted() || i != active_ ||
                              state_ != QMediaPlayer::PlayingState);
  }
}
