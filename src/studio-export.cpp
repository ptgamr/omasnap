#include "studio-export.hpp"

#include "overlay-chrome.hpp"
#include "studio-composition.hpp"
#include "studio-project.hpp"
#include "studio-theme.hpp"
#include "studio.hpp"

#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QProcess>
#include <QProgressBar>
#include <QPromise>
#include <QPushButton>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrentRun>
#include <QtGlobal>

StudioExportDialog::StudioExportDialog(const StudioProject &project,
                                       const QString &source, QWidget *parent)
    : QDialog(parent) {
  setObjectName(QStringLiteral("studioExportDialog"));
  setWindowTitle(QStringLiteral("Export MP4"));
  setWindowModality(Qt::WindowModal);
  setAttribute(Qt::WA_DeleteOnClose);
  setFont(chromeMonoFont(12));
  resize(560, 280);
  auto *layout = new QVBoxLayout(this);
  const int margin = StudioChrome::panelPadding;
  layout->setContentsMargins(margin, margin, margin, margin);
  layout->setSpacing(StudioChrome::gap * 2);
  heading_ = new QLabel(QStringLiteral("Exporting video"), this);
  heading_->setFont(chromeMonoFont(16, true));
  heading_->setObjectName(QStringLiteral("exportHeading"));
  layout->addWidget(heading_);
  detail_ = new QLabel(QStringLiteral("Preparing export…"), this);
  detail_->setObjectName(QStringLiteral("exportDetail"));
  detail_->setWordWrap(true);
  detail_->setTextFormat(Qt::PlainText);
  detail_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                   Qt::TextSelectableByKeyboard);
  layout->addWidget(detail_);
  progress_ = new QProgressBar(this);
  progress_->setObjectName(QStringLiteral("exportProgress"));
  progress_->setRange(0, 100);
  progress_->setValue(0);
  progress_->setTextVisible(false);
  layout->addWidget(progress_);
  path_ = new QLabel(this);
  path_->setObjectName(QStringLiteral("exportPath"));
  path_->setTextFormat(Qt::PlainText);
  path_->setWordWrap(true);
  path_->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                 Qt::TextSelectableByKeyboard);
  layout->addWidget(path_);
  auto *buttons = new QHBoxLayout;
  layout->addLayout(buttons);
  open_ = new QPushButton(QStringLiteral("Open video"), this);
  open_->setObjectName(QStringLiteral("exportOpen"));
  folder_ = new QPushButton(QStringLiteral("Open folder"), this);
  folder_->setObjectName(QStringLiteral("exportFolder"));
  close_ = new QPushButton(QStringLiteral("Cancel"), this);
  close_->setObjectName(QStringLiteral("exportClose"));
  open_->hide();
  folder_->hide();
  buttons->addWidget(open_);
  buttons->addWidget(folder_);
  buttons->addStretch();
  buttons->addWidget(close_);
  connect(close_, &QPushButton::clicked, this, &StudioExportDialog::reject);
  connect(open_, &QPushButton::clicked, this,
          [this] { openDestination(false); });
  connect(folder_, &QPushButton::clicked, this,
          [this] { openDestination(true); });
  connect(&openWatcher_, &QFutureWatcher<bool>::finished, this, [this] {
    open_->setEnabled(true);
    folder_->setEnabled(true);
    if (!openWatcher_.result())
      detail_->setText(QStringLiteral(
          "Could not launch the default application. "
          "Open the saved path manually (xdg-open is required)."));
  });
  connect(&watcher_, &QFutureWatcher<Result>::progressTextChanged, path_,
          &QLabel::setText);
  connect(
      &watcher_, &QFutureWatcher<Result>::progressValueChanged, this,
      [this](int value) {
        if (!running_ || cancel_->load())
          return;
        progress_->setValue(value);
        detail_->setText(
            value >= 99
                ? QStringLiteral("99% · Finalizing MP4…")
                : QStringLiteral("%1% · Encoding video and audio…").arg(value));
      });
  connect(&watcher_, &QFutureWatcher<Result>::finished, this, [this] {
    running_ = false;
    const Result result = watcher_.result();
    destination_ = result.destination;
    const bool success = result.error.isEmpty() && !result.cancelled;
    heading_->setText(success            ? QStringLiteral("Export complete")
                      : result.cancelled ? QStringLiteral("Export cancelled")
                                         : QStringLiteral("Export failed"));
    detail_->setText(
        success ? QStringLiteral("Your video is saved and ready to share.")
        : result.cancelled
            ? QStringLiteral("No video was saved. Your project is unchanged.")
            : result.error);
    path_->setText(success ? destination_ : QString());
    progress_->setValue(success ? 100 : progress_->value());
    progress_->setVisible(success);
    open_->setVisible(success);
    folder_->setVisible(success);
    close_->setText(QStringLiteral("Close"));
    close_->setEnabled(true);
    close_->setFocus();
    emit exportFinished(success ? QStringLiteral("Saved %1").arg(destination_)
                                : heading_->text(),
                        !success && !result.cancelled);
  });
  const auto cancel = cancel_;
  watcher_.setFuture(QtConcurrent::run([project, source,
                                        cancel](QPromise<Result> &promise) {
    Result result;
    const auto run = [&] {
      const QString ffmpeg =
          QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
      if (ffmpeg.isEmpty()) {
        result.error = QStringLiteral(
            "ffmpeg is not installed. Install it and try exporting again.");
        return;
      }
      result.destination =
          QFileInfo(studioExportPath(source)).absoluteFilePath();
      promise.setProgressRange(0, 100);
      promise.setProgressValueAndText(0, result.destination);
      // Never expose a partial MP4 or overwrite an existing destination.
      QTemporaryFile temporary(QFileInfo(result.destination).absolutePath() +
                               QStringLiteral("/.omasnap-export-XXXXXX.mp4"));
      if (!temporary.open()) {
        result.error = QStringLiteral("Cannot write to the export folder: %1")
                           .arg(temporary.errorString());
        return;
      }
      const QString temporaryPath = temporary.fileName();
      temporary.close();
      QStringList arguments =
          studioCompositionArguments(project, temporaryPath, result.error);
      if (arguments.isEmpty())
        return;
      if (cancel->load()) {
        result.cancelled = true;
        return;
      }
      arguments.prepend(QStringLiteral("pipe:1"));
      arguments.prepend(QStringLiteral("-progress"));
      arguments.prepend(QStringLiteral("-nostats"));
      QProcess process;
      process.start(ffmpeg, arguments);
      if (!process.waitForStarted(5000)) {
        result.error = QStringLiteral("Could not start ffmpeg: %1")
                           .arg(process.errorString());
        return;
      }
      QElapsedTimer elapsed;
      elapsed.start();
      QByteArray pending;
      QByteArray diagnostics;
      const qint64 duration = (project.trimOutMs < 0 ? studioDuration(project)
                                                     : project.trimOutMs) -
                              project.trimInMs;
      while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(100);
        diagnostics =
            (diagnostics + process.readAllStandardError()).right(1000);
        pending += process.readAllStandardOutput();
        qsizetype newline;
        while ((newline = pending.indexOf('\n')) >= 0) {
          const QByteArray line = pending.left(newline).trimmed();
          pending.remove(0, newline + 1);
          if (line.startsWith("out_time_us=")) {
            bool ok = false;
            const qint64 us = line.mid(12).toLongLong(&ok);
            if (ok && duration > 0)
              promise.setProgressValueAndText(
                  qBound(0,
                         int(100.0 * double(us) / (double(duration) * 1000.0)),
                         99),
                  result.destination);
          }
        }
        if (cancel->load() || elapsed.elapsed() >= 3600000) {
          result.cancelled = cancel->load();
          if (!result.cancelled)
            result.error = QStringLiteral(
                "Export timed out after one hour. Try a shorter export range.");
          process.kill();
          process.waitForFinished(5000);
          return;
        }
      }
      if (process.exitStatus() != QProcess::NormalExit ||
          process.exitCode() != 0) {
        result.error = QStringLiteral("FFmpeg could not export this video:\n%1")
                           .arg(QString::fromUtf8(diagnostics));
        return;
      }
      if (cancel->load()) {
        result.cancelled = true;
        return;
      }
      if (!QFile::rename(temporaryPath, result.destination))
        result.error =
            QStringLiteral(
                "Could not save the finished video to %1. Check folder "
                "permissions and available space, then retry.")
                .arg(result.destination);
    };
    run();
    promise.addResult(result);
  }));
}

StudioExportDialog::~StudioExportDialog() { cancel_->store(true); }

void StudioExportDialog::reject() {
  if (!running_) {
    QDialog::reject();
    return;
  }
  cancel_->store(true);
  detail_->setText(QStringLiteral("Cancelling export…"));
  close_->setEnabled(false);
}

void StudioExportDialog::openDestination(bool folder) {
  open_->setEnabled(false);
  folder_->setEnabled(false);
  const QString target =
      folder ? QFileInfo(destination_).absolutePath() : destination_;
  openWatcher_.setFuture(QtConcurrent::run([target] {
    return QProcess::startDetached(QStringLiteral("xdg-open"), {target});
  }));
}
