/** @fileoverview Declares the recording indicator: the small pill that sits
 *  under the top bar for as long as the screen is being recorded, and the
 *  only way to stop from the UI. */
#pragma once

#include <QString>
#include <QWidget>

/**
 * A recording is never invisible: while the encoder runs, this pill is on
 * screen showing the elapsed time, and clicking its stop button ends the
 * recording. It is a layer surface in its own `omasnap-record` namespace so
 * a Hyprland `noscreenshare` rule can keep it out of the recording itself.
 *
 * Painted, not composed from widgets, for the same reason the rest of
 * Omasnap's chrome is: pinned fonts and explicit colours, no style, no
 * palette, no theme plugin.
 */
class RecordIndicator final : public QWidget {
  Q_OBJECT
public:
  enum class Phase { Starting, Recording, Paused, Stopping, Failed };

  explicit RecordIndicator(QWidget *parent = nullptr);

  void setPhase(Phase phase);
  /** Recorded time to show, excluding anything spent paused. */
  void setElapsed(qint64 milliseconds);
  /** Replaces the timer with a short message; used for failures. */
  void setMessage(const QString &message);
  /** Natural size for the current contents, for the layer surface. */
  [[nodiscard]] QSize sizeHint() const override;

signals:
  void stopRequested();
  void pauseRequested(bool paused);
  /**
   * The pill's natural size changed. A layer surface has to be told its size
   * separately from the widget, and a stale one clips the contents.
   */
  void desiredSizeChanged(const QSize &size);

protected:
  void leaveEvent(QEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

private:
  enum class Control { None, Pause, Stop };

  /// Resizes to the current contents and announces it, so the layer surface
  /// follows the widget.
  void applySize();

  [[nodiscard]] QString timeText() const;
  [[nodiscard]] QRectF pauseRect() const;
  [[nodiscard]] QRectF stopRect() const;
  [[nodiscard]] Control controlAt(const QPointF &position) const;

  Phase phase_ = Phase::Starting;
  qint64 elapsedMs_ = 0;
  QString message_;
  Control hovered_ = Control::None;
  Control pressed_ = Control::None;
  /// Drives the record dot's slow pulse, so a still screen still reads as
  /// "this is live" rather than "this is a screenshot of a recorder".
  int pulseTick_ = 0;
};

/** Formats milliseconds as `MM:SS`, or `H:MM:SS` past an hour. */
[[nodiscard]] QString formatRecordingTime(qint64 milliseconds);
