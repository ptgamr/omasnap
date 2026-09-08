/** @fileoverview Asynchronous scene import and non-destructive arrangement. */
#include "studio-playback.hpp"
#include "studio.hpp"
#include "overlay-chrome.hpp"
#include "studio-theme.hpp"
#include <QApplication>
#include <QComboBox>
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

#include <limits>

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
  sceneSection_ = new QWidget(this);
  auto *sceneLayout = new QVBoxLayout(sceneSection_);
  sceneLayout->setContentsMargins(0, 0, 0, 0);
  sceneLabel_ =
      new QLabel(QStringLiteral("Select a scene on the video lane"), this);
  sceneLabel_->setWordWrap(true);
  sceneLayout->addWidget(sceneLabel_);
  auto *selectedLabel = new QLabel(QStringLiteral("Selected Clip"), this);
  selectedLabel->setFont(chromeMonoFont(13));
  sceneLayout->addWidget(selectedLabel);
  auto *speedRow = new QHBoxLayout;
  speedRow->addWidget(new QLabel(QStringLiteral("Speed"), this));
  speedRow->addStretch();
  clipSpeed_ = new StudioComboBox(this);
  clipSpeed_->setObjectName(QStringLiteral("clipSpeed"));
  clipSpeed_->setToolTip(
      QStringLiteral("Playback speed of the selected scene"));
  for (const double speed :
       {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0})
    clipSpeed_->addItem(QStringLiteral("%1×").arg(speed), speed);
  speedRow->addWidget(clipSpeed_);
  sceneLayout->addLayout(speedRow);
  controls->addWidget(sceneSection_);
  connect(clipSpeed_, &QComboBox::activated, this,
          &StudioWindow::changeClipSpeed);
  audioSection_ = new QWidget(this);
  auto *audioLayout = new QVBoxLayout(audioSection_);
  audioLayout->setContentsMargins(0, 0, 0, 0);
  audioLabel_ =
      new QLabel(QStringLiteral("Select a sound on the audio lane"), this);
  audioLabel_->setObjectName(QStringLiteral("audioDetails"));
  audioLabel_->setWordWrap(true);
  audioLayout->addWidget(audioLabel_);
  auto *audioSelectedLabel =
      new QLabel(QStringLiteral("Selected Audio"), this);
  audioSelectedLabel->setFont(chromeMonoFont(13));
  audioLayout->addWidget(audioSelectedLabel);
  auto *gainRow = new QHBoxLayout;
  gainRow->addWidget(new QLabel(QStringLiteral("Gain"), this));
  gainRow->addStretch();
  auto *gainValue = new QLabel(QStringLiteral("100%"), this);
  gainValue->setObjectName(QStringLiteral("audioGainValue"));
  gainValue->setFont(chromeMonoFont(12));
  audioGainValue_ = gainValue;
  gainRow->addWidget(gainValue);
  audioLayout->addLayout(gainRow);
  audioGain_ = new StudioSlider(Qt::Horizontal, this);
  audioGain_->setObjectName(QStringLiteral("audioGain"));
  audioGain_->setRange(0, 100);
  audioGain_->setToolTip(QStringLiteral("Level of the selected sound"));
  audioLayout->addWidget(audioGain_);
  QObject::connect(audioGain_, &QSlider::valueChanged, gainValue,
                   [gainValue](int n) {
                     gainValue->setText(QString::number(n) +
                                        QStringLiteral("%"));
                   });
  auto *audioSpeedRow = new QHBoxLayout;
  audioSpeedRow->addWidget(new QLabel(QStringLiteral("Speed"), this));
  audioSpeedRow->addStretch();
  audioSpeed_ = new StudioComboBox(this);
  audioSpeed_->setObjectName(QStringLiteral("audioSpeed"));
  audioSpeed_->setToolTip(
      QStringLiteral("Playback speed of the selected sound"));
  for (const double speed :
       {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0})
    audioSpeed_->addItem(QStringLiteral("%1×").arg(speed), speed);
  audioSpeedRow->addWidget(audioSpeed_);
  audioLayout->addLayout(audioSpeedRow);
  controls->addWidget(audioSection_);
  connect(audioGain_, &QSlider::valueChanged, this,
          &StudioWindow::changeAudioGain);
  connect(audioGain_, &QSlider::sliderPressed, this, &StudioWindow::beginEdit);
  connect(audioGain_, &QSlider::sliderReleased, this, &StudioWindow::endEdit);
  connect(audioSpeed_, &QComboBox::activated, this,
          &StudioWindow::changeAudioSpeed);
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

void StudioWindow::refreshSceneControls() {
  if (!sceneLabel_)
    return;
  importButton_->setEnabled(scenesEditable() && !editGesture_);
  refreshTransitionControls();
  const StudioClip *selected = nullptr;
  qsizetype index = 0;
  for (; index < project_.clips.size(); ++index)
    if (project_.clips[index].id == timeline_->selectedClip()) {
      selected = &project_.clips[index];
      break;
    }
  const auto *asset =
      selected ? studioAsset(project_, selected->assetId) : nullptr;
  const StudioAudioClip *selectedAudio = nullptr;
  for (const auto &clip : project_.audioClips)
    if (clip.id == timeline_->selectedAudioClip()) {
      selectedAudio = &clip;
      break;
    }
  const auto *audioAsset =
      selectedAudio ? studioAsset(project_, selectedAudio->assetId) : nullptr;
  sceneSection_->setVisible(selectedAudio == nullptr);
  audioSection_->setVisible(selectedAudio != nullptr);
  const auto nearestSpeed = [](StudioComboBox *combo, double speed) {
    // Display-only mapping: the model admits any 0.125..8 speed, the combo
    // offers presets. Returns the nearest preset without touching the model.
    int nearest = 0;
    double best = std::numeric_limits<double>::max();
    for (int i = 0; i < combo->count(); ++i) {
      const double gap = qAbs(combo->itemData(i).toDouble() - speed);
      if (gap < best) {
        best = gap;
        nearest = i;
      }
    }
    return nearest;
  };
  const bool speedEditable = scenesEditable() && !editGesture_ && selected;
  clipSpeed_->setEnabled(speedEditable);
  if (selected) {
    const QSignalBlocker quiet(clipSpeed_);
    clipSpeed_->setCurrentIndex(nearestSpeed(clipSpeed_, selected->speed));
  }
  const bool audioEditable = scenesEditable() && selectedAudio;
  audioGain_->setEnabled(audioEditable);
  audioSpeed_->setEnabled(scenesEditable() && !editGesture_ && selectedAudio);
  if (selectedAudio && audioAsset) {
    audioLabel_->setText(
        QStringLiteral("%1\n%2")
            .arg(QFileInfo(audioAsset->path).fileName(),
                 studioTimecode(qRound64((selectedAudio->outMs -
                                         selectedAudio->inMs) /
                                        selectedAudio->speed))
                     .mid(3)));
    // No blocker: the value signal repaints the percentage label, and the
    // commit is a no-op when the model already matches. The label still
    // follows the model explicitly: restoring an unchanged value emits no
    // signal, which once left a zero-gain sound showing 100%.
    audioGain_->setValue(selectedAudio->gain);
    if (audioGainValue_) {
      audioGainValue_->setText(QString::number(selectedAudio->gain) +
                               QStringLiteral("%"));
    }
    const QSignalBlocker quietSpeed(audioSpeed_);
    audioSpeed_->setCurrentIndex(
        nearestSpeed(audioSpeed_, selectedAudio->speed));
  } else {
    audioLabel_->setText(
        QStringLiteral("Click a sound on the audio lane to tune it"));
  }
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
  if (!scenesEditable() || editGesture_)
    return;
  if (timeline_->selectedAudioClip() != 0) {
    duplicateAudioClip();
    return;
  }
  if (!timeline_->selectedClip())
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
    refreshSceneControls();
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

void StudioWindow::moveAudioClip(quint64 id, quint64 before) {
  // No edit-gesture guard: like scene trims, moves arrive live per drag
  // tick and commit once on release.
  if (!scenesEditable())
    return;
  captureCursor();
  QString error;
  if (!studioMoveAudioClip(project_, id, before, error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    return;
  }
  timeline_->update();
  player_->setAudioClips(project_.audioClips);
  rememberEdit();
}

void StudioWindow::duplicateAudioClip() {
  if (!scenesEditable() || editGesture_ || !timeline_->selectedAudioClip())
    return;
  captureCursor();
  QString error;
  const quint64 id = nextAudioClipId_++;
  if (!studioDuplicateAudioClip(project_, timeline_->selectedAudioClip(), id,
                                error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    return;
  }
  timeline_->setSelectedAudioClip(id);
  timeline_->update();
  player_->setAudioClips(project_.audioClips);
  rememberEdit();
  setStatus(QStringLiteral("Sound duplicated — Ctrl+Z to undo"));
}

void StudioWindow::trimAudioClip(quint64 id, qint64 inMs, qint64 outMs) {
  if (!scenesEditable() || restoring_)
    return;
  captureCursor();
  QString error;
  if (!studioTrimAudioClip(project_, id, inMs, outMs, error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    refreshSceneControls();
    return;
  }
  timeline_->setSelectedAudioClip(id);
  timeline_->update();
  player_->setAudioClips(project_.audioClips);
  rememberEdit();
  refreshControls();
}

void StudioWindow::deleteAudioClip() {
  if (!scenesEditable() || editGesture_ || !timeline_->selectedAudioClip())
    return;
  captureCursor();
  QString error;
  if (!studioDeleteAudioClip(project_, timeline_->selectedAudioClip(),
                             error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    return;
  }
  timeline_->setSelectedAudioClip(0);
  timeline_->update();
  player_->setAudioClips(project_.audioClips);
  rememberEdit();
  refreshControls();
  setStatus(QStringLiteral("Sound deleted — Ctrl+Z to undo"));
}

void StudioWindow::changeAudioGain() {
  if (restoring_ || !audioGain_ || !audioGain_->isEnabled())
    return;
  const quint64 id = timeline_->selectedAudioClip();
  if (!id)
    return;
  captureCursor();
  QString error;
  if (!studioSetAudioClipGain(project_, id, audioGain_->value(), error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    return;
  }
  player_->setAudioClips(project_.audioClips);
  rememberEdit();
  refreshControls();
}

void StudioWindow::changeAudioSpeed() {
  if (restoring_ || !audioSpeed_ || !audioSpeed_->isEnabled())
    return;
  const quint64 id = timeline_->selectedAudioClip();
  if (!id)
    return;
  captureCursor();
  QString error;
  if (!studioSetAudioClipSpeed(project_, id,
                               audioSpeed_->currentData().toDouble(), error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    refreshSceneControls();
    return;
  }
  timeline_->update();
  player_->setAudioClips(project_.audioClips);
  rememberEdit();
  refreshControls();
}

void StudioWindow::changeClipSpeed() {
  if (!scenesEditable() || editGesture_ || !clipSpeed_ ||
      !clipSpeed_->isEnabled())
    return;
  const quint64 id = timeline_->selectedClip();
  if (!id)
    return;
  captureCursor();
  const double speed = clipSpeed_->currentData().toDouble();
  const auto transitions = project_.transitions;
  QString error;
  if (!studioSetClipSpeed(project_, id, speed, error)) {
    if (!error.isEmpty())
      setStatus(error, true);
    refreshSceneControls();
    return;
  }
  player_->pause();
  applyProject(false, player_->position());
  timeline_->setSelectedClip(id);
  rememberEdit();
  setStatus(QStringLiteral("Scene speed %1 — Ctrl+Z to undo")
                .arg(clipSpeed_->currentText()) +
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
