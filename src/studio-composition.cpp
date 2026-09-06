/** @fileoverview Timestamp-aware, non-destructive multi-source export. */
#include "studio-composition.hpp"

#include <cmath>

namespace {
QString seconds(qint64 ms) { return QString::number(ms / 1000.0, 'f', 6); }

QString tempo(double speed) {
  QStringList filters;
  while (speed > 2.0) {
    filters << QStringLiteral("atempo=2");
    speed /= 2.0;
  }
  while (speed < 0.5) {
    filters << QStringLiteral("atempo=0.5");
    speed *= 2.0;
  }
  filters << QStringLiteral("atempo=%1").arg(speed, 0, 'g', 12);
  return filters.join(QLatin1Char(','));
}
} // namespace

QStringList studioCompositionArguments(const StudioProject &project,
                                       const QString &destination,
                                       QString &error) {
  error = validateStudioProject(project);
  if (!error.isEmpty())
    return {};
  if (destination.isEmpty() || project.clips.isEmpty()) {
    error = QStringLiteral("Add a scene and choose an export destination.");
    return {};
  }
  // This graph owns one decoder per clip occurrence. Keep export bounded
  // until a batched renderer exists; the editable project may be larger.
  if (project.clips.size() > 64) {
    error = QStringLiteral("Export supports up to 64 scenes. Export a smaller "
                           "composition; the full project remains saved.");
    return {};
  }
  const auto spans = studioComposition(project);
  const qint64 duration = studioDuration(project);
  const qint64 exportIn = project.trimInMs;
  const qint64 exportOut = project.trimOutMs < 0 ? duration : project.trimOutMs;
  if (exportOut <= exportIn) {
    error = QStringLiteral("The export range is empty.");
    return {};
  }
  const int width = project.canvas.width();
  const int height = project.canvas.height();
  if (width % 2 || height % 2) {
    error = QStringLiteral("The export canvas must have even dimensions.");
    return {};
  }
  QStringList args{QStringLiteral("-hide_banner"),
                   QStringLiteral("-loglevel"),
                   QStringLiteral("error"),
                   QStringLiteral("-y"),
                   QStringLiteral("-filter_complex_threads"),
                   QStringLiteral("2")};
  QStringList graph;
  QString joined;
  for (qsizetype i = 0; i < spans.size(); ++i) {
    const auto &span = spans[i];
    const auto *asset = studioAsset(project, span.assetId);
    if (!asset || !asset->source.usable()) {
      error = QStringLiteral("A scene has no usable source. Relink its media.");
      return {};
    }
    const QString index = QString::number(i);
    const QString length = seconds(span.endMs - span.startMs);
    args << QStringLiteral("-threads") << QStringLiteral("2")
         << QStringLiteral("-ss") << seconds(span.inMs) << QStringLiteral("-t")
         << seconds(span.outMs - span.inMs) << QStringLiteral("-i")
         << asset->path;
    // Input seeking rebases stream timestamps against the same cut point.
    // Do not independently subtract STARTPTS from audio and video: doing so
    // erases an intentional audio offset. concat gets exact segment length
    // from its padded audio anchor, not a rounded number of video frames.
    graph << QStringLiteral(
                 "[%1:v:0]trim=duration=%2,setpts=PTS/%3,"
                 "scale=%4:%5:force_original_aspect_ratio=decrease:"
                 "force_divisible_by=2,setsar=1,pad=%4:%5:(ow-iw)/2:(oh-ih)/2:"
                 "color=black,format=yuv420p,settb=AVTB[v%1]")
                 .arg(index, seconds(span.outMs - span.inMs),
                      QString::number(span.speed, 'g', 12))
                 .arg(width)
                 .arg(height);
    QString audio;
    if (asset->source.audioStreams == 0) {
      audio = QStringLiteral("anullsrc=r=48000:cl=stereo");
    } else {
      // QMediaPlayer previews the primary audio track. Keep exports faithful
      // until the independent track mixer is implemented; do not surprise
      // the user by mixing a microphone they could not hear in the preview.
      audio =
          QStringLiteral(
              "[%1:a:0]atrim=duration=%2,aresample=48000:async=1:"
              "first_pts=0,aformat=sample_fmts=fltp:channel_layouts=stereo,%3")
              .arg(index, seconds(span.outMs - span.inMs), tempo(span.speed));
    }
    graph << audio +
                 QStringLiteral(",apad,atrim=duration=%1,asetpts=N/SR/TB[a%2]")
                     .arg(length, index);
    joined += QStringLiteral("[v%1][a%1]").arg(index);
  }
  graph << joined + QStringLiteral("concat=n=%1:v=1:a=1[sequence][sound]")
                        .arg(spans.size());
  QString video = QStringLiteral("[sequence]fps=%1/%2:start_time=0")
                      .arg(project.fpsNumerator)
                      .arg(project.fpsDenominator);
  const auto camera = zoomPanExpressions(project.zoom, project.fpsNumerator,
                                         project.fpsDenominator);
  if (!camera.identity)
    video +=
        QStringLiteral(",zoompan=z='%1':x='%2':y='%3':d=1:s=%4x%5:fps=%6/%7")
            .arg(camera.z, camera.x, camera.y)
            .arg(width)
            .arg(height)
            .arg(project.fpsNumerator)
            .arg(project.fpsDenominator);
  video += QStringLiteral(",trim=start=%1:end=%2,setpts=PTS-STARTPTS")
               .arg(seconds(exportIn), seconds(exportOut));
  if (project.style.padding > 0 || project.style.radius > 0) {
    const int cardWidth = qMax(
        2, qRound(width * (1 - 2 * project.style.padding / 100.0)) / 2 * 2);
    const int cardHeight = qMax(
        2, qRound(height * (1 - 2 * project.style.padding / 100.0)) / 2 * 2);
    video += QStringLiteral(",scale=%1:%2,format=rgba")
                 .arg(cardWidth)
                 .arg(cardHeight);
    const double radius = project.style.radius * height / 1080.0;
    if (radius > 0)
      video += QStringLiteral(",geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*"
                              "clip(%1+0.5-hypot(max(abs(X-(W-1)/2)-(W/"
                              "2-%1),0),max(abs(Y-(H-1)/2)-(H/2-%1),0)),0,1)'")
                   .arg(radius, 0, 'f', 4);
    graph << video + QStringLiteral("[card]");
    graph << QStringLiteral("color=c=%1:s=%2x%3:r=%4/%5[canvas]")
                 .arg(project.style.color().name())
                 .arg(width)
                 .arg(height)
                 .arg(project.fpsNumerator)
                 .arg(project.fpsDenominator);
    graph << QStringLiteral("[canvas][card]overlay=x=(W-w)/2:y=(H-h)/2:"
                            "shortest=1:format=auto,format=yuv420p[video]");
  } else {
    graph << video + QStringLiteral(",format=yuv420p[video]");
  }
  graph << QStringLiteral(
               "[sound]atrim=start=%1:end=%2,asetpts=PTS-STARTPTS[audio]")
               .arg(seconds(exportIn), seconds(exportOut));
  args << QStringLiteral("-filter_complex") << graph.join(QLatin1Char(';'))
       << QStringLiteral("-map") << QStringLiteral("[video]")
       << QStringLiteral("-map") << QStringLiteral("[audio]")
       << QStringLiteral("-t") << seconds(exportOut - exportIn)
       << QStringLiteral("-c:v") << QStringLiteral("libx264")
       << QStringLiteral("-crf") << QStringLiteral("20")
       << QStringLiteral("-preset") << QStringLiteral("veryfast")
       << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
       << QStringLiteral("-c:a") << QStringLiteral("aac")
       << QStringLiteral("-ar") << QStringLiteral("48000")
       << QStringLiteral("-map_metadata") << QStringLiteral("-1")
       << QStringLiteral("-movflags") << QStringLiteral("+faststart")
       << destination;
  return args;
}
