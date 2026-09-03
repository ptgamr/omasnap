#include "capture.hpp"
#include "cli-path.hpp"
#include "editor.hpp"
#include "instance-lock.hpp"
#include "overlay-chrome.hpp"
#include "pin.hpp"
#include "quit-signals.hpp"
#include "record.hpp"
#include "record-target.hpp"
#include "recent-snaps.hpp"
#include "startup-timing.hpp"

#include <LayerShellQt/Window>


#include <QImageReader>
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLockFile>
#include <QScreen>
#include <QSocketNotifier>
#include <QUrl>
#include <QWindow>

#include <optional>


int main(int argc, char **argv) {
  startupTimingMark("entered main");
  QCoreApplication::setApplicationName(QStringLiteral("omasnap"));
  QCoreApplication::setApplicationVersion(QString::fromLatin1(OMASNAP_VERSION));
  QCoreApplication::setOrganizationName(QStringLiteral("Omarchy"));
  qputenv("QT_WAYLAND_SHELL_INTEGRATION", "layer-shell");
  // Omarchy exports QT_QPA_PLATFORMTHEME=gtk3 session-wide. Honouring it
  // loads the qgtk3 plugin, which initialises GTK inside this process
  // (measured 81-112 ms of QApplication construction, plus ~20-24 MiB of
  // RSS) for a hand-painted overlay that opens no dialogs and reads no palette.
  // Qt's built-in generic theme is all it needs, so select it by name
  // (an empty value would let Qt pick a theme from XDG_CURRENT_DESKTOP
  // instead). The chrome font is pinned in chromeFont() rather than taken
  // from the theme. `-platformtheme gtk3` on the command line still
  // overrides this for debugging.
  qputenv("QT_QPA_PLATFORMTHEME", "generic");
  QGuiApplication::setDesktopFileName(QStringLiteral("omasnap"));
  QApplication application(argc, argv);
  startupTimingMark("QApplication constructed");
  // With the external desktop theme bypassed, Qt's default font would be
  // generic "Sans Serif 9"; pin what the theme used to install before any
  // widget is created, so painter/widget default-font text keeps its size and
  // face.
  QApplication::setFont(chromeDefaultFont());

  // A stitched scroll capture (or any tall pinned image) exceeds Qt's default
  // 256 MB image-decode allocation limit; lift it so --file/--pin can open it.
  QImageReader::setAllocationLimit(0);
  PosixSignalNotifier signalNotifier(&application);

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Native Wayland screenshot and annotation overlay for Hyprland and "
      "Omarchy.\n"
      "\n"
      "Only one capture overlay runs at a time. Starting omasnap again while "
      "an\noverlay is open dismisses it: the running instance is asked to "
      "quit and the\nnew process exits without capturing, so the same hotkey "
      "opens and closes the\noverlay. Quick output (--copy, --save) dismisses "
      "it the same way instead of\nscreenshotting the overlay. With --file (or "
      "an image path) or --clipboard, the running\ninstance is stopped and "
      "the editor opens on that image instead.\n"
      "\n"
      "Recording: add --record to a capture mode (region, windows, "
      "fullscreen) to\nrecord that target instead of screenshotting it. The "
      "selector hands off to a\nrecorder process, so screenshots keep working "
      "while a recording runs.\n"
      "\n"
      "Exit codes: 0 success, including dismissing a running overlay; 1 "
      "capture,\nimage, or single-instance lock failure; 2 usage error; 3 an "
      "overlay was\nalready open when a recording was requested."));
  parser.addHelpOption();
  parser.addVersionOption();
  const QCommandLineOption fullscreenOption(
      QStringLiteral("capture-fullscreen"),
      QStringLiteral("Start with the entire focused monitor selected."));
  const QCommandLineOption windowOption(
      {QStringLiteral("capture-window"), QStringLiteral("capture-windows")},
      QStringLiteral("Start in window selection mode."));
  const QCommandLineOption regionOption(
      QStringLiteral("capture-region"),
      QStringLiteral("Start in freeform region selection mode (default)."));
  parser.addOption(fullscreenOption);
  parser.addOption(windowOption);
  parser.addOption(regionOption);
  const QCommandLineOption copyOption(
      QStringLiteral("copy"),
      QStringLiteral("Copy the capture directly without opening the editor."));
  const QCommandLineOption saveOption(
      QStringLiteral("save"),
      QStringLiteral("Save the capture directly without opening the editor."));
  parser.addOption(copyOption);
  parser.addOption(saveOption);
  const QCommandLineOption fileOption(
      QStringLiteral("file"),
      QStringLiteral("Open an existing image file in the annotation editor "
                     "instead of capturing the screen."),
      QStringLiteral("path"));
  parser.addOption(fileOption);
  const QCommandLineOption clipboardOption(
      QStringLiteral("clipboard"),
      QStringLiteral("Open the current clipboard image in the annotation "
                     "editor instead of capturing the screen."));
  parser.addOption(clipboardOption);
  const QCommandLineOption pinOption(
      QStringLiteral("pin"),
      QStringLiteral("Show an image as a pinned always-visible layer."),
      QStringLiteral("path"));
  parser.addOption(pinOption);
  const QCommandLineOption scrollOption(
      QStringLiteral("scroll"),
      QStringLiteral("Capture a scrolling region and stitch it into one tall "
                     "image, then open it in the editor."));
  parser.addOption(scrollOption);
  const QCommandLineOption recordOption(
      QStringLiteral("record"),
      QStringLiteral("Record the selected target as video instead of taking a "
                     "screenshot."));
  parser.addOption(recordOption);
  const QCommandLineOption stopOption(
      QStringLiteral("stop"),
      QStringLiteral("Stop and save the running recording (--record only)."));
  parser.addOption(stopOption);
  const QCommandLineOption audioOption(
      QStringLiteral("audio"),
      QStringLiteral("Record desktop sound as well (--record only)."));
  parser.addOption(audioOption);
  const QCommandLineOption micOption(
      QStringLiteral("mic"),
      QStringLiteral("Record the microphone as well (--record only)."));
  parser.addOption(micOption);
  const QCommandLineOption fpsOption(
      QStringLiteral("fps"),
      QStringLiteral("Recording frame rate (default 60)."),
      QStringLiteral("frames"), QStringLiteral("60"));
  parser.addOption(fpsOption);
  // The recorder half of --record: a second process, started by the first,
  // that owns the encoder and shows the indicator. Not something to type.
  QCommandLineOption recordRunOption(
      QStringLiteral("record-run"),
      QStringLiteral("Internal: run the recorder on a written target file."),
      QStringLiteral("path"));
  recordRunOption.setFlags(QCommandLineOption::HiddenFromHelp);
  parser.addOption(recordRunOption);
  parser.addPositionalArgument(
      QStringLiteral("target"),
      QStringLiteral("Capture mode (smart, region, windows, fullscreen) or the "
                     "path of an image file to edit."),
      QStringLiteral("[target]"));
  parser.process(application);
  startupTimingMark("command line parsed");

  bool fpsValid = false;
  const int fps = parser.value(fpsOption).toInt(&fpsValid);
  if (!fpsValid || fps < 1 || fps > 500) {
    qCritical() << "--fps takes a frame rate between 1 and 500";
    return 2;
  }
  RecordOptions recordOptions;
  recordOptions.systemAudio = parser.isSet(audioOption);
  recordOptions.microphone = parser.isSet(micOption);
  recordOptions.fps = fps;

  // The recorder never touches the screenshot instance lock, the capture
  // fonts, or a monitor grab: it has a target already and only needs a layer
  // surface for the indicator.
  if (parser.isSet(recordRunOption))
    return runRecorder(parser.value(recordRunOption), recordOptions,
                       &signalNotifier);

  QString filePath = parser.value(fileOption);
  const bool clipboardInput = parser.isSet(clipboardOption);

  QuickOutputMode quickOutputMode = QuickOutputMode::None;
  if (parser.isSet(copyOption) && parser.isSet(saveOption))
    quickOutputMode = QuickOutputMode::Both;
  else if (parser.isSet(copyOption))
    quickOutputMode = QuickOutputMode::Copy;
  else if (parser.isSet(saveOption))
    quickOutputMode = QuickOutputMode::Save;

  CaptureEditor::CaptureMode captureMode = CaptureEditor::CaptureMode::Region;
  int requestedModes = parser.isSet(fullscreenOption) +
                       parser.isSet(windowOption) + parser.isSet(regionOption) +
                       parser.isSet(scrollOption);
  if (parser.isSet(fullscreenOption))
    captureMode = CaptureEditor::CaptureMode::Fullscreen;
  else if (parser.isSet(windowOption))
    captureMode = CaptureEditor::CaptureMode::Window;
  else if (parser.isSet(scrollOption))
    captureMode = CaptureEditor::CaptureMode::Scroll;

  const QStringList positional = parser.positionalArguments();
  if (parser.isSet(pinOption)) {
    if (!filePath.isEmpty() || clipboardInput || requestedModes > 0 ||
        !positional.isEmpty() || quickOutputMode != QuickOutputMode::None ||
        parser.isSet(recordOption)) {
      qCritical()
          << "Pinned mode cannot be combined with capture or edit targets";
      return 2;
    }
    QString pinPath = QUrl(parser.value(pinOption)).toLocalFile();
    if (pinPath.isEmpty())
      pinPath = parser.value(pinOption);
    return runPinnedCapture(pinPath);
  }
  if (positional.size() > 1) {
    qCritical() << "Only one capture target may be specified";
    return 2;
  }
  if (!positional.isEmpty()) {
    const QString localTarget = resolveLocalImagePath(positional.first());
    if (filePath.isEmpty() && !localTarget.isEmpty()) {
      filePath = localTarget;
    } else {
      ++requestedModes;
      const QString mode = positional.first();
      if (mode == QStringLiteral("fullscreen"))
        captureMode = CaptureEditor::CaptureMode::Fullscreen;
      else if (mode == QStringLiteral("windows") ||
               mode == QStringLiteral("window"))
        captureMode = CaptureEditor::CaptureMode::Window;
      else if (mode == QStringLiteral("smart") ||
               mode == QStringLiteral("region"))
        captureMode = CaptureEditor::CaptureMode::Region;
      else if (mode == QStringLiteral("scroll"))
        captureMode = CaptureEditor::CaptureMode::Scroll;
      else {
        qCritical().noquote()
            << QStringLiteral("Unknown capture target: %1").arg(mode);
        return 2;
      }
    }
  }
  if (filePath.isEmpty() && requestedModes > 1) {
    qCritical() << "Capture mode options are mutually exclusive";
    return 2;
  }
  if (!filePath.isEmpty() && requestedModes > 0) {
    qCritical() << "An image file cannot be combined with a capture mode";
    return 2;
  }
  if (clipboardInput && (!filePath.isEmpty() || requestedModes > 0)) {
    qCritical() << "Clipboard input cannot be combined with another target";
    return 2;
  }
  const bool editingImage = clipboardInput || !filePath.isEmpty();
  if (editingImage && quickOutputMode != QuickOutputMode::None) {
    qCritical()
        << "Quick output options cannot be combined with an image input";
    return 2;
  }

  const bool recording = parser.isSet(recordOption);
  if (!recording && (parser.isSet(audioOption) || parser.isSet(micOption) ||
                     parser.isSet(stopOption) || parser.isSet(fpsOption))) {
    qCritical() << "--audio, --mic, --fps and --stop only apply to --record";
    return 2;
  }
  if (recording && parser.isSet(stopOption)) {
    if (requestedModes > 0 || editingImage) {
      qCritical() << "--record --stop takes no capture target";
      return 2;
    }
    QString stopError;
    if (!stopActiveRecording(stopError)) {
      qCritical().noquote() << stopError;
      return 1;
    }
    return 0;
  }
  if (recording) {
    if (editingImage || quickOutputMode != QuickOutputMode::None) {
      qCritical() << "--record cannot be combined with an image input or "
                     "quick output";
      return 2;
    }
    if (captureMode == CaptureEditor::CaptureMode::Scroll) {
      qCritical() << "--record cannot be combined with scrolling capture";
      return 2;
    }
    // A whole display needs no selector, so it needs no overlay, no monitor
    // grab, and no instance lock: an open annotation session is left alone
    // and recording starts immediately.
    if (captureMode == CaptureEditor::CaptureMode::Fullscreen) {
      MonitorInfo monitor;
      QString probeError;
      if (!probeFocusedMonitor(monitor, probeError)) {
        qCritical().noquote() << probeError;
        return 1;
      }
      QString handoffError;
      if (!handOffToRecorder(makeRecordTarget(monitor,
                                              RecordTargetKind::Fullscreen, {}),
                             recordOptions, handoffError)) {
        qCritical().noquote() << handoffError;
        return 1;
      }
      return 0;
    }
  }
  startupTimingMark("options resolved");
  if (!loadCaptureFonts())
    return 1;
  startupTimingMark("capture font loaded");
  application.setQuitOnLastWindowClosed(true);

  const QString runtime = secureRuntimeDirectory();
  startupTimingMark("runtime directory ready");
  if (runtime.isEmpty()) {
    qCritical() << "Could not create private runtime directory";
    return 1;
  }
  QLockFile instanceLock(
      QDir(runtime).filePath(QStringLiteral("omasnap.instance")));
  // Every capture, quick output included, dismisses a running overlay instead
  // of starting a second one: a late capture would otherwise photograph that overlay.
  // Editing an image always takes over so the requested editor can open.
  const InstanceLockResult lockResult = acquireInstanceLock(
      instanceLock, editingImage  ? InstanceMode::EditFile
                    : recording   ? InstanceMode::RecordTarget
                                  : InstanceMode::Capture);
  startupTimingMark("instance lock acquired");
  if (lockResult.signalledPid != 0)
    qInfo().noquote() << QStringLiteral("Asked the running omasnap (pid %1) to "
                                        "quit")
                             .arg(lockResult.signalledPid);
  if (!lockResult.proceed) {
    if (!lockResult.error.isEmpty())
      qCritical().noquote() << lockResult.error;
    return lockResult.exitCode;
  }

  CaptureData capture;
  OperationLog restoredLog;
  QString error;
  if (editingImage) {
    QImage image;
    QString inputName;
    if (clipboardInput) {
      if (!loadClipboardImage(image, error)) {
        const QString message =
            QStringLiteral("Could not load clipboard image: %1").arg(error);
        qCritical().noquote() << message;
        sendCaptureNotification(message);
        return 1;
      }
      inputName = QStringLiteral("clipboard image");
    } else {
      QString localFile = QUrl(filePath).toLocalFile();
      if (localFile.isEmpty())
        localFile = filePath;
      image.load(localFile);
      if (image.isNull()) {
        qCritical().noquote()
            << QStringLiteral("Could not load image: %1").arg(filePath);
        return 1;
      }
      inputName = localFile;
      const QString sidecar = operationLogPath(localFile);
      if (QFile::exists(sidecar) &&
          !loadOperationLog(sidecar, restoredLog, error)) {
        qCritical().noquote()
            << QStringLiteral("Could not restore operation log: %1").arg(error);
        return 1;
      }
    }
    describeFileCapture(capture, image, restoredLog);
    captureMode = CaptureEditor::CaptureMode::File;
    qInfo().noquote() << QStringLiteral("Opened %1 for annotation (%2x%3)")
                             .arg(inputName)
                             .arg(image.width())
                             .arg(image.height());
  } else if (!probeFocusedMonitor(capture.monitor, error)) {
    qCritical().noquote() << error;
    sendCaptureNotification(QStringLiteral("Screenshot failed: %1").arg(error));
    return 1;
  }
  startupTimingMark(editingImage ? "input image prepared"
                                 : "focused monitor probed");

  // Grab the output before the layer exists. ext-image-copy-capture waits for
  // a composited frame, so mapping the dim overlay first photographs the veil.
  const bool instantFullscreenOutput =
      !editingImage && captureMode == CaptureEditor::CaptureMode::Fullscreen &&
      quickOutputMode != QuickOutputMode::None;
  if (!editingImage &&
      !captureMonitorPixels(capture.monitor, capture,
                            !instantFullscreenOutput, error)) {
    qCritical().noquote() << error;
    sendCaptureNotification(QStringLiteral("Screenshot failed: %1").arg(error));
    return 1;
  }
  startupTimingMark(editingImage ? "pixel capture skipped"
                                 : "monitor pixels captured");

  if (instantFullscreenOutput) {
    QString outputError;
    const QSize expectedSize(
        qRound(capture.previewSize.width() * capture.monitor.scale),
        qRound(capture.previewSize.height() * capture.monitor.scale));
    const QImage output = capture.monitor.scale <= 1.0 ||
                                  capture.source.size() == expectedSize
                              ? capture.source
                              : renderCapture(capture,
                                              QRectF(QPointF(), capture.previewSize), {},
                                              BackgroundStyle::None);
    if (!quickOutput(output, quickOutputMode, outputError)) {
      qCritical().noquote() << outputError;
      return 1;
    }
    return 0;
  }

  QScreen *targetScreen = QGuiApplication::primaryScreen();
  for (QScreen *screen : QGuiApplication::screens()) {
    if (screen->name() == capture.monitor.name) {
      targetScreen = screen;
      break;
    }
  }

  if (!editingImage) {
    qInfo().noquote() << QStringLiteral(
                             "Captured %1 workspace %2 with %3 selectable "
                             "windows")
                             .arg(capture.monitor.name)
                             .arg(capture.monitor.workspaceId)
                             .arg(capture.windows.size());
  }

  CaptureEditor editor(std::move(capture), captureMode, quickOutputMode,
                       restoredLog);
  startupTimingMark("CaptureEditor constructed");
  RecordTarget chosenTarget;
  if (recording) {
    editor.setRecordTargetMode(true);
    QObject::connect(
        &editor, &CaptureEditor::recordTargetSelected, &editor,
        [&](const QRectF &selection, CaptureKind kind) {
          const CaptureData &data = editor.captureData();
          const RecordTargetKind targetKind =
              kind == CaptureKind::Fullscreen ? RecordTargetKind::Fullscreen
              : kind == CaptureKind::Window   ? RecordTargetKind::Window
                                              : RecordTargetKind::Region;
          chosenTarget = makeRecordTarget(data.monitor, targetKind, selection);
          if (targetKind == RecordTargetKind::Window)
            chosenTarget.windowClass =
                dominantAppClass(data.windows, selection);
        });
  }
  editor.setScreen(targetScreen);
  editor.setGeometry(targetScreen->geometry());
  editor.winId();
  QWindow *window = editor.windowHandle();
  LayerShellQt::Window *layerWindow = LayerShellQt::Window::get(window);
  if (!window || !layerWindow) {
    qCritical() << "Could not create capture overlay layer";
    return 1;
  }
  layerWindow->setScope(QStringLiteral("omasnap"));
  layerWindow->setScreen(targetScreen);
  layerWindow->setLayer(LayerShellQt::Window::LayerOverlay);
  LayerShellQt::Window::Anchors anchors;
  anchors.setFlag(LayerShellQt::Window::AnchorTop);
  anchors.setFlag(LayerShellQt::Window::AnchorBottom);
  anchors.setFlag(LayerShellQt::Window::AnchorLeft);
  anchors.setFlag(LayerShellQt::Window::AnchorRight);
  layerWindow->setAnchors(anchors);
  layerWindow->setExclusiveZone(-1);
  layerWindow->setKeyboardInteractivity(
      LayerShellQt::Window::KeyboardInteractivityExclusive);
  layerWindow->setActivateOnShow(true);
  editor.setLayerWindow(layerWindow);
  startupTimingMark("layer surface configured");
  editor.show();
  editor.setFocus(Qt::ActiveWindowFocusReason);
  startupTimingMark("show requested; entering event loop");

  const int exitCode = application.exec();
  if (!recording || exitCode != 0)
    return exitCode;
  // Cancelled without picking anything: the same nothing-happened exit a
  // dismissed overlay gives.
  if (chosenTarget.globalLogical.isEmpty())
    return 0;
  // The overlay is gone and this process is about to release the screenshot
  // lock, so the recorder starts free of it.
  QString handoffError;
  if (!handOffToRecorder(chosenTarget, recordOptions, handoffError)) {
    qCritical().noquote() << handoffError;
    return 1;
  }
  return 0;
}
