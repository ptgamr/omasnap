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
  void setProject(const StudioProject &project);
  void setPosition(qint64 milliseconds);
  [[nodiscard]] qint64 position() const { return position_; }
  [[nodiscard]] qint64 duration() const { return studioDuration(project_); }
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
    QString path;
    QString error;
    qint64 seekMs = 0;
    bool awaitingSeek = false;
    bool loaded = false;
    bool priming = false;
    QVideoFrame frame;
  };
  void loadSlot(int index, const StudioFrame &frame);
  void seekSlot(int index, qint64 sourceMs);
  void preload();
  void present(int index, const QVideoFrame &frame);
  void tick();
  void setState(QMediaPlayer::PlaybackState state);
  void updateAudio();
  [[nodiscard]] const StudioSpan *spanFor(quint64 clipId) const;
  StudioPreview *preview_;
  StudioProject project_;
  QVector<StudioSpan> spans_;
  std::array<Slot, 2> slots_;
  QAudioOutput *audio_;
  QTimer timer_;
  QElapsedTimer frameClock_;
  qint64 frameClockPosition_ = 0;
  int active_ = 0;
  qint64 position_ = 0;
  double rate_ = 1.0;
  QMediaPlayer::PlaybackState state_ = QMediaPlayer::StoppedState;
};
