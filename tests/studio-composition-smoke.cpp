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
  for (quint64 id = 2; id <= 65; ++id)
    project.clips.push_back({id, 1, 0, 2000, 1});
  if (!require(studioCompositionArguments(project, path, error).isEmpty() &&
                   error.contains(QStringLiteral("64 scenes")),
               "large export graph was not bounded"))
    return false;
  error.clear();
  return true;
}
