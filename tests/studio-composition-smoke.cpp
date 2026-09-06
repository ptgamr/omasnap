/** @fileoverview Decode exported compositions to verify ordering and clocks. */
#include "studio-composition-smoke.hpp"
#include "studio-composition.hpp"
#include "studio-preview.hpp"

#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>

namespace {
bool run(const QString &tool, const QStringList &arguments, QByteArray &output,
         QString &error) {
  QProcess process;
  process.start(tool, arguments);
  if (!process.waitForFinished(30000)) {
    process.kill();
    process.waitForFinished(1000);
    error =
        QStringLiteral("Composition fixture process timed out: %1").arg(tool);
    return false;
  }
  output = process.readAllStandardOutput();
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    error = QString::fromUtf8(process.readAllStandardError());
    return false;
  }
  return true;
}

bool exportProject(const QString &ffmpeg, const StudioProject &project,
                   const QString &path, QString &error) {
  const auto args = studioCompositionArguments(project, path, error);
  QByteArray ignored;
  return !args.isEmpty() && run(ffmpeg, args, ignored, error);
}

QImage sample(const QString &ffmpeg, const QString &path, double at,
              QString &error) {
  QByteArray bytes;
  if (!run(ffmpeg,
           {"-v", "error", "-ss", QString::number(at, 'f', 4), "-i", path,
            "-frames:v", "1", "-f", "image2pipe", "-c:v", "png", "-"},
           bytes, error))
    return {};
  return QImage::fromData(bytes, "PNG");
}

double audioEnergy(const QByteArray &pcm, double at) {
  const qsizetype begin = qRound64(at * 48000) * 2;
  const qsizetype end = qMin(pcm.size(), begin + 4800 * 2);
  if (end <= begin)
    return -1;
  double squares = 0;
  for (qsizetype i = begin; i < end; i += 2) {
    const double value =
        qFromLittleEndian<qint16>(pcm.constData() + i) / 32768.0;
    squares += value * value;
  }
  return std::sqrt(squares / static_cast<double>((end - begin) / 2));
}

// Inspect every output frame and audio sample, not just two representative
// timestamps: a removed frame or brief audio burst at a cut is still a leak.
bool removedPassageIsAbsent(const QString &ffmpeg, const QString &path,
                            int expectedFrames, QString &error,
                            bool blends = false) {
  QByteArray bytes;
  if (!run(ffmpeg,
           {"-v", "error", "-i", path, "-an", "-vf", "scale=1:1", "-pix_fmt",
            "rgb24", "-f", "rawvideo", "-"},
           bytes, error))
    return false;
  if (bytes.size() != expectedFrames * 3) {
    error = QStringLiteral("Ripple export contains %1 frames, expected %2.")
                .arg(bytes.size() / 3)
                .arg(expectedFrames);
    return false;
  }
  for (int frame = 0; frame < expectedFrames; ++frame) {
    const auto r = static_cast<unsigned char>(bytes[frame * 3]);
    const auto g = static_cast<unsigned char>(bytes[frame * 3 + 1]);
    const auto b = static_cast<unsigned char>(bytes[frame * 3 + 2]);
    if (g > 20 ||
        (!blends && (frame < expectedFrames / 2 ? r < 200 : b < 200))) {
      error =
          QStringLiteral("Deleted picture or wrong scene at output frame %1.")
              .arg(frame);
      return false;
    }
  }
  if (!run(ffmpeg,
           {"-v", "error", "-i", path, "-vn", "-ac", "1", "-ar", "48000", "-f",
            "s16le", "-"},
           bytes, error))
    return false;
  for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) {
    if (std::abs(static_cast<int>(
            qFromLittleEndian<qint16>(bytes.constData() + i))) > 2) {
      error = QStringLiteral("Deleted audio survives at output sample %1.")
                  .arg(i / 2);
      return false;
    }
  }
  return true;
}

bool runCutExportChecks(const QString &ffmpeg, const QTemporaryDir &scratch,
                        QString &error) {
  const QString sourcePath = scratch.filePath("cut-source.mkv");
  const QString outputPath = scratch.filePath("cut-output.mp4");
  QByteArray bytes;
  if (!run(ffmpeg,
           {"-v",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "color=red:s=320x180:r=30",
            "-f",
            "lavfi",
            "-i",
            "aevalsrc='if(between(t,1,1.99999),0.4*sin(2*PI*880*t),0)':s=48000:"
            "d=3",
            "-vf",
            "drawbox=c=green:t=fill:enable='gte(t,1)*lt(t,2)',"
            "drawbox=c=blue:t=fill:enable='gte(t,2)'",
            "-t",
            "3",
            "-c:v",
            "libx264",
            "-g",
            "1",
            "-pix_fmt",
            "yuv420p",
            "-c:a",
            "pcm_s16le",
            sourcePath},
           bytes, error))
    return false;
  StudioProject project;
  project.canvas = {320, 180};
  StudioSource source;
  source.size = project.canvas;
  source.fpsNumerator = 30;
  source.durationMs = 3000;
  source.audioStreams = 1;
  project.assets = {{1, sourcePath, source}};
  project.clips = {{1, 1, 0, 3000, 1}};
  StudioHistory history;
  StudioEditState before;
  before.project = project;
  before.positionMs = 1500;
  before.rangeIn = 1000;
  before.rangeOut = 2000;
  history.reset(before);
  if (!studioDeleteRange(project, 1000, 2000, 99).changed) {
    error = QStringLiteral("Interior ripple delete was rejected.");
    return false;
  }
  StudioEditState after = before;
  after.project = project;
  history.push(after);
  if (!exportProject(ffmpeg, project, outputPath, error) ||
      !removedPassageIsAbsent(ffmpeg, outputPath, 60, error))
    return false;
  // Transition handles are strictly inside surviving trimmed ranges. Even
  // their overlap must never bring back the excluded green picture/tone.
  StudioProject blendedCut = project;
  for (const auto kind :
       {StudioTransitionKind::Crossfade, StudioTransitionKind::FadeBlack}) {
    if (!studioSetTransition(blendedCut, 1, 99, kind, 300, error) ||
        !exportProject(ffmpeg, blendedCut, outputPath, error) ||
        !removedPassageIsAbsent(ffmpeg, outputPath, 51, error, true))
      return false;
  }
  if (!history.undo() || history.current() != before ||
      !exportProject(ffmpeg, history.current().project, outputPath, error)) {
    if (error.isEmpty())
      error = QStringLiteral("Undo did not restore the exact pre-cut project.");
    return false;
  }
  const auto restored = sample(ffmpeg, outputPath, 1.5, error);
  if (restored.isNull() || restored.pixelColor(160, 90).green() < 90 ||
      !run(ffmpeg,
           {"-v", "error", "-i", outputPath, "-vn", "-ac", "1", "-ar", "48000",
            "-f", "s16le", "-"},
           bytes, error) ||
      audioEnergy(bytes, 1.5) < 0.1) {
    if (error.isEmpty())
      error = QStringLiteral(
          "Undo export did not restore deleted picture and audio.");
    return false;
  }
  if (!history.redo() || history.current() != after) {
    error = QStringLiteral("Redo did not restore the ripple deletion.");
    return false;
  }
  // One cut across two occurrences of the same source removes both middle
  // tones and the scene boundary; surviving endpoints remain independent.
  project = before.project;
  project.clips.push_back({2, 1, 0, 3000, 1});
  if (!studioDeleteRange(project, 500, 5500, 99).changed ||
      !exportProject(ffmpeg, project, outputPath, error) ||
      !removedPassageIsAbsent(ffmpeg, outputPath, 30, error)) {
    if (error.isEmpty())
      error = QStringLiteral("Cross-scene ripple deletion failed.");
    return false;
  }
  if (!studioDeleteRange(project, 0, studioDuration(project), 100).changed ||
      !project.clips.isEmpty() ||
      !studioCompositionArguments(project, outputPath, error).isEmpty()) {
    error = QStringLiteral(
        "Deleting all remaining scenes did not produce an empty project.");
    return false;
  }
  error.clear();
  return true;
}

bool runSceneExportChecks(const QString &ffmpeg,
                          const QVector<StudioAsset> &assets,
                          const QTemporaryDir &scratch, QString &error) {
  StudioProject project;
  // The import controller picks the initial canvas once. Structural edits
  // must retain it, including when later clips use a different shape/FPS.
  project.canvas = assets.first().source.size;
  project.fpsNumerator = assets.first().source.fpsNumerator;
  project.fpsDenominator = assets.first().source.fpsDenominator;
  const auto checked = [&error](bool result, const char *message) {
    if (!result && error.isEmpty())
      error = QString::fromLatin1(message);
    return result;
  };
  if (!checked(studioInsertScenes(
                   project, assets,
                   {{1, 1, 0, 1000, 1}, {2, 2, 0, 1000, 1}, {3, 3, 0, 1000, 1}},
                   0, error),
               "three-scene import was rejected") ||
      !checked(studioMoveClip(project, 3, 1, error),
               "scene reorder was rejected") ||
      !checked(studioDuplicateClip(project, 3, 4, error),
               "scene duplicate was rejected") ||
      !checked(studioTrimClip(project, 4, 250, 750, error),
               "independent scene trim was rejected"))
    return false;
  StudioHistory history;
  StudioEditState before;
  before.project = project;
  before.selectedClip = 4;
  before.positionMs = 1200;
  history.reset(before);
  if (!checked(studioMoveClip(project, 4, 0, error),
               "scene append reorder was rejected"))
    return false;
  StudioEditState after = before;
  after.project = project;
  after.positionMs = 3200;
  history.push(after);
  if (!checked(history.undo() && history.current() == before &&
                   history.redo() && history.current() == after,
               "scene reorder undo/redo lost composition or selection"))
    return false;
  const QString projectPath = scratch.filePath("combined.omasnap-project.json");
  error = saveStudioProject(projectPath, project);
  if (!error.isEmpty())
    return false;
  const auto reopened = loadStudioProject(projectPath);
  if (!checked(reopened.error.isEmpty() && reopened.missingAssets.isEmpty() &&
                   reopened.project == project &&
                   studioDuration(reopened.project) == 3500 &&
                   project.canvas == assets.first().source.size &&
                   project.fpsNumerator == 24,
               "combined scene save/reopen or locked canvas/FPS changed"))
    return false;
  const QString outputPath = scratch.filePath("combined.mp4");
  if (!exportProject(ffmpeg, reopened.project, outputPath, error))
    return false;
  const QPoint centre(project.canvas.width() / 2, project.canvas.height() / 2);
  const auto green = sample(ffmpeg, outputPath, 0.5, error);
  const auto red = sample(ffmpeg, outputPath, 1.5, error);
  const auto blue = sample(ffmpeg, outputPath, 2.5, error);
  const auto duplicate = sample(ffmpeg, outputPath, 3.25, error);
  if (!checked(!green.isNull() && !red.isNull() && !blue.isNull() &&
                   !duplicate.isNull() && green.size() == project.canvas &&
                   green.pixelColor(centre).green() > 90 &&
                   red.pixelColor(centre).red() > 200 &&
                   blue.pixelColor(centre).blue() > 200 &&
                   duplicate.pixelColor(centre).green() > 90,
               "import/reorder/duplicate/trim output scene sequence differed"))
    return false;
  QByteArray audio;
  if (!run(ffmpeg,
           {"-v", "error", "-i", outputPath, "-vn", "-ac", "1", "-ar", "48000",
            "-f", "s16le", "-"},
           audio, error))
    return false;
  return checked(
      std::abs(audio.size() / 96000.0 - 3.5) < 0.04 &&
          audioEnergy(audio, 0.5) > 0.01 && audioEnergy(audio, 1.5) > 0.01 &&
          audioEnergy(audio, 2.5) < 0.0001 && audioEnergy(audio, 3.25) > 0.01,
      "reopened scene audio order or duration differed");
}

bool runTransitionExportChecks(const QString &ffmpeg,
                               const QVector<StudioAsset> &assets,
                               const QTemporaryDir &scratch, QString &error) {
  const auto checked = [&error](bool ok, const char *message) {
    if (!ok && error.isEmpty())
      error = QString::fromLatin1(message);
    return ok;
  };
  StudioProject project;
  project.canvas = {320, 180};
  project.assets = assets;
  project.clips = {{1, 1, 0, 2000, 1}, {2, 2, 0, 2000, 1}};
  const QString path = scratch.filePath("transition.mp4");
  for (const auto kind :
       {StudioTransitionKind::Crossfade, StudioTransitionKind::FadeBlack}) {
    if (!checked(studioSetTransition(project, 1, 2, kind, 800, error),
                 "transition setup failed") ||
        !exportProject(ffmpeg, project, path, error))
      return false;
    const auto spans = studioComposition(project);
    const auto *transition = studioTransition(project, 1, 2);
    for (int quarter = 0; quarter <= 4; ++quarter) {
      const qint64 at = spans[1].startMs + transition->durationMs * quarter / 4;
      const auto blend = studioBlendAt(project, at);
      const double outgoing = blend ? blend->outgoingOpacity : 0;
      const double incoming = blend ? blend->incomingOpacity : 1;
      const auto frame = sample(ffmpeg, path, at / 1000.0, error);
      if (!checked(!frame.isNull(), "transition frame was not decoded"))
        return false;
      const auto seen = frame.pixelColor(160, 90);
      if (std::abs(seen.red() - qRound(254 * outgoing)) > 10 ||
          std::abs(seen.blue() - qRound(254 * incoming)) > 10 ||
          seen.green() > 10) {
        error = QStringLiteral("Transition %1 quarter %2 differs from model: "
                               "RGB %3,%4,%5 vs weights %6,%7")
                    .arg(static_cast<int>(kind))
                    .arg(quarter)
                    .arg(seen.red())
                    .arg(seen.green())
                    .arg(seen.blue())
                    .arg(outgoing)
                    .arg(incoming);
        return false;
      }
    }
    QByteArray audio;
    if (!run(ffmpeg,
             {"-v", "error", "-i", path, "-vn", "-ac", "1", "-ar", "48000",
              "-f", "s16le", "-"},
             audio, error))
      return false;
    const double baseline = audioEnergy(audio, 0.3);
    for (int quarter = 1; quarter <= 3; ++quarter) {
      const double at =
          (spans[1].startMs + transition->durationMs * quarter / 4) / 1000.0;
      const double ratio = audioEnergy(audio, at - 0.05) / baseline;
      if (!checked(std::abs(ratio - (1 - quarter / 4.0)) < 0.05,
                   "transition audio gain does not match linear model"))
        return false;
    }
    if (!checked(std::abs(audio.size() / 96000.0 -
                          studioDuration(project) / 1000.0) < 0.04 &&
                     audioEnergy(audio, 2.5) < 0.0001,
                 "transition audio length or silent incoming clip differed"))
      return false;
  }
  // Alternate transitions and hard cuts, with deliberately non-frame-aligned
  // source durations and rational output FPS. Absolute endpoints own joins.
  project.transitions.clear();
  project.fpsNumerator = 30000;
  project.fpsDenominator = 1001;
  project.clips = {{1, 1, 0, 1333, 1},
                   {2, 2, 0, 1777, 1},
                   {3, 3, 0, 1555, 1},
                   {4, 1, 0, 1111, 1}};
  if (!studioSetTransition(project, 1, 2, StudioTransitionKind::Crossfade, 400,
                           error) ||
      !studioSetTransition(project, 3, 4, StudioTransitionKind::FadeBlack, 400,
                           error) ||
      !exportProject(ffmpeg, project, path, error))
    return false;
  const auto spans = studioComposition(project);
  // The hard cut between blue and green must not inherit the previous
  // overlap's frame-rounding error, or the third color arrives late.
  const auto before =
      sample(ffmpeg, path, (spans[2].startMs - 80) / 1000.0, error);
  const auto after =
      sample(ffmpeg, path, (spans[2].startMs + 80) / 1000.0, error);
  if (!checked(!before.isNull() && !after.isNull() &&
                   before.pixelColor(160, 90).blue() > 200 &&
                   after.pixelColor(160, 90).green() > 90,
               "hard cut amid transitions shifted to the wrong time"))
    return false;
  QByteArray audio;
  if (!run(ffmpeg,
           {"-v", "error", "-i", path, "-vn", "-ac", "1", "-ar", "48000", "-f",
            "s16le", "-"},
           audio, error) ||
      !checked(std::abs(audio.size() / 96000.0 -
                        studioDuration(project) / 1000.0) < 0.04,
               "mixed transition/cut composition duration drifted"))
    return false;
  QByteArray frames;
  if (!run(ffmpeg,
           {"-v", "error", "-i", path, "-an", "-vf", "scale=1:1", "-pix_fmt",
            "rgb24", "-f", "rawvideo", "-"},
           frames, error))
    return false;
  const qint64 expected = qCeil(studioDuration(project) / 1000.0 *
                                project.fpsNumerator / project.fpsDenominator);
  if (!checked(std::abs(frames.size() / 3 - expected) <= 1 &&
                   frames.size() >= 3 &&
                   static_cast<unsigned char>(frames[frames.size() - 3]) > 200,
               "mixed transition video frame count or ending picture differed"))
    return false;
  return true;
}
} // namespace

bool runStudioCompositionChecks(QString &error) {
  const auto require = [&error](bool ok, const char *message) {
    if (!ok && error.isEmpty())
      error = QString::fromLatin1(message);
    return ok;
  };
  StudioProject empty;
  if (!require(studioCompositionArguments(empty, "out.mp4", error).isEmpty(),
               "empty composition was exportable"))
    return false;
  error.clear();
  const QString ffmpeg =
      QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
  const QString ffprobe =
      QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
  if (ffmpeg.isEmpty() || ffprobe.isEmpty()) {
    qInfo("studio composition smoke: ffmpeg/ffprobe absent; skipping decoded "
          "export checks");
    return true;
  }
  QTemporaryDir scratch;
  if (!require(scratch.isValid(),
               "could not create composition scratch directory"))
    return false;
  StudioProject project;
  project.canvas = {320, 180};
  const QList<QSize> sizes{{160, 90}, {90, 160}, {320, 180}};
  const QList<int> rates{24, 30, 60};
  const QStringList colors{"red", "blue", "green"};
  QByteArray output;
  for (int i = 0; i < 3; ++i) {
    const QString path =
        scratch.filePath(QStringLiteral("source-%1.mp4").arg(i));
    QStringList args{"-v",
                     "error",
                     "-y",
                     "-f",
                     "lavfi",
                     "-i",
                     QStringLiteral("color=%1:s=%2x%3:r=%4")
                         .arg(colors[i])
                         .arg(sizes[i].width())
                         .arg(sizes[i].height())
                         .arg(rates[i])};
    if (i != 1)
      args << "-f" << "lavfi" << "-i" << "sine=frequency=880:sample_rate=48000";
    if (i == 2)
      args << "-f" << "lavfi" << "-i" << "sine=frequency=440:sample_rate=48000";
    args << "-map" << "0:v";
    if (i != 1)
      args << "-map" << "1:a";
    if (i == 2)
      args << "-map" << "2:a";
    args << "-t" << "2" << "-c:v" << "libx264" << "-g" << "1"
         << "-pix_fmt" << "yuv420p" << "-c:a" << "aac" << path;
    if (!run(ffmpeg, args, output, error))
      return false;
    StudioSource source;
    source.size = sizes[i];
    source.fpsNumerator = rates[i];
    source.durationMs = 2000;
    source.audioStreams = i == 1 ? 0 : i == 2 ? 2 : 1;
    project.assets.push_back({static_cast<quint64>(i + 1), path, source});
  }
  if (!runSceneExportChecks(ffmpeg, project.assets, scratch, error))
    return false;
  if (!runTransitionExportChecks(ffmpeg, project.assets, scratch, error))
    return false;
  project.clips = {{1, 1, 500, 1500, 2},
                   {2, 2, 500, 1500, 1},
                   {3, 1, 0, 500, 1},
                   {4, 3, 0, 1000, 0.5}};
  const QString path = scratch.filePath("composition.mp4");
  if (!exportProject(ffmpeg, project, path, error))
    return false;
  const auto red = sample(ffmpeg, path, 0.2, error);
  const auto blue = sample(ffmpeg, path, 0.8, error);
  const auto repeated = sample(ffmpeg, path, 1.7, error);
  const auto green = sample(ffmpeg, path, 3.0, error);
  if (!require(!red.isNull() && !blue.isNull() && !repeated.isNull() &&
                   !green.isNull(),
               "composition output frame was missing"))
    return false;
  if (!require(
          red.size() == project.canvas && red.pixelColor(160, 90).red() > 200 &&
              blue.pixelColor(160, 90).blue() > 200 &&
              blue.pixelColor(10, 90).red() < 10 &&
              blue.pixelColor(10, 90).blue() < 10 &&
              repeated.pixelColor(160, 90).red() > 200 &&
              green.pixelColor(160, 90).green() > 90,
          "scene order, speed, repeated source, or fit-to-canvas differed"))
    return false;
  if (!run(ffmpeg,
           {"-v", "error", "-i", path, "-vn", "-ac", "1", "-ar", "48000", "-f",
            "s16le", "-"},
           output, error))
    return false;
  if (!require(std::abs(output.size() / 96000.0 - 4.0) < 0.04 &&
                   audioEnergy(output, 0.15) > 0.01 &&
                   audioEnergy(output, 0.8) < 0.0001 &&
                   audioEnergy(output, 1.7) > 0.01 &&
                   audioEnergy(output, 3.0) > 0.01,
               "retimed audio, silent clip, or project duration differed"))
    return false;
  // Fractional frame clip lengths must not accumulate a per-scene rounding
  // error. Audio anchors exact scene milliseconds before one final CFR pass.
  project.clips.clear();
  for (quint64 i = 0; i < 10; ++i)
    project.clips.push_back({i + 1, 1, 0, 333, 1});
  if (!exportProject(ffmpeg, project, path, error) ||
      !run(ffprobe,
           {"-v", "error", "-show_entries", "format=duration", "-of",
            "default=noprint_wrappers=1:nokey=1", path},
           output, error))
    return false;
  if (!require(std::abs(output.trimmed().toDouble() - 3.33) < 0.04,
               "fractional scene durations accumulated frame rounding drift"))
    return false;
  // Camera coordinates address the canonical frame, including its fit bars.
  // Export trimming must not restart the camera clock at zero.
  project.clips = {{1, 2, 0, 1000, 1}};
  project.style = {2, 10, 32};
  project.trimInMs = 200;
  project.trimOutMs = 800;
  project.zoom.cues = {{1, 0, 1000, 60, 60, {0.5, 0.5}, 2}};
  if (!exportProject(ffmpeg, project, path, error))
    return false;
  const auto styled = sample(ffmpeg, path, 0.3, error);
  if (!require(!styled.isNull() && styled.pixelColor(90, 90).blue() > 200 &&
                   std::abs(styled.pixelColor(4, 4).red() -
                            project.style.color().red()) < 8,
               "canonical zoom, project-time trim, or styled canvas differed"))
    return false;
  StudioPreview preview;
  preview.resize(320, 180);
  preview.setCanvasSize(project.canvas);
  preview.setStyle(project.style);
  preview.setTrack(&project.zoom);
  preview.setPosition(500);
  preview.setPickable(false);
  preview.setFrame(sample(ffmpeg, project.assets[1].path, 0.5, error));
  const QImage shown = preview.grab().toImage().scaled(project.canvas);
  double difference = 0;
  for (int y = 0; y < 180; y += 3)
    for (int x = 0; x < 320; x += 3) {
      const auto a = shown.pixelColor(x, y), b = styled.pixelColor(x, y);
      difference += std::abs(a.red() - b.red()) +
                    std::abs(a.green() - b.green()) +
                    std::abs(a.blue() - b.blue());
    }
  difference /= 60 * 107 * 3;
  if (!require(difference < 10,
               "mixed-aspect canvas camera/style preview and export disagree"))
    return false;
  // A changing cadence retains timestamps; frame-index-based concatenation
  // would move the blue/green changes and shorten this two-second source.
  const QString vfrPath = scratch.filePath("vfr.mp4");
  if (!run(ffmpeg,
           {"-v", "error", "-y", "-f", "lavfi", "-i",
            "color=red:s=320x180:r=60", "-vf",
            "drawbox=c=blue:t=fill:enable='gte(t,0.5)*lt(t,1)',"
            "drawbox=c=green:t=fill:enable='gte(t,1)',"
            "select='not(mod(n,if(lt(t,1),3,2)))'",
            "-t", "2", "-fps_mode", "vfr", "-c:v", "libx264", "-g", "1",
            vfrPath},
           output, error))
    return false;
  StudioSource vfr;
  vfr.size = {320, 180};
  vfr.fpsNumerator = 60;
  vfr.durationMs = 2000;
  project.assets = {{1, vfrPath, vfr}};
  project.clips = {{1, 1, 0, 2000, 2}};
  project.zoom = {};
  project.style = {};
  project.trimInMs = 0;
  project.trimOutMs = -1;
  if (!exportProject(ffmpeg, project, path, error))
    return false;
  const auto vfrRed = sample(ffmpeg, path, 0.1, error);
  const auto vfrBlue = sample(ffmpeg, path, 0.35, error);
  const auto vfrGreen = sample(ffmpeg, path, 0.75, error);
  if (!require(!vfrRed.isNull() && !vfrBlue.isNull() && !vfrGreen.isNull() &&
                   vfrRed.pixelColor(160, 90).red() > 200 &&
                   vfrBlue.pixelColor(160, 90).blue() > 200 &&
                   vfrGreen.pixelColor(160, 90).green() > 90,
               "VFR timestamp normalization or speed mapping differed"))
    return false;
  project.clips = {{1, 1, 0, 2000, 1}, {2, 1, 0, 2000, 2}};
  if (!studioSetTransition(project, 1, 2, StudioTransitionKind::Crossfade, 333,
                           error) ||
      !exportProject(ffmpeg, project, path, error))
    return false;
  const auto spans = studioComposition(project);
  const auto *transition = studioTransition(project, 1, 2);
  const qint64 midpoint = spans[1].startMs + transition->durationMs / 2;
  const auto weights = studioBlendAt(project, midpoint);
  const auto blended = sample(ffmpeg, path, midpoint / 1000.0, error);
  if (!require(
          weights && !blended.isNull() &&
              std::abs(blended.pixelColor(160, 90).red() -
                       qRound(254 * weights->incomingOpacity)) < 12 &&
              std::abs(blended.pixelColor(160, 90).green() -
                       qRound(128 * weights->outgoingOpacity)) < 12,
          "retimed VFR transition did not sample both sources at project time"))
    return false;
  project.transitions.clear();
  project.clips = {{1, 1, 0, 2000, 2}};
  for (quint64 id = 2; id <= 65; ++id)
    project.clips.push_back({id, 1, 0, 2000, 1});
  if (!require(studioCompositionArguments(project, path, error).isEmpty() &&
                   error.contains(QStringLiteral("64 scenes")),
               "large export graph was not bounded"))
    return false;
  error.clear();
  return runCutExportChecks(ffmpeg, scratch, error);
}
