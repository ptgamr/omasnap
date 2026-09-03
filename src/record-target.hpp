/** @fileoverview Declares the recording target contract: what the screenshot
 *  selector hands to the recorder, and how that becomes a
 *  gpu-screen-recorder source argument. Pure data and pure functions, so the
 *  coordinate maths is testable without a compositor. */
#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>

struct MonitorInfo;

/** What the user pointed at. Window records the screen rectangle the window
 *  occupies, occlusion included, exactly like a window screenshot does. */
enum class RecordTargetKind { Region, Window, Fullscreen };

/**
 * One recording target, versioned because the recorder reads it out of a file
 * written by a separate process.
 *
 * Two rectangles, both in Hyprland's logical coordinates: `logical` is
 * monitor-local (the space Omasnap's selector works in) and `globalLogical`
 * is compositor-global. gpu-screen-recorder takes the global one --- measured
 * against GSR 6.0.1, `-w WxH+X+Y` is global *logical*, and the encoded video
 * comes out at logical x monitor scale native pixels. See
 * docs/recording-targets.md.
 */
struct RecordTarget {
  static constexpr int kSchema = 1;

  RecordTargetKind kind = RecordTargetKind::Fullscreen;
  /** Hyprland/GSR output name, e.g. "DP-1". */
  QString output;
  int workspace = 0;
  qreal scale = 1.0;
  QRect logical;
  QRect globalLogical;
  /** Native pixel size the encoder is expected to produce. */
  QSize sourcePixels;
  /** Window metadata, empty unless `kind` is Window. */
  QString windowId;
  QString windowClass;
};

/** Lowercase serialization name ("region", "window", "fullscreen"). */
[[nodiscard]] QString recordTargetKindName(RecordTargetKind kind);
[[nodiscard]] bool recordTargetKindFromName(const QString &name,
                                            RecordTargetKind &kind);

/**
 * Builds a target from a monitor and a selection in that monitor's logical
 * (preview) coordinates. The selection is rounded outward to whole logical
 * pixels and clamped to the monitor, because GSR only takes integers and a
 * region that hangs off the output is rejected. An empty result means the
 * selection did not overlap the monitor.
 */
[[nodiscard]] RecordTarget makeRecordTarget(const MonitorInfo &monitor,
                                            RecordTargetKind kind,
                                            const QRectF &selection);

/** Serializes to the versioned on-disk form the recorder reads. */
[[nodiscard]] QByteArray writeRecordTarget(const RecordTarget &target);
/** Parses that form. Rejects an unknown schema and an empty geometry. */
[[nodiscard]] bool readRecordTarget(const QByteArray &json,
                                    RecordTarget &target, QString &error);

/**
 * The gpu-screen-recorder source arguments for this target: `-w <output>` for
 * a whole display, `-w WxH+X+Y` for an area. (GSR 6.0.1 deprecated the
 * separate `-region` flag in favour of the inline form.) Empty when the
 * target has no usable geometry.
 */
[[nodiscard]] QStringList recordTargetSourceArguments(const RecordTarget &target);

/** Filename-safe suffix naming what was recorded, e.g. "firefox" or "dp-1". */
[[nodiscard]] QString recordTargetSlug(const RecordTarget &target);
