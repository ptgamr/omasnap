/** @fileoverview Bounded source decoders driven by the shared composition. */
#pragma once

#include "studio-project.hpp"
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QObject>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoSink>
#include <array>

class StudioPreview;

/** Two decoder slots, never one per scene. Source files remain untouched;
 * frames retain timestamp-based VFR timing until preview maps them to the
 * edited composition. The inactive slot is muted and preloads the next cut. */
class StudioPlayback final : public QObject {
  Q_OBJECT
public:
  explicit StudioPlayback(StudioPreview *preview, QObject *parent = nullptr);
  void setProject(const StudioProject &project, qint64 desiredPosition = -1);
  void setPosition(qint64 milliseconds);
  /** Interactive seeks are serialized by the window until preparation finishes.
   */
  void scrubTo(qint64 milliseconds);
  [[nodiscard]] bool seekPending() const;
  [[nodiscard]] bool canScrub() const;
  [[nodiscard]] qint64 position() const { return position_; }
  [[nodiscard]] qint64 duration() const {
    return spans_.isEmpty() ? 0 : spans_.last().endMs;
  }
  void play();
  void pause();
  void stop();
  [[nodiscard]] QMediaPlayer::PlaybackState playbackState() const {
    return state_;
  }
  void setPlaybackRate(double rate);
  [[nodiscard]] double playbackRate() const { return rate_; }
  [[nodiscard]] QAudioOutput *audioOutput() const { return audio_; }
  [[nodiscard]] QMediaPlayer *activePlayer() const;

signals:
  void positionChanged(qint64 milliseconds);
  void durationChanged(qint64 milliseconds);
  void playbackStateChanged(QMediaPlayer::PlaybackState state);
  void errorOccurred(const QString &message);
  void activeClipChanged(quint64 clipId);
  void ready();

private:
  struct Slot {
    QMediaPlayer *player = nullptr;
    QVideoSink *sink = nullptr;
    QAudioOutput *audio = nullptr;
    quint64 clipId = 0;
    quint64 loadGeneration = 0;
    QString path;
    QString error;
    qint64 seekMs = 0;
    bool awaitingSeek = false;
    bool loaded = false;
    bool priming = false;
    QVideoFrame frame;
  };
  void loadSlot(int index, const StudioFrame &frame);
  void seekSlot(int index, qint64 sourceMs, bool retainFrame = false);
  void seekPosition(qint64 milliseconds, bool retainFrame);
  void preload();
  void present(int index, const QVideoFrame &frame, bool cached = false);
  void tick();
  void finishSpan(const StudioSpan &span);
  void setState(QMediaPlayer::PlaybackState state);
  void updateAudio();
  void refreshComposition();
  void synchronize();
  [[nodiscard]] const StudioSpan *nextSpan() const;
  [[nodiscard]] bool contributing(int index) const;
  [[nodiscard]] const std::optional<StudioBlend> &currentBlend() const;
  [[nodiscard]] const StudioSpan *spanFor(quint64 clipId) const;
  StudioPreview *preview_;
  StudioProject project_;
  QVector<StudioSpan> spans_;
  std::array<Slot, 2> slots_;
  QAudioOutput *audio_;
  QTimer timer_;
  QElapsedTimer frameClock_;
  QElapsedTimer seekClock_;
  mutable qint64 blendTime_ = -1;
  mutable std::optional<StudioBlend> blend_;
  qint64 frameClockPosition_ = 0;
  int active_ = 0;
  qint64 position_ = 0;
  double rate_ = 1.0;
  bool waiting_ = false;
  std::array<bool, 2> prepared_{};
  QMediaPlayer::PlaybackState state_ = QMediaPlayer::StoppedState;
};
