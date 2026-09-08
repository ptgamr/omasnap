/** @fileoverview Tests the selector-to-recorder target contract: logical
 *  coordinate mapping on scaled and offset outputs, outward rounding,
 *  clamping, the GSR source argument, and JSON round trips. */
#include "record-target-smoke.hpp"

#include "capture.hpp"
#include "record-target.hpp"

namespace {

MonitorInfo scaledMonitor() {
  // The measurement rig from docs/recording-targets.md: HDMI-A-1, 3840x2160
  // at 1.5x, sitting to the right of another output.
  MonitorInfo monitor;
  monitor.name = QStringLiteral("HDMI-A-1");
  monitor.geometry = {2560, 0, 2560, 1440};
  monitor.pixelSize = {3840, 2160};
  monitor.scale = 1.5;
  monitor.workspaceId = 1;
  return monitor;
}

bool check(bool condition, QString &error, const QString &message) {
  if (condition)
    return true;
  error = message;
  return false;
}

} // namespace

bool runRecordTargetSmoke(QString &error) {
  const MonitorInfo monitor = scaledMonitor();

  // A monitor-local selection becomes a compositor-global logical rectangle,
  // and the native pixel size is that rectangle times the scale. These are
  // the numbers gpu-screen-recorder 6.0.1 was measured against.
  const RecordTarget region = makeRecordTarget(
      monitor, RecordTargetKind::Region, QRectF(440, 300, 800, 600));
  if (!check(region.logical == QRect(440, 300, 800, 600), error,
             QStringLiteral("region kept the wrong monitor-local rectangle")))
    return false;
  if (!check(region.globalLogical == QRect(3000, 300, 800, 600), error,
             QStringLiteral("region was not translated into global logical "
                            "coordinates")))
    return false;
  if (!check(region.sourcePixels == QSize(1200, 900), error,
             QStringLiteral("region native pixel size ignored the scale")))
    return false;
  if (!check(recordTargetSourceArguments(region) ==
                 QStringList{QStringLiteral("-w"),
                             QStringLiteral("800x600+3000+300")},
             error,
             QStringLiteral("region did not produce the inline -w geometry")))
    return false;

  // A fractional selection rounds outward: rounding in would drop a column
  // the user dragged over.
  const RecordTarget rounded = makeRecordTarget(
      monitor, RecordTargetKind::Region, QRectF(10.4, 20.6, 100.3, 50.1));
  if (!check(rounded.logical == QRect(10, 20, 101, 51), error,
             QStringLiteral("fractional selection did not round outward")))
    return false;

  // A selection dragged past the edge is clamped; GSR rejects a region that
  // hangs off the output.
  const RecordTarget clamped = makeRecordTarget(
      monitor, RecordTargetKind::Region, QRectF(2500, 1400, 400, 400));
  if (!check(clamped.globalLogical == QRect(5060, 1400, 60, 40), error,
             QStringLiteral("selection past the monitor edge was not clamped")))
    return false;

  // A selection entirely off the monitor yields nothing to record.
  const RecordTarget offscreen = makeRecordTarget(
      monitor, RecordTargetKind::Region, QRectF(3000, 100, 200, 200));
  if (!check(offscreen.globalLogical.isEmpty() && offscreen.output.isEmpty(),
             error,
             QStringLiteral("an off-monitor selection produced a target")))
    return false;
  if (!check(recordTargetSourceArguments(offscreen).isEmpty(), error,
             QStringLiteral("an empty target produced GSR arguments")))
    return false;

  // Fullscreen names the output instead of a rectangle, and takes its native
  // size from the logical size times the scale rather than from the raw mode:
  // a rotated output reports that mode the wrong way round.
  const RecordTarget full =
      makeRecordTarget(monitor, RecordTargetKind::Fullscreen, {});
  if (!check(full.globalLogical == monitor.geometry, error,
             QStringLiteral("fullscreen did not cover the monitor")))
    return false;
  if (!check(full.sourcePixels == QSize(3840, 2160), error,
             QStringLiteral("fullscreen native pixel size is wrong")))
    return false;
  if (!check(recordTargetSourceArguments(full) ==
                 QStringList{QStringLiteral("-w"),
                             QStringLiteral("HDMI-A-1")},
             error, QStringLiteral("fullscreen did not name the output")))
    return false;

  MonitorInfo rotated;
  rotated.name = QStringLiteral("DP-1");
  rotated.geometry = {1120, 0, 1440, 2560}; // 2560x1440 mode, transform 1.
  rotated.pixelSize = {2560, 1440};
  rotated.scale = 1.0;
  const RecordTarget rotatedFull =
      makeRecordTarget(rotated, RecordTargetKind::Fullscreen, {});
  if (!check(rotatedFull.sourcePixels == QSize(1440, 2560), error,
             QStringLiteral("a rotated output took its size from the raw "
                            "mode")))
    return false;

  // The target crosses a process boundary, so it round trips through JSON.
  RecordTarget window = makeRecordTarget(monitor, RecordTargetKind::Window,
                                         QRectF(100, 100, 640, 480));
  window.windowId = QStringLiteral("0x5640e59d02d0");
  window.windowClass = QStringLiteral("org.mozilla.firefox");
  RecordTarget parsed;
  QString parseError;
  if (!check(readRecordTarget(writeRecordTarget(window), parsed, parseError),
             error,
             QStringLiteral("target did not survive a JSON round trip: %1")
                 .arg(parseError)))
    return false;
  if (!check(parsed.kind == window.kind && parsed.output == window.output &&
                 parsed.logical == window.logical &&
                 parsed.globalLogical == window.globalLogical &&
                 parsed.sourcePixels == window.sourcePixels &&
                 parsed.windowId == window.windowId &&
                 parsed.windowClass == window.windowClass &&
                 qFuzzyCompare(parsed.scale, window.scale),
             error, QStringLiteral("round-tripped target lost a field")))
    return false;
  if (!check(recordTargetSlug(parsed) == QStringLiteral("firefox"), error,
             QStringLiteral("window target did not name its app")))
    return false;
  if (!check(recordTargetSlug(full) == QStringLiteral("hdmi-a-1"), error,
             QStringLiteral("display target did not name its output")))
    return false;

  // A newer schema is refused rather than half-read.
  QByteArray future = writeRecordTarget(window);
  future.replace("\"schema\":1", "\"schema\":2");
  RecordTarget ignored;
  QString schemaError;
  if (!check(!readRecordTarget(future, ignored, schemaError) &&
                 !schemaError.isEmpty(),
             error, QStringLiteral("a future schema was accepted")))
    return false;
  QString garbageError;
  if (!check(!readRecordTarget("not json", ignored, garbageError), error,
             QStringLiteral("malformed target JSON was accepted")))
    return false;

  return true;
}
