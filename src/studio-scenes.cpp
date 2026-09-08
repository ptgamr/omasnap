/** @fileoverview Asynchronous scene import and non-destructive arrangement. */
#include "studio-playback.hpp"
#include "studio.hpp"
#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>

namespace {
QStringList localVideos(const QMimeData *mime) {
  QStringList paths;
  if (!mime->hasUrls())
    return paths;
  for (const auto &url : mime->urls()) {
    if (!url.isLocalFile())
      return {};
    paths.push_back(url.toLocalFile());
  }
  return paths;
}
} // namespace

bool StudioWindow::scenesEditable() const {
  return loaded_ && !export_ && !relinking_ && !importing_ && !closing_;
}

void StudioWindow::setupScenes(QVBoxLayout *controls) {
  setAcceptDrops(true);
  importButton_ = new QPushButton(QStringLiteral("Add scenes"), this);
  importButton_->setObjectName(QStringLiteral("addScenes"));
  importButton_->setToolTip(
      QStringLiteral("Import video files · Ctrl+O · or drop files here"));
  controls->addWidget(importButton_);
  connect(importButton_, &QPushButton::clicked, this,
          &StudioWindow::chooseScenes);
  sceneLabel_ =
      new QLabel(QStringLiteral("Select a scene on the video lane"), this);
  sceneLabel_->setWordWrap(true);
  controls->addWidget(sceneLabel_);
  sceneIn_ = new QSpinBox(this);
  sceneOut_ = new QSpinBox(this);
  sceneIn_->setObjectName(QStringLiteral("sceneIn"));
  sceneOut_->setObjectName(QStringLiteral("sceneOut"));
  for (auto *spin : {sceneIn_, sceneOut_}) {
    spin->setKeyboardTracking(false);
    spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
    spin->setSuffix(QStringLiteral(" ms"));
  }
  for (const auto &entry :
       {std::pair{QStringLiteral("Source in"), sceneIn_},
        std::pair{QStringLiteral("Source out"), sceneOut_}}) {
    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(entry.first, this));
    row->addWidget(entry.second);
    controls->addLayout(row);
  }
  const auto trim = [this] {
    trimScene(timeline_->selectedClip(), sceneIn_->value(), sceneOut_->value());
  };
  connect(sceneIn_, &QSpinBox::valueChanged, this, trim);
  connect(sceneOut_, &QSpinBox::valueChanged, this, trim);
  duplicateButton_ =
      new QPushButton(QStringLiteral("Duplicate · Ctrl+D"), this);
  duplicateButton_->setObjectName(QStringLiteral("duplicateScene"));
  controls->addWidget(duplicateButton_);
  connect(duplicateButton_, &QPushButton::clicked, this,
          &StudioWindow::duplicateScene);
  auto *order = new QHBoxLayout;
  earlierButton_ = new QPushButton(QStringLiteral("Earlier"), this);
  laterButton_ = new QPushButton(QStringLiteral("Later"), this);
  order->addWidget(earlierButton_);
  order->addWidget(laterButton_);
  controls->addLayout(order);
  connect(earlierButton_, &QPushButton::clicked, this, [this] {
    for (qsizetype i = 1; i < project_.clips.size(); ++i)
      if (project_.clips[i].id == timeline_->selectedClip()) {
        moveScene(project_.clips[i].id, project_.clips[i - 1].id);
        return;
      }
  });
  connect(laterButton_, &QPushButton::clicked, this, [this] {
    for (qsizetype i = 0; i + 1 < project_.clips.size(); ++i)
      if (project_.clips[i].id == timeline_->selectedClip()) {
        moveScene(project_.clips[i].id,
                  i + 2 < project_.clips.size() ? project_.clips[i + 2].id : 0);
        return;
      }
  });
  setupTransitions(controls);
  controls->addWidget(new QLabel(QStringLiteral("Project export range"), this));
  connect(&importWatcher_, &QFutureWatcher<StudioProjectLoad>::finished, this,
          [this] {
            importing_ = false;
            const auto result = importWatcher_.result();
            if (!result.error.isEmpty()) {
              setStatus(result.error, true);
              refreshControls();
              return;
            }
            const auto before = project_.transitions;
            project_ = result.project;
            finishCompositionEdit(player_->position());
            setStatus(QStringLiteral(
                          "Scenes added — drag to arrange; Ctrl+Z to undo") +
                      transitionAdjustment(before));
          });
}

void StudioWindow::refreshSceneControls(bool force) {
  if (!importButton_)
    return;
  importButton_->setEnabled(scenesEditable() && !editGesture_);
  refreshTransitionControls(force);
  const StudioClip *selected = nullptr;
  qsizetype index = 0;
  for (; index < project_.clips.size(); ++index)
    if (project_.clips[index].id == timeline_->selectedClip()) {
      selected = &project_.clips[index];
      break;
    }
  const auto *asset =
      selected ? studioAsset(project_, selected->assetId) : nullptr;
  const bool editable = selected && asset && scenesEditable();
  for (auto *widget : QVector<QWidget *>{sceneIn_, sceneOut_, duplicateButton_,
                                         earlierButton_, laterButton_})
    widget->setEnabled(editable);
  earlierButton_->setEnabled(editable && index > 0);
  laterButton_->setEnabled(editable && index + 1 < project_.clips.size());
  if (!selected || !asset) {
    sceneLabel_->setText(QStringLiteral("Ctrl+click a clip to see its filename and details"));
    return;
  }
   sceneLabel_->setText(
       QStringLiteral("%1\n%2 · source %3 × %4")
           .arg(QFileInfo(asset->path).fileName(),
                studioTimecode(qRound64((selected->outMs - selected->inMs) /
                                       selected->speed))
                    .mid(3))
               .arg(asset->source.size.width())
               .arg(asset->source.size.height()));
  // Never wipe values being typed: trims commit on Enter, and a refresh in
  // between must not replace the fields from behind.
  const auto *inEdit = sceneIn_->findChild<QLineEdit *>();
  const auto *outEdit = sceneOut_->findChild<QLineEdit *>();
  const bool typingTrim =
      sceneIn_->hasFocus() || sceneOut_->hasFocus() ||
      (inEdit && inEdit->hasFocus()) || (outEdit && outEdit->hasFocus());
  if (!typingTrim || force) {
    const QSignalBlocker quietIn(sceneIn_), quietOut(sceneOut_);
    sceneIn_->setRange(0, static_cast<int>(selected->outMs - 1));
    sceneOut_->setRange(static_cast<int>(selected->inMs + 1),
                        static_cast<int>(asset->source.durationMs));
    sceneIn_->setValue(static_cast<int>(selected->inMs));
    sceneOut_->setValue(static_cast<int>(selected->outMs));
  }
}

void StudioWindow::chooseScenes() {
  if (!scenesEditable() || editGesture_)
    return;
  auto *dialog = new QFileDialog(this, QStringLiteral("Add scenes"));
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setOption(QFileDialog::DontUseNativeDialog);
  dialog->setFileMode(QFileDialog::ExistingFiles);
  dialog->setNameFilters(
      {QStringLiteral("Video (*.mp4 *.mkv *.mov *.webm *.m4v *.avi)"),
       QStringLiteral("All files (*)")});
  connect(dialog, &QFileDialog::filesSelected, this,
          [this](const QStringList &paths) { importScenes(paths); });
  dialog->open();
}

void StudioWindow::importScenes(const QStringList &paths, quint64 before) {
  if (!scenesEditable() || editGesture_ || paths.isEmpty())
    return;
  if (paths.size() + project_.clips.size() > 1000) {
    setStatus(QStringLiteral("A project supports at most 1000 scenes"), true);
    return;
  }
  captureCursor();
  player_->pause();
  importing_ = true;
  refreshControls();
  setStatus(QStringLiteral("Reading %1 scene(s)…").arg(paths.size()));
  const auto snapshot = project_;
  const quint64 assetBase = nextAssetId_, clipBase = nextClipId_;
  nextAssetId_ += paths.size();
  nextClipId_ += paths.size();
  importWatcher_.setFuture(QtConcurrent::run([snapshot, paths, before,
                                              assetBase, clipBase] {
    StudioProjectLoad result;
    result.project = snapshot;
    QVector<StudioAsset> assets;
    QVector<StudioClip> clips;
    for (qsizetype i = 0; i < paths.size(); ++i) {
      if (!QFileInfo(paths[i]).isFile()) {
        result.error =
            QStringLiteral(
                "Cannot import %1: not a regular file. Project unchanged.")
                .arg(QFileInfo(paths[i]).fileName());
        return result;
      }
      const auto source = probeStudioSource(paths[i]);
      if (!source.usable() || source.durationMs <= 0) {
        result.error =
            QStringLiteral(
                "Cannot import %1: not a supported video. Project unchanged.")
                .arg(QFileInfo(paths[i]).fileName());
        return result;
      }
      assets.push_back({assetBase + static_cast<quint64>(i),
                        QFileInfo(paths[i]).absoluteFilePath(), source});
      clips.push_back({clipBase + static_cast<quint64>(i), assets.last().id, 0,
                       source.durationMs, 1.0});
    }
    qsizetype index = snapshot.clips.size();
    for (qsizetype i = 0; i < snapshot.clips.size(); ++i)
      if (snapshot.clips[i].id == before) {
        index = i;
        break;
      }
    if (!studioInsertScenes(result.project, assets, clips, index,
                            result.error) &&
        result.error.isEmpty())
      result.error = QStringLiteral("No scenes were added");
    return result;
  }));
}

void StudioWindow::moveScene(quint64 id, quint64 before) {
  if (!scenesEditable())
    return;
  captureCursor();
  const auto transitions = project_.transitions;
  const auto frame = studioFrameAt(project_, player_->position());
  QString error;
  if (!studioMoveClip(project_, id, before, error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    return;
  }
  const auto mapped =
      frame ? studioTimelineTime(project_, frame->span.clipId, frame->sourceMs)
            : std::nullopt;
  finishCompositionEdit(mapped.value_or(player_->position()));
  timeline_->setSelectedClip(id);
  rememberEdit();
  setStatus(QStringLiteral("Scene moved — Ctrl+Z to undo") +
            transitionAdjustment(transitions));
}

void StudioWindow::duplicateScene() {
  if (!scenesEditable() || editGesture_ || !timeline_->selectedClip())
    return;
  captureCursor();
  const auto transitions = project_.transitions;
  QString error;
  const quint64 id = nextClipId_++;
  if (!studioDuplicateClip(project_, timeline_->selectedClip(), id, error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    return;
  }
  finishCompositionEdit(player_->position());
  timeline_->setSelectedClip(id);
  rememberEdit();
  setStatus(QStringLiteral("Scene duplicated — Ctrl+Z to undo") +
            transitionAdjustment(transitions));
}

void StudioWindow::trimScene(quint64 id, qint64 in, qint64 out) {
  if (!scenesEditable() || restoring_)
    return;
  captureCursor();
  auto candidate = editGesture_ ? gestureProject_ : project_;
  const auto transitions = candidate.transitions;
  QString error;
  const bool changed = studioTrimClip(candidate, id, in, out, error);
  if (!error.isEmpty()) {
    setStatus(error, true);
    refreshSceneControls(true);
    return;
  }
  if ((!changed && !editGesture_) || candidate == project_)
    return;
  project_ = candidate;
  player_->pause();
  applyProject(false, player_->position());
  timeline_->setSelectedClip(id);
  rememberEdit();
  setStatus(QStringLiteral("Scene trimmed — Ctrl+Z to undo") +
            transitionAdjustment(transitions));
}

void StudioWindow::dragEnterEvent(QDragEnterEvent *event) {
  if (scenesEditable() && !editGesture_ &&
      !localVideos(event->mimeData()).isEmpty())
    event->acceptProposedAction();
}
void StudioWindow::dragMoveEvent(QDragMoveEvent *event) {
  if (!scenesEditable() || localVideos(event->mimeData()).isEmpty())
    return;
  const auto point = timeline_->mapFrom(this, event->position().toPoint());
  timeline_->showInsertion(timeline_->insertionBefore(point.x()),
                           timeline_->rect().contains(point));
  event->acceptProposedAction();
}
void StudioWindow::dragLeaveEvent(QDragLeaveEvent *event) {
  timeline_->showInsertion(0, false);
  event->accept();
}
void StudioWindow::dropEvent(QDropEvent *event) {
  timeline_->showInsertion(0, false);
  const auto paths = localVideos(event->mimeData());
  if (!scenesEditable() || editGesture_ || paths.isEmpty())
    return;
  const auto point = timeline_->mapFrom(this, event->position().toPoint());
  importScenes(paths, timeline_->rect().contains(point)
                          ? timeline_->insertionBefore(point.x())
                          : 0);
  event->acceptProposedAction();
}
