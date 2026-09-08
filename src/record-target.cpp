/** @fileoverview The recording target contract and its coordinate maths. */
#include "record-target.hpp"

#include "capture.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRectF>

#include <cmath>

namespace {

QJsonObject rectToJson(const QRect &rect) {
  return QJsonObject{{QStringLiteral("x"), rect.x()},
                     {QStringLiteral("y"), rect.y()},
                     {QStringLiteral("width"), rect.width()},
                     {QStringLiteral("height"), rect.height()}};
}

QRect rectFromJson(const QJsonObject &object) {
  return {object.value(QStringLiteral("x")).toInt(),
          object.value(QStringLiteral("y")).toInt(),
          object.value(QStringLiteral("width")).toInt(),
          object.value(QStringLiteral("height")).toInt()};
}

/// Whole logical pixels containing `rect`. GSR takes integers only, and a
/// region rounded inward would crop a column the user selected.
QRect enclosingLogicalRect(const QRectF &rect) {
  const int left = static_cast<int>(std::floor(rect.left()));
  const int top = static_cast<int>(std::floor(rect.top()));
  const int right = static_cast<int>(std::ceil(rect.right()));
  const int bottom = static_cast<int>(std::ceil(rect.bottom()));
  return {left, top, right - left, bottom - top};
}

} // namespace

QString recordTargetKindName(RecordTargetKind kind) {
  switch (kind) {
  case RecordTargetKind::Region:
    return QStringLiteral("region");
  case RecordTargetKind::Window:
    return QStringLiteral("window");
  case RecordTargetKind::Fullscreen:
    break;
  }
  return QStringLiteral("fullscreen");
}

bool recordTargetKindFromName(const QString &name, RecordTargetKind &kind) {
  if (name == QStringLiteral("region")) {
    kind = RecordTargetKind::Region;
    return true;
  }
  if (name == QStringLiteral("window")) {
    kind = RecordTargetKind::Window;
    return true;
  }
  if (name == QStringLiteral("fullscreen")) {
    kind = RecordTargetKind::Fullscreen;
    return true;
  }
  return false;
}

RecordTarget makeRecordTarget(const MonitorInfo &monitor, RecordTargetKind kind,
                              const QRectF &selection) {
  RecordTarget target;
  target.kind = kind;
  target.output = monitor.name;
  target.workspace = monitor.workspaceId;
  target.scale = monitor.scale > 0.0 ? monitor.scale : 1.0;

  const QRect monitorLocal(QPoint(0, 0), monitor.geometry.size());
  target.logical = kind == RecordTargetKind::Fullscreen
                       ? monitorLocal
                       : enclosingLogicalRect(selection).intersected(monitorLocal);
  if (target.logical.isEmpty())
    return {};

  target.globalLogical = target.logical.translated(monitor.geometry.topLeft());
  // Native pixels rather than MonitorInfo::pixelSize: that one is the raw
  // mode, so a rotated output reports it the wrong way round.
  target.sourcePixels = {qRound(target.logical.width() * target.scale),
                         qRound(target.logical.height() * target.scale)};
  return target;
}

QByteArray writeRecordTarget(const RecordTarget &target) {
  QJsonObject object{
      {QStringLiteral("schema"), RecordTarget::kSchema},
      {QStringLiteral("kind"), recordTargetKindName(target.kind)},
      {QStringLiteral("output"), target.output},
      {QStringLiteral("workspace"), target.workspace},
      {QStringLiteral("scale"), target.scale},
      {QStringLiteral("logical"), rectToJson(target.logical)},
      {QStringLiteral("globalLogical"), rectToJson(target.globalLogical)},
      {QStringLiteral("sourcePixels"),
       QJsonObject{{QStringLiteral("width"), target.sourcePixels.width()},
                   {QStringLiteral("height"), target.sourcePixels.height()}}}};
  if (!target.windowId.isEmpty())
    object.insert(QStringLiteral("windowId"), target.windowId);
  if (!target.windowClass.isEmpty())
    object.insert(QStringLiteral("windowClass"), target.windowClass);
  return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

bool readRecordTarget(const QByteArray &json, RecordTarget &target,
                      QString &error) {
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    error = QStringLiteral("Could not parse recording target: %1")
                .arg(parseError.errorString());
    return false;
  }
  const QJsonObject object = document.object();
  const int schema = object.value(QStringLiteral("schema")).toInt();
  if (schema != RecordTarget::kSchema) {
    error = QStringLiteral("Unsupported recording target schema %1")
                .arg(schema);
    return false;
  }

  RecordTarget parsed;
  if (!recordTargetKindFromName(object.value(QStringLiteral("kind")).toString(),
                                parsed.kind)) {
    error = QStringLiteral("Unknown recording target kind");
    return false;
  }
  parsed.output = object.value(QStringLiteral("output")).toString();
  parsed.workspace = object.value(QStringLiteral("workspace")).toInt();
  parsed.scale = object.value(QStringLiteral("scale")).toDouble(1.0);
  parsed.logical = rectFromJson(object.value(QStringLiteral("logical")).toObject());
  parsed.globalLogical =
      rectFromJson(object.value(QStringLiteral("globalLogical")).toObject());
  const QJsonObject pixels =
      object.value(QStringLiteral("sourcePixels")).toObject();
  parsed.sourcePixels = {pixels.value(QStringLiteral("width")).toInt(),
                         pixels.value(QStringLiteral("height")).toInt()};
  parsed.windowId = object.value(QStringLiteral("windowId")).toString();
  parsed.windowClass = object.value(QStringLiteral("windowClass")).toString();

  if (parsed.output.isEmpty()) {
    error = QStringLiteral("Recording target names no output");
    return false;
  }
  if (parsed.globalLogical.isEmpty()) {
    error = QStringLiteral("Recording target has an empty geometry");
    return false;
  }
  target = parsed;
  return true;
}

QStringList recordTargetSourceArguments(const RecordTarget &target) {
  if (target.kind == RecordTargetKind::Fullscreen) {
    if (target.output.isEmpty())
      return {};
    return {QStringLiteral("-w"), target.output};
  }
  if (target.globalLogical.isEmpty())
    return {};
  return {QStringLiteral("-w"),
          QStringLiteral("%1x%2+%3+%4")
              .arg(target.globalLogical.width())
              .arg(target.globalLogical.height())
              .arg(target.globalLogical.x())
              .arg(target.globalLogical.y())};
}

QString recordTargetSlug(const RecordTarget &target) {
  if (!target.windowClass.isEmpty()) {
    const QString slug = appFilenameSlug(target.windowClass);
    if (!slug.isEmpty())
      return slug;
  }
  return appFilenameSlug(target.output);
}
