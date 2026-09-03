/** @fileoverview Entry point for omasnap-studio. Deliberately its own
 *  executable: `omasnap` sets layer-shell process-wide and links no media
 *  libraries, and neither should change because video exists. */
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
  QApplication application(argc, argv);

  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral(
      "Review and trim an OmaSnap screen recording.\n"
      "\n"
      "Space plays and pauses, Left/Right seek five seconds, I and O set the "
      "in\nand out points at the playhead, R clears the trim, and Ctrl+E "
      "exports the\nkept range beside the original.\n"
      "\n"
      "Exit codes: 0 success; 1 the recording could not be opened; 2 usage "
      "error."));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(QStringLiteral("recording"),
                               QStringLiteral("Video file to open."),
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
    qCritical().noquote()
        << QStringLiteral("No such recording: %1").arg(path);
    return 1;
  }

  StudioWindow window(path);
  window.show();
  return QApplication::exec();
}
