/** @fileoverview Tests the recording indicator offscreen: the elapsed-time
 *  readout, and that its controls are where the clicks that stop and pause a
 *  recording have to land. */
#include "record-indicator-smoke.hpp"

#include "record-indicator.hpp"

#include <QSignalSpy>
#include <QTest>

namespace {

bool check(bool condition, QString &error, const QString &message) {
  if (condition)
    return true;
  error = message;
  return false;
}

} // namespace

bool runRecordIndicatorSmoke(QString &error) {
  if (!check(formatRecordingTime(0) == QStringLiteral("00:00") &&
                 formatRecordingTime(9500) == QStringLiteral("00:09") &&
                 formatRecordingTime(61000) == QStringLiteral("01:01") &&
                 formatRecordingTime(3661000) == QStringLiteral("1:01:01") &&
                 formatRecordingTime(-5) == QStringLiteral("00:00"),
             error, QStringLiteral("elapsed time is formatted wrong")))
    return false;

  RecordIndicator indicator;
  indicator.setPhase(RecordIndicator::Phase::Recording);
  indicator.resize(indicator.sizeHint());
  indicator.show();

  QSignalSpy stopped(&indicator, &RecordIndicator::stopRequested);
  QSignalSpy paused(&indicator, &RecordIndicator::pauseRequested);

  // Stop is the rightmost control, pause sits beside it. Both are inside the
  // pill; a recording that could only be stopped from a terminal would not be
  // a visible indicator at all.
  const int centreY = indicator.height() / 2;
  QTest::mouseClick(&indicator, Qt::LeftButton, {},
                    QPoint(indicator.width() - 12 - 11, centreY));
  if (!check(stopped.count() == 1, error,
             QStringLiteral("clicking the stop control did not stop")))
    return false;

  QTest::mouseClick(&indicator, Qt::LeftButton, {},
                    QPoint(indicator.width() - 12 - 22 - 8 - 11, centreY));
  if (!check(paused.count() == 1 && paused.at(0).at(0).toBool(), error,
             QStringLiteral("clicking the pause control did not pause")))
    return false;

  // The body of the pill is not a button: a stray click on the clock must not
  // end a recording.
  QTest::mouseClick(&indicator, Qt::LeftButton, {}, QPoint(30, centreY));
  if (!check(stopped.count() == 1 && paused.count() == 1, error,
             QStringLiteral("clicking the clock triggered a control")))
    return false;

  // While starting and while saving there is nothing to click: the encoder
  // has not confirmed it is running, or has already been asked to stop.
  indicator.setPhase(RecordIndicator::Phase::Stopping);
  indicator.resize(indicator.sizeHint());
  QTest::mouseClick(&indicator, Qt::LeftButton, {},
                    QPoint(indicator.width() - 12, centreY));
  if (!check(stopped.count() == 1, error,
             QStringLiteral("the pill was still clickable while saving")))
    return false;

  return true;
}
