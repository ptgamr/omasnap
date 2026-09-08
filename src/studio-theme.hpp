/** @fileoverview Quattro-derived chrome, independent of saved video styling. */
#pragma once

#include <QColor>
#include <QComboBox>
#include <QFutureWatcher>
#include <QObject>
#include <QSlider>
#include <QString>
#include <QTimer>

struct StudioChrome {
  // Quattro Commons/Color.qml defaults. Never require an Omarchy installation.
  QColor background{QStringLiteral("#101315")};
  QColor foreground{QStringLiteral("#cacccc")};
  QColor accent{QStringLiteral("#cacccc")};
  QColor urgent{QStringLiteral("#a55555")};
  QColor muted{QStringLiteral("#707880")};

  static constexpr int radius = 0;
  static constexpr int borderWidth = 1;
  static constexpr int paddingX = 10;
  static constexpr int paddingY = 6;
  static constexpr int gap = 8;
  static constexpr int panelPadding = 18;

  [[nodiscard]] QColor surface() const;
  [[nodiscard]] QColor hover() const;
  [[nodiscard]] QColor pressed() const;
  [[nodiscard]] QColor selected() const;
  [[nodiscard]] QColor border() const;
  /** Quieter than border: hairline dividers between regions. */
  [[nodiscard]] QColor divider() const;
  [[nodiscard]] QColor mutedText() const;
  [[nodiscard]] QColor onAccent() const;
  [[nodiscard]] QString styleSheet() const;
  bool operator==(const StudioChrome &) const = default;
};

/** Parse only the flat six-digit color vocabulary consumed by Quattro.
 * Unknown keys and invalid values are ignored; missing roles use defaults. */
[[nodiscard]] StudioChrome parseStudioChrome(const QByteArray &text);

/** Explicit vector indicator: native style arrows can retain platform colors.
 */
class StudioComboBox final : public QComboBox {
public:
  explicit StudioComboBox(QWidget *parent = nullptr) : QComboBox(parent) {}
  void setChrome(const StudioChrome &chrome);
  void showPopup() override;

protected:
  void paintEvent(QPaintEvent *event) override;

private:
  void stylePopup();
  StudioChrome chrome_;
};

/**
 * Clicking the groove jumps straight to the click and keeps dragging from
 * there: Qt's default groove click only moves one pageStep, which reads as
 * broken next to drag-the-thumb. Presses landing on the style handle rect
 * keep the default drag untouched.
 */
class StudioSlider final : public QSlider {
public:
  using QSlider::QSlider;

protected:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private:
  // Absolute value under the cursor, mapped over the groove travel minus the
  // handle like Qt's own drag math — not over the full widget width, where
  // rounding can strand the handle off the click and retrigger a page step.
  void jumpTo(const QPoint &pos);
  bool grooveDrag_ = false;
};

/** Bounded, asynchronous palette reads. No shell IPC, hooks, or theme plugins.
 * Polling the logical path also handles atomic theme-directory/symlink swaps.
 */
class StudioTheme final : public QObject {
  Q_OBJECT
public:
  explicit StudioTheme(QObject *parent = nullptr, QString path = {});
  [[nodiscard]] const StudioChrome &chrome() const { return chrome_; }
  void reload();

signals:
  void changed();

private:
  QString path_;
  StudioChrome chrome_;
  QTimer timer_;
  QFutureWatcher<StudioChrome> watcher_;
  bool reading_ = false;
  bool pending_ = false;
};
