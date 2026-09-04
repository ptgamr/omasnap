/** @fileoverview Tests the selector in recording-target mode: dragging an
 *  area answers with a rectangle and closes, a window pick answers with the
 *  window's rectangle, cancelling answers with nothing, and none of it
 *  writes an image, a shelved document, or an operation log, and none of the
 *  screenshot-only ways out of the selector are reachable. */
#include "record-select-smoke.hpp"

#include "capture.hpp"
#include "editor.hpp"
#include "record-target.hpp"
#include "recent-snaps.hpp"

#include <QApplication>
#include <QDir>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

CaptureData sampleCapture() {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("HDMI-A-1");
  // Offset and scaled, so a target that forgot either would be visible.
  capture.monitor.geometry = {2560, 0, 800, 600};
  capture.monitor.pixelSize = {1200, 900};
  capture.monitor.scale = 1.5;
  capture.monitor.workspaceId = 4;
  capture.source = QImage(1200, 900, QImage::Format_ARGB32_Premultiplied);
  capture.source.fill(QColor(QStringLiteral("#182030")));
  capture.previewSize = QSize(800, 600);
  capture.windows = {WindowTarget{QRect(100, 80, 400, 300),
                                  QStringLiteral("0x1"),
                                  QStringLiteral("Editor"),
                                  QStringLiteral("org.mozilla.firefox")}};
  return capture;
}

bool check(bool condition, QString &error, const QString &message) {
  if (condition)
    return true;
  error = message;
  return false;
}

} // namespace

bool runRecordSelectSmoke(QApplication &application, QString &error) {
  // Point the recents shelf at a directory of our own so "nothing was
  // written" is a claim about this test and not about the developer's shelf.
  QTemporaryDir shelf;
  if (!shelf.isValid()) {
    error = QStringLiteral("could not create a temporary shelf");
    return false;
  }
  const QByteArray previousShelf = qgetenv("OMASNAP_RECENT_DIR");
  qputenv("OMASNAP_RECENT_DIR", shelf.path().toUtf8());
  const auto restoreShelf = qScopeGuard([&previousShelf] {
    if (previousShelf.isEmpty())
      qunsetenv("OMASNAP_RECENT_DIR");
    else
      qputenv("OMASNAP_RECENT_DIR", previousShelf);
  });

  // A shelf with something on it, so "the recents are not offered" is a
  // claim about the selector and not about an empty directory.
  {
    QTemporaryDir working;
    if (!working.isValid()) {
      error = QStringLiteral("could not create a temporary working directory");
      return false;
    }
    const QString source =
        QDir(working.path()).filePath(QStringLiteral("snap.png"));
    QImage image(200, 150, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(QStringLiteral("#804020")));
    QString recordError;
    if (!image.save(source, "PNG") ||
        !recordRecentSnap(source, {}, image, recordError)) {
      error = QStringLiteral("could not shelve a recent capture: %1")
                  .arg(recordError);
      return false;
    }
    if (listRecentSnaps(false).isEmpty()) {
      error = QStringLiteral("the shelved capture did not appear on the "
                             "shelf");
      return false;
    }
  }

  // Dragging an area answers with that rectangle, in preview coordinates,
  // and closes without ever entering the annotation editor.
  {
    CaptureEditor editor(sampleCapture());
    editor.setRecordTargetMode(true);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QSignalSpy chosen(&editor, &CaptureEditor::recordTargetSelected);

    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(120, 140));
    QTest::mouseMove(&editor, QPoint(520, 380), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(520, 380));
    application.processEvents();

    if (!check(chosen.count() == 1, error,
               QStringLiteral("dragging an area did not answer with a "
                              "target")))
      return false;
    const QRectF selection = chosen.at(0).at(0).toRectF();
    if (!check(qAbs(selection.width() - 400.0) < 2.0 &&
                   qAbs(selection.height() - 240.0) < 2.0,
               error, QStringLiteral("the answered rectangle is the wrong "
                                     "size")))
      return false;
    if (!check(!editor.editingForTest() && !editor.exportingForTest(), error,
               QStringLiteral("picking a target entered the editor")))
      return false;

    // The same conversion the recorder does, on the same numbers: the
    // rectangle has to survive into global logical coordinates and native
    // pixels, not just look right on screen.
    const RecordTarget target = makeRecordTarget(
        editor.captureData().monitor, RecordTargetKind::Region, selection);
    if (!check(target.globalLogical.x() >= 2560 &&
                   target.globalLogical.x() < 2560 + 800,
               error, QStringLiteral("the target did not land on the "
                                     "recorded output")))
      return false;
    if (!check(target.sourcePixels.width() ==
                   qRound(target.logical.width() * 1.5),
               error, QStringLiteral("the target lost the monitor scale")))
      return false;

    editor.close();
    application.processEvents();
    if (!check(editor.operationLog().isEmpty(), error,
               QStringLiteral("picking a target wrote an operation log")))
      return false;
    if (!check(editor.workingSourcePath().isEmpty() ||
                   !QFile::exists(editor.workingSourcePath()),
               error, QStringLiteral("picking a target wrote an image")))
      return false;
  }

  // A window pick answers with that window's rectangle, tagged as a window
  // so the recorder can name the file after the app.
  {
    CaptureEditor editor(sampleCapture(), CaptureEditor::CaptureMode::Window);
    editor.setRecordTargetMode(true);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QSignalSpy chosen(&editor, &CaptureEditor::recordTargetSelected);

    QTest::mouseMove(&editor, QPoint(300, 200), 20);
    application.processEvents();
    QTest::mouseClick(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(300, 200));
    application.processEvents();

    if (!check(chosen.count() == 1, error,
               QStringLiteral("clicking a window did not answer with a "
                              "target")))
      return false;
    if (!check(chosen.at(0).at(0).toRectF() == QRectF(100, 80, 400, 300),
               error,
               QStringLiteral("the window target is not the window's "
                              "rectangle")))
      return false;
    if (!check(chosen.at(0).at(1).value<CaptureKind>() == CaptureKind::Window,
               error, QStringLiteral("the window target was not tagged as "
                                     "one")))
      return false;
    if (!check(dominantAppClass(editor.captureData().windows,
                                chosen.at(0).at(0).toRectF()) ==
                   QStringLiteral("org.mozilla.firefox"),
               error, QStringLiteral("the window target does not name its "
                                     "app")))
      return false;
    editor.close();
    application.processEvents();
  }

  // The screenshot-only routes out of the selector are closed. The recents
  // shelf reopens an image, and S starts a scrolling stitch; either would
  // leave the overlay unable to answer with a target at all.
  {
    CaptureEditor editor(sampleCapture());
    editor.setRecordTargetMode(true);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    static_cast<void>(editor.waitForRecents());
    application.processEvents();
    if (!check(editor.recentCardRectForTest(0).isNull(), error,
               QStringLiteral("the recents shelf is offered while picking a "
                              "recording target")))
      return false;

    QSignalSpy chosen(&editor, &CaptureEditor::recordTargetSelected);
    QTest::keyClick(&editor, Qt::Key_S);
    application.processEvents();
    QTest::mousePress(&editor, Qt::LeftButton, Qt::NoModifier,
                      QPoint(200, 200));
    QTest::mouseMove(&editor, QPoint(400, 350), 20);
    QTest::mouseRelease(&editor, Qt::LeftButton, Qt::NoModifier,
                        QPoint(400, 350));
    application.processEvents();
    if (!check(chosen.count() == 1, error,
               QStringLiteral("S turned the selector into a scrolling "
                              "capture")))
      return false;
    editor.close();
    application.processEvents();
  }

  // Escape answers with nothing, which is how the caller knows to record
  // nothing rather than to record the whole screen.
  {
    CaptureEditor editor(sampleCapture());
    editor.setRecordTargetMode(true);
    editor.resize(800, 600);
    editor.show();
    application.processEvents();
    QSignalSpy chosen(&editor, &CaptureEditor::recordTargetSelected);
    QTest::keyClick(&editor, Qt::Key_Escape);
    application.processEvents();
    if (!check(chosen.isEmpty(), error,
               QStringLiteral("cancelling still answered with a target")))
      return false;
    editor.close();
    application.processEvents();
  }

  // One entry on the shelf, the one this test put there: the selector added
  // nothing of its own.
  if (!check(listRecentSnaps(false).size() == 1, error,
             QStringLiteral("the recording selector shelved a document")))
    return false;
  return true;
}
