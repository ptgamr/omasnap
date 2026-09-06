/** @fileoverview Entry point for omasnap-studio. Deliberately its own
 *  executable: `omasnap` sets layer-shell process-wide and links no media
 *  libraries, and neither should change because video exists. */
#include "overlay-chrome.hpp"
#include "studio.hpp"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QUrl>

int main(int argc, char **argv) {
  QCoreApplication::setApplicationName(QStringLiteral("omasnap-studio"));
  QCoreApplication::setApplicationVersion(QString::fromLatin1(OMASNAP_VERSION));
  QCoreApplication::setOrganizationName(QStringLiteral("Omarchy"));
  // Same reasoning as omasnap: the window paints its own explicit colours and
  // opens no native dialogs, so loading a desktop theme plugin buys nothing.
  qputenv("QT_QPA_PLATFORMTHEME", "generic");
  QGuiApplication::setDesktopFileName(QStringLiteral("omasnap-studio"));
  const QApplication application(argc, argv);
  QApplication::setFont(chromeMonoFont(12));

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Review a non-destructive Studio project or screen recording. A video "
      "does "
      "not\nhave to have been recorded by omasnap.\n"
      "\n"
      "Click the picture to aim a zoom: inside an existing cue that re-aims "
      "it,\nand anywhere else it starts a new one at the playhead. Cues are "
      "the blocks\non the lane under the trim bar; drag their bodies to move "
      "them and their\nedges to change how long they run. The slider sets how "
      "far in the selected\ncue goes.\n"
      "\n"
      "Space plays and pauses from any control. Left/Right step one frame; "
      "Shift+Left/Right seek five seconds. I/O set trim points, Z adds a zoom, "
      "Ctrl+Z undoes, and Ctrl+E exports. Press ? for all shortcuts.\n"
      "\n"
      "Source assets, clip ranges, zoom cues, and canvas styling are saved as "
      "`<recording>.omasnap.json`,\nso the original file is never "
      "touched and reopening brings them back.\n"
      "\n"
      "Exit codes: 0 success; 1 the recording could not be opened; 2 usage "
      "error."));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(
      QStringLiteral("recording"),
      QStringLiteral("Video or .omasnap.json project to open."),
      QStringLiteral("<recording>"));
  parser.process(application);

  const QStringList positional = parser.positionalArguments();
  if (positional.size() != 1) {
    qCritical() << "omasnap-studio takes exactly one recording to open";
    return 2;
  }
  QString path = QUrl(positional.first()).toLocalFile();
  if (path.isEmpty())
    path = positional.first();
  if (!QFileInfo::exists(path)) {
    qCritical().noquote() << QStringLiteral("No such recording: %1").arg(path);
    return 1;
  }

  StudioWindow window(path);
  window.show();
  return QApplication::exec();
}
