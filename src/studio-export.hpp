#pragma once

#include "studio-project.hpp"

#include <QDialog>
#include <QFutureWatcher>
#include <atomic>
#include <memory>

class QLabel;
class QProgressBar;
class QPushButton;

/** A window-modal export job. Workers own all process and filesystem work. */
class StudioExportDialog final : public QDialog {
  Q_OBJECT
public:
  StudioExportDialog(const StudioProject &project, const QString &source,
                     QWidget *parent = nullptr);
  ~StudioExportDialog() override;
  void reject() override;

signals:
  void exportFinished(const QString &message, bool failed);

private:
  struct Result {
    QString destination;
    QString error;
    bool cancelled = false;
  };
  void openDestination(bool folder);
  std::shared_ptr<std::atomic_bool> cancel_ =
      std::make_shared<std::atomic_bool>(false);
  QFutureWatcher<Result> watcher_;
  QFutureWatcher<bool> openWatcher_;
  QLabel *heading_;
  QLabel *detail_;
  QLabel *path_;
  QProgressBar *progress_;
  QPushButton *close_;
  QPushButton *open_;
  QPushButton *folder_;
  QString destination_;
  bool running_ = true;
};
