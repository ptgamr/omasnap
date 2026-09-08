/** @fileoverview Explicit pair-bound transitions and their Quattro inspector.
 */
#include "studio-composition.hpp"
#include "studio-playback.hpp"
#include "studio.hpp"
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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
  transitionType_->addItem(QStringLiteral("Hard cut"), -1);
  const std::pair<const char *, StudioTransitionKind> kinds[] = {
      {"Crossfade", StudioTransitionKind::Crossfade},
      {"Fade through black", StudioTransitionKind::FadeBlack},
      {"Wipe left", StudioTransitionKind::WipeLeft},
      {"Wipe right", StudioTransitionKind::WipeRight},
      {"Wipe up", StudioTransitionKind::WipeUp},
      {"Wipe down", StudioTransitionKind::WipeDown},
      {"Slide left", StudioTransitionKind::SlideLeft},
      {"Slide right", StudioTransitionKind::SlideRight},
      {"Slide up", StudioTransitionKind::SlideUp},
      {"Slide down", StudioTransitionKind::SlideDown}};
  for (const auto &[label, kind] : kinds)
    transitionType_->addItem(QString::fromLatin1(label),
                             static_cast<int>(kind));
  transitionType_->setToolTip(QStringLiteral(
      "Transition from the selected scene to its next neighbor · T"));
  controls->addWidget(transitionType_);
  transitionDuration_ = new QSpinBox(this);
  transitionDuration_->setObjectName(QStringLiteral("transitionDuration"));
  transitionDuration_->setKeyboardTracking(false);
  transitionDuration_->setButtonSymbols(QAbstractSpinBox::NoButtons);
  transitionDuration_->setSingleStep(100);
  transitionDuration_->setRange(1, 5000);
  transitionDuration_->setValue(300);
  transitionDuration_->setSuffix(QStringLiteral(" ms overlap"));
  controls->addWidget(transitionDuration_);
  overlapHint_ =
      new QLabel(QStringLiteral("Maximum 300 ms · overlaps kept frames"), this);
  overlapHint_->setWordWrap(true);
  overlapHint_->setObjectName(QStringLiteral("muted"));
  controls->addWidget(overlapHint_);
  connect(transitionType_, &QComboBox::currentIndexChanged, this,
          &StudioWindow::changeTransition);
  connect(transitionDuration_, &QSpinBox::valueChanged, this,
          &StudioWindow::changeTransition);
  connect(timeline_, &StudioTimeline::transitionRequested, this,
          &StudioWindow::showTransitionEditor);
}

std::pair<quint64, quint64> StudioWindow::transitionPair() const {
  const quint64 outgoing = timeline_->selectedTransition()
                               ? timeline_->selectedTransition()
                               : timeline_->selectedClip();
  quint64 incoming = 0;
  for (qsizetype i = 0; i + 1 < project_.clips.size(); ++i)
    if (project_.clips[i].id == outgoing)
      incoming = project_.clips[i + 1].id;
  return {outgoing, incoming};
}

void StudioWindow::refreshTransitionControls(bool force) {
  if (!transitionType_)
    return;
  transitionType_->setChrome(theme_->chrome());
  const QSignalBlocker quietType(transitionType_),
      quietDuration(transitionDuration_);
  const auto [outgoing, incoming] = transitionPair();
  QString name;
  for (qsizetype i = 0; i + 1 < project_.clips.size(); ++i)
    if (project_.clips[i].id == outgoing &&
        project_.clips[i + 1].id == incoming) {
      if (const auto *asset =
              studioAsset(project_, project_.clips[i + 1].assetId))
        name = QFileInfo(asset->path).fileName();
      break;
    }
  const auto *transition = studioTransition(project_, outgoing, incoming);
  const qint64 maximum = studioTransitionMaximum(project_, outgoing, incoming);
  const bool editable = scenesEditable() && incoming && !editGesture_;
  transitionType_->setEnabled(editable);
  transitionDuration_->setEnabled(editable && transition && maximum > 0);
  transitionType_->setCurrentIndex(
      transition ? transitionType_->findData(static_cast<int>(transition->kind))
                 : 0);
  // Never wipe a value being typed: the model write waits for Enter, and a
  // refresh in between must not replace the field from behind. Commits pass
  // force so the field reconciles with normalization or rejection.
  const auto *durationEdit = transitionDuration_->findChild<QLineEdit *>();
  const bool typingDuration = transitionDuration_->hasFocus() ||
                              (durationEdit && durationEdit->hasFocus());
  if (force || !typingDuration) {
    transitionDuration_->setMaximum(static_cast<int>(qMax<qint64>(1, maximum)));
    transitionDuration_->setValue(static_cast<int>(
        transition ? transition->durationMs
                   : qMin<qint64>(300, qMax<qint64>(1, maximum))));
  }
  transitionLabel_->setText(QStringLiteral("Transition to next scene"));
  transitionLabel_->setToolTip(
      incoming ? QStringLiteral("%1 · maximum %2 ms overlap").arg(name).arg(maximum)
               : QStringLiteral("Select a scene with a following neighbor"));
  if (overlapHint_)
    overlapHint_->setText(
        QStringLiteral("Maximum %1 ms · overlaps kept frames").arg(maximum));
  transitionDuration_->setToolTip(QStringLiteral(
      "Consumes the kept tail/head of both scenes; short scenes clamp the "
      "duration. No excluded source frames are revealed."));
}

void StudioWindow::showTransitionEditor(quint64 id) {
  if (!id)
    if (const auto frame = studioFrameAt(project_, player_->position()))
      id = frame->span.clipId;
  if (id) {
    // The last scene has no outgoing boundary: keep the clip selection and
    // say so instead of stranding the inspector on nothing editable.
    bool hasOutgoing = false;
    for (qsizetype i = 0; i + 1 < project_.clips.size(); ++i)
      if (project_.clips[i].id == id)
        hasOutgoing = true;
    if (!hasOutgoing) {
      setStatus(QStringLiteral("The last scene has no outgoing transition"));
      return;
    }
  }
  timeline_->setSelectedTransition(id);
  const auto [outgoing, incoming] = transitionPair();
  if (outgoing && incoming) {
    // Park the playhead at the overlap start so the blend is on screen.
    for (const auto &span : studioComposition(project_))
      if (span.clipId == incoming) {
        player_->pause();
        seekTo(span.startMs);
        break;
      }
  }
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
  const auto [outgoing, incoming] = transitionPair();
  if (!incoming)
    return;
  const int requestedType = transitionType_->currentData().toInt();
  const int requestedDuration = transitionDuration_->value();
  captureCursor();
  const auto anchor = studioFrameAt(project_, player_->position());
  QString error;
  const bool changed =
      requestedType == -1
          ? studioRemoveTransition(project_, outgoing, incoming, error)
          : studioSetTransition(
                project_, outgoing, incoming,
                static_cast<StudioTransitionKind>(requestedType),
                requestedDuration, error);
  if (!changed) {
    if (!error.isEmpty())
      setStatus(error, true);
    refreshTransitionControls(true);
    return;
  }
  const auto position =
      anchor
          ? studioTimelineTime(project_, anchor->span.clipId, anchor->sourceMs)
          : std::nullopt;
  finishCompositionEdit(position.value_or(player_->position()));
  timeline_->setSelectedTransition(outgoing);
  rememberEdit();
  refreshTransitionControls(true);
  const auto *transition = studioTransition(project_, outgoing, incoming);
  setStatus(transition
                ? QStringLiteral("Transition: %1 ms overlap — Ctrl+Z to undo")
                      .arg(transition->durationMs)
                : QStringLiteral("Hard cut restored — Ctrl+Z to undo"));
}

void StudioWindow::removeTransition(quint64 outgoingClipId) {
  if (!deleteButton_->isEnabled() || editGesture_ || !outgoingClipId)
    return;
  quint64 incoming = 0;
  for (qsizetype i = 0; i + 1 < project_.clips.size(); ++i)
    if (project_.clips[i].id == outgoingClipId)
      incoming = project_.clips[i + 1].id;
  if (!incoming) {
    // Stale selection after the pair moved apart: drop it expressly.
    timeline_->setSelectedTransition(0);
    setStatus(QStringLiteral("That scene change is gone — select another boundary"));
    return;
  }
  if (!studioTransition(project_, outgoingClipId, incoming)) {
    setStatus(QStringLiteral("No transition here — pick an effect to add one"));
    return;
  }
  captureCursor();
  QString error;
  if (!studioRemoveTransition(project_, outgoingClipId, incoming, error)) {
    setStatus(error.isEmpty() ? QStringLiteral("Could not remove the transition")
                              : error,
              true);
    return;
  }
  finishCompositionEdit(player_->position());
  // The boundary stays selected as a hard cut, ready for another effect.
  timeline_->setSelectedTransition(outgoingClipId);
  rememberEdit();
  setStatus(QStringLiteral("Transition removed — Ctrl+Z to undo"));
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
