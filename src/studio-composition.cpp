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

QString transitionFilter(StudioTransitionKind kind) {
  // xfade's P counts down from one to zero. Wipes use pixel centres to
  // match the GPU's half-open normalized clips without an extra seam column.
  switch (kind) {
  case StudioTransitionKind::Crossfade:
    return QStringLiteral("transition=fade");
  case StudioTransitionKind::FadeBlack:
    return QStringLiteral(
        "transition=custom:expr='A*max(2*P-1,0)+B*max(1-2*P,0)'");
  case StudioTransitionKind::WipeLeft:
    return QStringLiteral("transition=custom:expr='if(gte(X+0.5,W*P),B,A)'");
  case StudioTransitionKind::WipeRight:
    return QStringLiteral("transition=custom:expr='if(lt(X+0.5,W*(1-P)),B,A)'");
  case StudioTransitionKind::WipeUp:
    return QStringLiteral("transition=custom:expr='if(gte(Y+0.5,H*P),B,A)'");
  case StudioTransitionKind::WipeDown:
    return QStringLiteral("transition=custom:expr='if(lt(Y+0.5,H*(1-P)),B,A)'");
  // Native slides move existing pixels, unlike wipes. Their direction names
  // match Studio: Left brings the incoming scene from the right, and so on.
  case StudioTransitionKind::SlideLeft:
    return QStringLiteral("transition=slideleft");
  case StudioTransitionKind::SlideRight:
    return QStringLiteral("transition=slideright");
  case StudioTransitionKind::SlideUp:
    return QStringLiteral("transition=slideup");
  case StudioTransitionKind::SlideDown:
    return QStringLiteral("transition=slidedown");
  }
  return {};
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
  const QSize effective = studioEffectiveCanvas(project);
  const int width = effective.width();
  const int height = effective.height();
  if (width % 2 || height % 2) {
    error = QStringLiteral("The export canvas must have even dimensions.");
    return {};
  }
  // Scenes fit the source canvas; its black bars are part of the frame and
  // zoom with it. The aspect expansion lives behind the card instead, so the
  // added bands always show the styled background.
  const int fittedWidth = project.canvas.width();
  const int fittedHeight = project.canvas.height();
  QStringList args{QStringLiteral("-hide_banner"),
                   QStringLiteral("-loglevel"),
                   QStringLiteral("error"),
                   QStringLiteral("-y"),
                   QStringLiteral("-filter_complex_threads"),
                   QStringLiteral("2")};
  QStringList graph;
  QString joined;
  const bool transitions = !project.transitions.isEmpty();
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
    QString sourceVideo =
        QStringLiteral(
            "[%1:v:0]trim=duration=%2,setpts=PTS/%3,"
            "scale=%4:%5:force_original_aspect_ratio=decrease:"
            "force_divisible_by=2,setsar=1,pad=%4:%5:(ow-iw)/2:(oh-ih)/2:"
            "color=black,format=yuv420p,settb=AVTB")
            .arg(index, seconds(span.outMs - span.inMs),
                 QString::number(span.speed, 'g', 12))
            .arg(fittedWidth)
            .arg(fittedHeight);
    if (transitions)
      sourceVideo +=
          QStringLiteral(",fps=%1/%2:start_time=0,format=gbrp,settb=AVTB")
              .arg(project.fpsNumerator)
              .arg(project.fpsDenominator);
    graph << sourceVideo + QStringLiteral("[v%1]").arg(index);
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
  // Wallpaper rides as one looped still after the scene inputs, stretched to
  // fill like Bettershot. A missing file fails loudly in ffmpeg, the same
  // way a missing scene source does.
  const bool wallpaper = !project.style.wallpaperPath.isEmpty();
  if (wallpaper)
    args << QStringLiteral("-loop") << QStringLiteral("1") << QStringLiteral("-i")
         << project.style.wallpaperPath;
  if (!transitions) {
    // Leave the timestamp-aware hard-cut path alone: it does not need a
    // per-scene CFR conversion, RGB intermediate, or overlapping decoders.
    graph << joined + QStringLiteral("concat=n=%1:v=1:a=1[sequence][sound]")
                          .arg(spans.size());
  } else {
    QString currentVideo = QStringLiteral("v0"),
            currentAudio = QStringLiteral("a0");
    for (qsizetype i = 1; i < spans.size(); ++i) {
      const auto *transition =
          studioTransition(project, spans[i - 1].clipId, spans[i].clipId);
      const QString index = QString::number(i);
      if (transition) {
        // Blend in RGB, as the GPU does. FFmpeg's built-in fadeblack has an
        // asymmetric nonlinear curve, not a black midpoint. P runs 1 -> 0.
        const QString effect = transitionFilter(transition->kind);
        graph << QStringLiteral("[%1][v%2]xfade=%3:duration=%4:offset=%5[jv%2]")
                     .arg(currentVideo, index, effect,
                          seconds(transition->durationMs),
                          seconds(spans[i].startMs));
        graph << QStringLiteral(
                     "[%1][a%2]acrossfade=d=%3:o=1:c1=tri:c2=tri[ja%2]")
                     .arg(currentAudio, index, seconds(transition->durationMs));
      } else {
        graph << QStringLiteral(
                     "[%1][%2][v%3][a%3]concat=n=2:v=1:a=1[jv%3][ja%3]")
                     .arg(currentVideo, currentAudio, index);
      }
      currentVideo = QStringLiteral("mv%1").arg(index);
      currentAudio = QStringLiteral("ma%1").arg(index);
      // Absolute model endpoints own every join. Do not propagate a rounded
      // previous video's length into the next transition's placement.
      graph << QStringLiteral("[jv%1]trim=duration=%2,fps=%3/%4:start_time=0,"
                              "format=gbrp,settb=AVTB[%5]")
                   .arg(index, seconds(spans[i].endMs))
                   .arg(project.fpsNumerator)
                   .arg(project.fpsDenominator)
                   .arg(currentVideo);
      graph << QStringLiteral(
                   "[ja%1]apad,atrim=duration=%2,asetpts=N/SR/TB[%3]")
                   .arg(index, seconds(spans[i].endMs), currentAudio);
    }
    graph << QStringLiteral("[%1]null[sequence]").arg(currentVideo);
    graph << QStringLiteral("[%1]anull[sound]").arg(currentAudio);
  }
  QString video = QStringLiteral("[sequence]fps=%1/%2:start_time=0")
                      .arg(project.fpsNumerator)
                      .arg(project.fpsDenominator);
  const auto camera = zoomPanExpressions(project.zoom, project.fpsNumerator,
                                         project.fpsDenominator);
  if (!camera.identity)
    video +=
        QStringLiteral(",zoompan=z='%1':x='%2':y='%3':d=1:s=%4x%5:fps=%6/%7")
            .arg(camera.z, camera.x, camera.y)
            .arg(fittedWidth)
            .arg(fittedHeight)
            .arg(project.fpsNumerator)
            .arg(project.fpsDenominator);
  video += QStringLiteral(",trim=start=%1:end=%2,setpts=PTS-STARTPTS")
               .arg(seconds(exportIn), seconds(exportOut));
  // An aspect expansion needs the canvas even with no padding or corners:
  // without it the output would stay the source size.
  if (project.style.padding > 0 || project.style.radius > 0 ||
      effective != project.canvas) {
    const int cardWidth = qMax(
        2, qRound(width * (1 - 2 * project.style.padding / 100.0)) / 2 * 2);
    const int cardHeight = qMax(
        2, qRound(height * (1 - 2 * project.style.padding / 100.0)) / 2 * 2);
    // The content frame keeps its shape inside the card: the surround stays
    // transparent so the styled canvas shows through (aspect bands). The
    // rounded mask runs before the padding on the content size, so it never
    // reads the padded alpha that geq cannot interpolate.
    const double fit =
        qMin(cardWidth / static_cast<double>(fittedWidth),
             cardHeight / static_cast<double>(fittedHeight));
    const int fitWidth = qMax(2, qRound(fittedWidth * fit) / 2 * 2);
    const int fitHeight = qMax(2, qRound(fittedHeight * fit) / 2 * 2);
    video += QStringLiteral(",scale=%1:%2,format=rgba")
                 .arg(fitWidth)
                 .arg(fitHeight);
    // Canvas units like the preview, even though the mask runs pre-pad.
    const double radius = project.style.radius * height / 1080.0;
    if (radius > 0)
      video += QStringLiteral(",geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*"
                              "clip(%1+0.5-hypot(max(abs(X-(W-1)/2)-(W/"
                              "2-%1),0),max(abs(Y-(H-1)/2)-(H/2-%1),0)),0,1)'")
                   .arg(radius, 0, 'f', 4);
    if (fitWidth != cardWidth || fitHeight != cardHeight)
      video += QStringLiteral(",pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=black@0")
                   .arg(cardWidth)
                   .arg(cardHeight);
    graph << video + QStringLiteral("[card]");
    if (wallpaper) {
      // Transparency flattens onto black, matching the preview worker: the
      // wallpaper keeps its alpha for the blend, then the canvas is opaque.
      graph << QStringLiteral(
                   "[%1:v:0]scale=%2:%3:force_divisible_by=2,setsar=1,"
                   "format=rgba[wall]")
                   .arg(spans.size())
                   .arg(width)
                   .arg(height);
      graph << QStringLiteral("color=c=black:s=%1x%2:r=%3/%4[wallbg]")
                   .arg(width)
                   .arg(height)
                   .arg(project.fpsNumerator)
                   .arg(project.fpsDenominator);
      graph << QStringLiteral("[wallbg][wall]overlay=x=0:y=0:format=auto,"
                              "fps=%1/%2,format=yuv420p[canvas]")
                   .arg(project.fpsNumerator)
                   .arg(project.fpsDenominator);
    } else if (StudioStyle::isGradient(project.style.background)) {
      const StudioGradient preset =
          StudioStyle::gradient(project.style.background);
      const auto hex = [](const StudioGradientStop &stop) {
        return studioStopColor(stop).name();
      };
      // Endpoints stay inside the frame: out-of-range values make the
      // filter pick random ones. speed=0 pins the axis: by default it
      // rotates a little on every frame.
      graph << QStringLiteral(
                   "gradients=s=%1x%2:r=%3/%4:n=3:speed=0:c0=%5:c1=%6:c2=%7:"
                   "x0=%8:y0=%9:x1=%10:y1=%11[canvas]")
                   .arg(width)
                   .arg(height)
                   .arg(project.fpsNumerator)
                   .arg(project.fpsDenominator)
                   .arg(hex(preset.stops[0]), hex(preset.stops[1]),
                        hex(preset.stops[2]))
                   .arg(qRound(preset.startX * (width - 1)))
                   .arg(qRound(preset.startY * (height - 1)))
                   .arg(qRound(preset.endX * (width - 1)))
                   .arg(qRound(preset.endY * (height - 1)));
    } else {
      graph << QStringLiteral("color=c=%1:s=%2x%3:r=%4/%5[canvas]")
                   .arg(project.style.color().name())
                   .arg(width)
                   .arg(height)
                   .arg(project.fpsNumerator)
                   .arg(project.fpsDenominator);
    }
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
