/** @fileoverview Project-time transport without a rendered preview proxy. */
#include "studio-playback.hpp"
#include "studio-preview.hpp"
#include <QUrl>
#include <cmath>

namespace {
bool containsTime(const QVideoFrame &frame, qint64 milliseconds) {
  return frame.isValid() && frame.startTime() >= 0 &&
         frame.startTime() <= milliseconds * 1000 &&
         frame.endTime() > milliseconds * 1000;
}
} // namespace

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
              if (status == QMediaPlayer::LoadingMedia) {
                // Replacing a source can synchronously report LoadedMedia
                // while stopping the OLD source, before LoadingMedia for the
                // new one. Retire that callback: seeking during LoadingMedia
                // is ignored by the backend, leaving a nonzero seek decoding
                // from zero with every frame rejected until it catches up.
                current.loaded = false;
                ++current.loadGeneration;
                return;
              }
              // LoadedMedia also occurs during seeks; prime once per source,
              // not once per status event, or each seek queues another seek.
              if (status == QMediaPlayer::LoadedMedia && !current.loaded) {
                current.loaded = true;
                const quint64 generation = current.loadGeneration;
                // Loading completion can restore a pause requested while the
                // source was loading. Start the seek after that callback has
                // returned, or Qt can override play() before producing a frame.
                QTimer::singleShot(0, this, [this, index, generation] {
                  auto &loaded = slots_[index];
                  if (!loaded.loaded || !loaded.awaitingSeek ||
                      loaded.loadGeneration != generation)
                    return;
                  seekSlot(index, loaded.seekMs);
                  if (index == active_)
                    emit ready();
                });
              } else if (status == QMediaPlayer::EndOfMedia &&
                         index == active_ &&
                         state_ == QMediaPlayer::PlayingState) {
                if (const auto *span = spanFor(current.clipId))
                  finishSpan(*span);
              }
            });
    connect(slot.player, &QMediaPlayer::errorOccurred, this,
            [this, index](QMediaPlayer::Error, const QString &error) {
              slots_[index].error = error;
              slots_[index].loaded = false;
              if (contributing(index)) {
                pause();
                emit errorOccurred(error);
              }
            });
  }
  connect(audio_, &QAudioOutput::volumeChanged, this,
          [this] { updateAudio(); });
  connect(audio_, &QAudioOutput::mutedChanged, this, [this] { updateAudio(); });
  connect(preview_, &StudioPreview::videoFrameReady, this, [this](int index) {
    prepared_[index] = true;
    synchronize();
  });
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

void StudioPlayback::setProject(const StudioProject &project,
                                qint64 desiredPosition) {
  const QString error = validateStudioProject(project);
  if (!error.isEmpty()) {
    emit errorOccurred(error);
    return;
  }
  const bool mediaChanged = project_.clips != project.clips ||
                            project_.assets != project.assets ||
                            project_.transitions != project.transitions;
  const qint64 target = desiredPosition >= 0 ? desiredPosition : position_;
  const auto before = studioFrameAt(project_, position_);
  const auto after = studioFrameAt(project, target);
  const auto clip = [](const StudioProject &p,
                       quint64 id) -> const StudioClip * {
    for (const auto &item : p.clips)
      if (item.id == id)
        return &item;
    return nullptr;
  };
  bool keepCurrent = false;
  if (before && after && before->span.clipId == after->span.clipId &&
      before->sourceMs == after->sourceMs && !currentBlend() &&
      !studioBlendAt(project, target) && prepared_[active_] &&
      slots_[active_].loaded && slots_[active_].error.isEmpty() &&
      preview_->videoSlotReady(active_) && slots_[active_].frame.isValid()) {
    const auto *oldAsset = studioAsset(project_, before->span.assetId);
    const auto *newAsset = studioAsset(project, after->span.assetId);
    const auto *oldClip = clip(project_, before->span.clipId);
    const auto *newClip = clip(project, after->span.clipId);
    keepCurrent = oldAsset && newAsset && *oldAsset == *newAsset && oldClip &&
                  newClip && *oldClip == *newClip;
  }
  project_ = project;
  spans_ = studioComposition(project_);
  blendTime_ = -1;
  preview_->setTrack(&project_.zoom);
  preview_->setStyle(project_.style);
  preview_->setCanvasSize(project_.canvas);
  emit durationChanged(duration());
  if (spans_.isEmpty()) {
    stop();
    for (auto &slot : slots_) {
      ++slot.loadGeneration;
      slot.clipId = 0;
      slot.path.clear();
      slot.frame = {};
      slot.player->setSource({});
    }
    preview_->clearFrame();
    return;
  }
  if (mediaChanged) {
    // The visible frame may belong to a passage the command just deleted.
    // Do not keep displaying it while the replacement source seek decodes.
    // Ordinary playback cuts do not take this project-edit path.
    for (int index = 0; index < 2; ++index) {
      if (keepCurrent && index == active_)
        continue;
      auto &slot = slots_[index];
      ++slot.loadGeneration;
      slot.player->pause();
      slot.clipId = 0;
      slot.frame = {};
      slot.priming = false;
      slot.awaitingSeek = false;
      prepared_[index] = false;
      preview_->clearVideoSlot(index);
    }
    if (keepCurrent) {
      position_ = target;
      frameClockPosition_ = target;
      frameClock_.restart();
      refreshComposition();
      preload();
      synchronize();
      emit positionChanged(position_);
      return;
    }
    setPosition(
        qMin(desiredPosition >= 0 ? desiredPosition : position_, duration()));
  } else if (desiredPosition >= 0 && desiredPosition != position_) {
    setPosition(desiredPosition);
  } else {
    preview_->setPosition(position_);
  }
}

void StudioPlayback::loadSlot(int index, const StudioFrame &frame) {
  prepared_[index] = false;
  auto &slot = slots_[index];
  ++slot.loadGeneration;
  const auto *asset = studioAsset(project_, frame.span.assetId);
  if (!asset)
    return;
  if (slot.loaded)
    slot.player->pause();
  const bool failed = !slot.error.isEmpty();
  slot.error.clear();
  slot.clipId = frame.span.clipId;
  slot.frame = {};
  slot.priming = false;
  preview_->clearVideoSlot(index);
  slot.seekMs = frame.sourceMs;
  slot.awaitingSeek = true;
  slot.player->setPlaybackRate(frame.span.speed * rate_);
  const bool needsReload =
      failed || (!slot.loaded &&
                 slot.player->mediaStatus() != QMediaPlayer::LoadingMedia);
  if (slot.path != asset->path || needsReload ||
      slot.player->mediaStatus() == QMediaPlayer::NoMedia ||
      slot.player->mediaStatus() == QMediaPlayer::InvalidMedia) {
    slot.loaded = false;
    slot.path = asset->path;
    if (slot.player->source() == QUrl::fromLocalFile(slot.path))
      slot.player->setSource({});
    slot.player->setSource(QUrl::fromLocalFile(slot.path));
  } else {
    seekSlot(index, frame.sourceMs);
  }
}

void StudioPlayback::seekSlot(int index, qint64 sourceMs, bool retainFrame) {
  auto &slot = slots_[index];
  slot.seekMs = sourceMs;
  slot.awaitingSeek = true;
  slot.frame = {};
  prepared_[index] = false;
  if (!retainFrame)
    preview_->clearVideoSlot(index);
  else
    preview_->invalidatePendingFrames(index);
  if (slot.loaded) {
    slot.priming = true;
    updateAudio();
    slot.player->setPosition(sourceMs);
    if (slot.priming)
      slot.player->play();
  }
}

void StudioPlayback::setPosition(qint64 milliseconds) {
  seekPosition(milliseconds, false);
}

void StudioPlayback::scrubTo(qint64 milliseconds) {
  seekPosition(milliseconds, true);
}

bool StudioPlayback::seekPending() const {
  if (spans_.isEmpty())
    return false;
  for (int index = 0; index < 2; ++index)
    if (contributing(index) && slots_[index].error.isEmpty() &&
        (slots_[index].awaitingSeek || !prepared_[index]))
      return true;
  return false;
}

bool StudioPlayback::canScrub() const {
  // A lost/failed frame must not permanently lock out the latest user target.
  // Normal seeks finish first; a stalled one can be superseded after one
  // second.
  return !seekPending() ||
         (seekClock_.isValid() && seekClock_.elapsed() >= 1000);
}

void StudioPlayback::seekPosition(qint64 milliseconds, bool retainFrame) {
  seekClock_.start();
  const qint64 target = qBound<qint64>(0, milliseconds, duration());
  retainFrame =
      retainFrame && !currentBlend() && !studioBlendAt(project_, target);
  frameClock_.invalidate();
  waiting_ = state_ == QMediaPlayer::PlayingState;
  position_ = target;
  if (position_ == duration() && state_ == QMediaPlayer::PlayingState)
    pause();
  if (spans_.isEmpty()) {
    emit positionChanged(position_);
    return;
  }
  const auto frame = studioFrameAt(project_, qMin(position_, duration() - 1));
  if (!frame)
    return;
  if (slots_[active_].clipId != frame->span.clipId) {
    if (slots_[active_].loaded)
      slots_[active_].player->pause();
    const int incoming = 1 - active_;
    const bool preloaded = slots_[incoming].clipId == frame->span.clipId;
    active_ = incoming;
    if (!preloaded)
      loadSlot(active_, *frame);
    // seekMs records a request, not where this decoder stopped after playing.
    // Reusing it can show a later frame (or skip the requested clip entirely).
    else if (!containsTime(slots_[active_].frame, frame->sourceMs))
      seekSlot(active_, frame->sourceMs);
    if (const auto *asset = studioAsset(project_, frame->span.assetId))
      preview_->setRotation(asset->source.rotation);
    emit activeClipChanged(frame->span.clipId);
  } else {
    seekSlot(active_, frame->sourceMs, retainFrame);
  }
  const auto blend = currentBlend();
  if (blend) {
    const int other = 1 - active_;
    if (slots_[other].clipId != blend->incoming.span.clipId)
      loadSlot(other, blend->incoming);
    else
      seekSlot(other, blend->incoming.sourceMs);
  }
  refreshComposition();
  if (!slots_[active_].error.isEmpty()) {
    pause();
    emit errorOccurred(slots_[active_].error);
  }
  updateAudio();
  if (slots_[active_].frame.isValid())
    present(active_, slots_[active_].frame, true);
  emit positionChanged(position_);
  preload();
  synchronize();
}

const StudioSpan *StudioPlayback::nextSpan() const {
  for (qsizetype i = 0; i + 1 < spans_.size(); ++i)
    if (spans_[i].clipId == slots_[active_].clipId)
      return &spans_[i + 1];
  return nullptr;
}

void StudioPlayback::preload() {
  const auto *span = spanFor(slots_[active_].clipId);
  if (!span)
    return;
  const auto *next = nextSpan();
  const int other = 1 - active_;
  if (next && slots_[other].clipId != next->clipId)
    loadSlot(other, StudioFrame{*next, next->inMs});
  else if (!next) {
    // A previous project or seek may have left this decoder priming. It is
    // not contributing and has no next clip to prepare; do not decode to EOF.
    auto &unused = slots_[other];
    unused.awaitingSeek = false;
    unused.priming = false;
    unused.player->pause();
  }
  updateAudio();
}

void StudioPlayback::present(int index, const QVideoFrame &frame, bool cached) {
  if (!frame.isValid())
    return;
  auto &slot = slots_[index];
  if (!slot.loaded)
    return;
  // Multimedia may have several queued frames when pause takes effect. Keep
  // the first requested frame, not a later buffered frame, for paused seeks
  // and incoming preloads; otherwise a cut can skip its opening frames.
  if (!cached && !slot.awaitingSeek && slot.frame.isValid() &&
      (!contributing(index) || state_ != QMediaPlayer::PlayingState ||
       waiting_))
    return;
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
  if (endMs <= span->inMs || sourceMs >= span->outMs)
    return;
  slot.awaitingSeek = false;
  slot.frame = frame;
  if (slot.priming) {
    slot.priming = false;
    if (!contributing(index) || state_ != QMediaPlayer::PlayingState ||
        waiting_)
      slot.player->pause();
  }
  const auto *asset = studioAsset(project_, span->assetId);
  preview_->setVideoFrame(index, frame, asset ? asset->source.rotation : 0);
  if (index != active_)
    return;
  const qint64 timeline =
      span->startMs +
      qRound64(static_cast<double>(qMax(span->inMs, sourceMs) - span->inMs) /
               span->speed);
  if (state_ == QMediaPlayer::PlayingState && !waiting_) {
    qint64 target = qBound(span->startMs, timeline, span->endMs - 1);
    if (const auto *next = nextSpan(); next && position_ < next->startMs)
      target = qMin(target, next->startMs);
    position_ = qMax(position_, target);
    frameClockPosition_ = position_;
    frameClock_.start();
    emit positionChanged(position_);
  }
  refreshComposition();
}

void StudioPlayback::tick() {
  if (state_ != QMediaPlayer::PlayingState)
    return;
  if (waiting_) {
    synchronize();
    return;
  }
  const auto &slot = slots_[active_];
  const auto *span = spanFor(slot.clipId);
  // QMediaPlayer positionChanged is deliberately coarse. A decoded frame's
  // presentation clock owns the cut, so audio cannot run a notification
  // interval beyond a removed passage before the next slot takes over.
  if (!span || !slot.loaded || slot.awaitingSeek || !frameClock_.isValid())
    return;
  qint64 target = frameClockPosition_ +
                  qRound64(static_cast<double>(frameClock_.elapsed()) * rate_);
  const auto *next = nextSpan();
  if (next && position_ < next->startMs && target >= next->startMs &&
      next->startMs < span->endMs) {
    position_ = next->startMs;
    frameClock_.invalidate();
    refreshComposition();
    synchronize();
    emit positionChanged(position_);
    return;
  }
  if (target >= span->endMs) {
    finishSpan(*span);
    return;
  }
  position_ = qMax(position_, target);
  refreshComposition();
  synchronize();
  emit positionChanged(position_);
}

void StudioPlayback::finishSpan(const StudioSpan &span) {
  const auto *next = nextSpan();
  if (next && next->startMs <= span.endMs &&
      slots_[1 - active_].clipId == next->clipId &&
      !slots_[1 - active_].awaitingSeek && prepared_[1 - active_] &&
      (next->startMs < span.endMs ||
       containsTime(slots_[1 - active_].frame, next->inMs)) &&
      preview_->videoSlotReady(1 - active_)) {
    const qint64 boundary = span.endMs;
    slots_[active_].player->pause();
    active_ = 1 - active_;
    position_ = boundary;
    frameClockPosition_ = position_;
    frameClock_.restart();
    emit activeClipChanged(slots_[active_].clipId);
    refreshComposition();
    preload();
    synchronize();
    emit positionChanged(position_);
  } else {
    setPosition(span.endMs);
  }
}

const std::optional<StudioBlend> &StudioPlayback::currentBlend() const {
  if (blendTime_ != position_) {
    blend_ = studioBlendAt(project_, position_);
    blendTime_ = position_;
  }
  return blend_;
}

bool StudioPlayback::contributing(int index) const {
  if (index == active_)
    return true;
  const auto &blend = currentBlend();
  return blend && slots_[index].clipId == blend->incoming.span.clipId;
}

void StudioPlayback::refreshComposition() {
  const auto &blend = currentBlend();
  preview_->setComposition(active_, blend ? 1 - active_ : -1,
                           blend ? blend->kind
                                 : StudioTransitionKind::Crossfade,
                           blend ? blend->progress : 0, position_);
  updateAudio();
}

void StudioPlayback::synchronize() {
  if (state_ != QMediaPlayer::PlayingState)
    return;
  for (int i = 0; i < 2; ++i) {
    if (contributing(i) && !slots_[i].error.isEmpty()) {
      const QString error = slots_[i].error;
      pause();
      emit errorOccurred(error);
      return;
    }
  }
  const bool wasWaiting = waiting_;
  waiting_ = false;
  for (int i = 0; i < 2; ++i)
    if (contributing(i) && (slots_[i].awaitingSeek || !prepared_[i] ||
                            !preview_->videoSlotReady(i)))
      waiting_ = true;
  if (waiting_)
    frameClock_.invalidate();
  else if (wasWaiting || !frameClock_.isValid()) {
    frameClockPosition_ = position_;
    frameClock_.restart();
  }
  updateAudio();
  for (int i = 0; i < 2; ++i) {
    auto &slot = slots_[i];
    if (!slot.loaded || slot.priming)
      continue;
    if (!waiting_ && contributing(i)) {
      if (slot.player->playbackState() != QMediaPlayer::PlayingState)
        slot.player->play();
    } else if (slot.player->playbackState() == QMediaPlayer::PlayingState) {
      slot.player->pause();
    }
  }
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
  synchronize();
}

void StudioPlayback::pause() {
  frameClock_.invalidate();
  setState(QMediaPlayer::PausedState);
  for (auto &slot : slots_)
    if (slot.loaded && !slot.priming)
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
  const auto &blend = currentBlend();
  for (int i = 0; i < 2; ++i) {
    const double gain = blend ? (i == active_ ? blend->outgoingAudioGain
                                              : blend->incomingAudioGain)
                              : (i == active_ ? 1 : 0);
    slots_[i].audio->setVolume(audio_->volume() * static_cast<float>(gain));
    slots_[i].audio->setMuted(audio_->isMuted() || !contributing(i) ||
                              waiting_ || state_ != QMediaPlayer::PlayingState);
  }
}
