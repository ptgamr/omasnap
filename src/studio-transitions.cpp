/** @fileoverview Explicit pair-bound transitions and their Quattro inspector.
 */
#include "studio-playback.hpp"
#include "studio.hpp"
#include <QFileInfo>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

void StudioWindow::setupTransitions(QVBoxLayout *controls) {
  transitionLabel_ =
      new QLabel(QStringLiteral("Transition to next scene"), this);
  transitionLabel_->setWordWrap(true);
  controls->addWidget(transitionLabel_);
  transitionType_ = new StudioComboBox(this);
  transitionType_->setObjectName(QStringLiteral("transitionType"));
  transitionType_->setChrome(theme_->chrome());
  transitionType_->addItems({QStringLiteral("Hard cut"),
                             QStringLiteral("Crossfade"),
                             QStringLiteral("Fade through black")});
  transitionType_->setToolTip(QStringLiteral(
      "Transition from the selected scene to its next neighbor · T"));
  controls->addWidget(transitionType_);
  transitionDuration_ = new QSpinBox(this);
  transitionDuration_->setObjectName(QStringLiteral("transitionDuration"));
  transitionDuration_->setKeyboardTracking(false);
  transitionDuration_->setButtonSymbols(QAbstractSpinBox::NoButtons);
  transitionDuration_->setRange(1, 5000);
  transitionDuration_->setValue(300);
  transitionDuration_->setSuffix(QStringLiteral(" ms overlap"));
  controls->addWidget(transitionDuration_);
  auto *hint = new QLabel(
      QStringLiteral("Transitions overlap kept frames. Remove a transition "
                     "before cutting through its overlap."),
      this);
  hint->setWordWrap(true);
  hint->setObjectName(QStringLiteral("muted"));
  controls->addWidget(hint);
  connect(transitionType_, &QComboBox::currentIndexChanged, this,
          &StudioWindow::changeTransition);
  connect(transitionDuration_, &QSpinBox::valueChanged, this,
          &StudioWindow::changeTransition);
  connect(timeline_, &StudioTimeline::transitionRequested, this,
          &StudioWindow::showTransitionEditor);
}

void StudioWindow::refreshTransitionControls() {
  if (!transitionType_)
    return;
  transitionType_->setChrome(theme_->chrome());
  const QSignalBlocker quietType(transitionType_),
      quietDuration(transitionDuration_);
  quint64 incoming = 0;
  QString name;
  for (qsizetype i = 0; i + 1 < project_.clips.size(); ++i)
    if (project_.clips[i].id == timeline_->selectedClip()) {
      incoming = project_.clips[i + 1].id;
      const auto *asset = studioAsset(project_, project_.clips[i + 1].assetId);
      if (asset)
        name = QFileInfo(asset->path).fileName();
      break;
    }
  const auto outgoing = timeline_->selectedClip();
  const auto *transition = studioTransition(project_, outgoing, incoming);
  const qint64 maximum = studioTransitionMaximum(project_, outgoing, incoming);
  const bool editable = scenesEditable() && incoming && !editGesture_;
  transitionType_->setEnabled(editable);
  transitionDuration_->setEnabled(editable && transition && maximum > 0);
  transitionType_->setCurrentIndex(
      !transition                                           ? 0
      : transition->kind == StudioTransitionKind::Crossfade ? 1
                                                            : 2);
  transitionDuration_->setMaximum(static_cast<int>(qMax<qint64>(1, maximum)));
  transitionDuration_->setValue(static_cast<int>(
      transition ? transition->durationMs
                 : qMin<qint64>(300, qMax<qint64>(1, maximum))));
  transitionLabel_->setText(
      incoming ? QStringLiteral(
                     "Transition to %1\nMaximum %2 ms · frame-snapped overlap")
                     .arg(name)
                     .arg(maximum)
               : QStringLiteral("Select a scene with a following neighbor"));
  transitionDuration_->setToolTip(QStringLiteral(
      "Consumes the kept tail/head of both scenes; short scenes clamp the "
      "duration. No excluded source frames are revealed."));
}

void StudioWindow::showTransitionEditor(quint64 id) {
  if (!id)
    if (const auto frame = studioFrameAt(project_, player_->position()))
      id = frame->span.clipId;
  timeline_->setSelectedClip(id);
  auto *tabs = inspector_->findChild<QTabWidget *>();
  if (tabs)
    for (int i = 0; i < tabs->count(); ++i)
      if (tabs->tabText(i) == QStringLiteral("Clip"))
        tabs->setCurrentIndex(i);
  inspectorWanted_ = true;
  inspector_->show();
  if (auto *scroll = inspector_->findChild<QScrollArea *>())
    scroll->ensureWidgetVisible(transitionType_);
  transitionType_->setFocus();
}

void StudioWindow::changeTransition() {
  if (restoring_ || !scenesEditable() || editGesture_)
    return;
  const quint64 outgoing = timeline_->selectedClip();
  quint64 incoming = 0;
  for (qsizetype i = 0; i + 1 < project_.clips.size(); ++i)
    if (project_.clips[i].id == outgoing)
      incoming = project_.clips[i + 1].id;
  if (!incoming)
    return;
  const int requestedType = transitionType_->currentIndex();
  const int requestedDuration = transitionDuration_->value();
  captureCursor();
  const auto anchor = studioFrameAt(project_, player_->position());
  QString error;
  const bool changed =
      requestedType == 0
          ? studioRemoveTransition(project_, outgoing, incoming, error)
          : studioSetTransition(project_, outgoing, incoming,
                                requestedType == 1
                                    ? StudioTransitionKind::Crossfade
                                    : StudioTransitionKind::FadeBlack,
                                requestedDuration, error);
  if (!changed) {
    if (!error.isEmpty())
      setStatus(error, true);
    refreshTransitionControls();
    return;
  }
  const auto position =
      anchor
          ? studioTimelineTime(project_, anchor->span.clipId, anchor->sourceMs)
          : std::nullopt;
  finishCompositionEdit(position.value_or(player_->position()));
  timeline_->setSelectedClip(outgoing);
  rememberEdit();
  const auto *transition = studioTransition(project_, outgoing, incoming);
  setStatus(transition
                ? QStringLiteral("Transition: %1 ms overlap — Ctrl+Z to undo")
                      .arg(transition->durationMs)
                : QStringLiteral("Hard cut restored — Ctrl+Z to undo"));
}

QString StudioWindow::transitionAdjustment(
    const QVector<StudioTransition> &before) const {
  int removed = 0, shortened = 0;
  for (const auto &transition : before) {
    const auto *after = studioTransition(project_, transition.outgoingClipId,
                                         transition.incomingClipId);
    if (!after)
      ++removed;
    else if (after->durationMs < transition.durationMs)
      ++shortened;
  }
  if (!removed && !shortened)
    return {};
  return QStringLiteral(" · Transitions: %1 removed, %2 shortened")
      .arg(removed)
      .arg(shortened);
}
