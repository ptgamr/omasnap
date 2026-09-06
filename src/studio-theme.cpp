/** @fileoverview Studio's native Quattro palette and shared control vocabulary.
 */
#include "studio-theme.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
QColor mix(const QColor &base, const QColor &ink, qreal amount) {
  return QColor::fromRgbF(
      static_cast<float>(base.redF() * (1 - amount) + ink.redF() * amount),
      static_cast<float>(base.greenF() * (1 - amount) + ink.greenF() * amount),
      static_cast<float>(base.blueF() * (1 - amount) + ink.blueF() * amount));
}

qreal luminance(const QColor &color) {
  const auto linear = [](qreal value) {
    return value <= 0.04045 ? value / 12.92
                            : std::pow((value + 0.055) / 1.055, 2.4);
  };
  return linear(color.redF()) * 0.2126 + linear(color.greenF()) * 0.7152 +
         linear(color.blueF()) * 0.0722;
}

qreal contrast(const QColor &a, const QColor &b) {
  const qreal first = luminance(a) + 0.05;
  const qreal second = luminance(b) + 0.05;
  return qMax(first, second) / qMin(first, second);
}
} // namespace

QColor StudioChrome::surface() const {
  return mix(background, foreground, 0.04);
}
QColor StudioChrome::hover() const { return mix(background, foreground, 0.08); }
QColor StudioChrome::pressed() const {
  return mix(background, foreground, 0.22);
}
QColor StudioChrome::selected() const {
  return mix(background, foreground, 0.18);
}
QColor StudioChrome::border() const { return mix(background, foreground, 0.4); }
QColor StudioChrome::mutedText() const {
  // Some terminal palettes use color8 for near-invisible text. Keep the hue,
  // but make small Studio labels readable against both chrome surfaces.
  for (int step = 0; step <= 20; ++step) {
    const QColor candidate = mix(muted, foreground, step / 20.0);
    if (contrast(candidate, surface()) >= 4.5 &&
        contrast(candidate, background) >= 4.5)
      return candidate;
  }
  return foreground;
}
QColor StudioChrome::onAccent() const {
  return contrast(Qt::black, accent) >= contrast(Qt::white, accent)
             ? QColor(Qt::black)
             : QColor(Qt::white);
}

StudioChrome parseStudioChrome(const QByteArray &text) {
  StudioChrome chrome;
  QHash<QString, QColor> colors;
  const QRegularExpression assignment(QStringLiteral(
      R"regex(^\s*([A-Za-z0-9_-]+)\s*=\s*(?:"(#[0-9A-Fa-f]{6})"|'(#[0-9A-Fa-f]{6})'|(#[0-9A-Fa-f]{6}))\s*(?:#.*)?$)regex"));
  for (const QString &line : QString::fromUtf8(text).split(QLatin1Char('\n'))) {
    const auto match = assignment.match(line);
    if (!match.hasMatch())
      continue;
    const QString value = !match.captured(2).isEmpty()   ? match.captured(2)
                          : !match.captured(3).isEmpty() ? match.captured(3)
                                                         : match.captured(4);
    const QString key = match.captured(1);
    if (key == QStringLiteral("background") ||
        key == QStringLiteral("foreground") ||
        key == QStringLiteral("accent") || key == QStringLiteral("muted") ||
        key == QStringLiteral("red") || key == QStringLiteral("color0") ||
        key == QStringLiteral("color1") || key == QStringLiteral("color4") ||
        key == QStringLiteral("color7") || key == QStringLiteral("color8"))
      colors.insert(key, QColor(value));
  }
  chrome.background =
      colors.value(QStringLiteral("background"),
                   colors.value(QStringLiteral("color0"), chrome.background));
  chrome.foreground =
      colors.value(QStringLiteral("foreground"),
                   colors.value(QStringLiteral("color7"), chrome.foreground));
  chrome.accent =
      colors.value(QStringLiteral("accent"),
                   colors.value(QStringLiteral("color4"), chrome.accent));
  chrome.muted =
      colors.value(QStringLiteral("muted"),
                   colors.value(QStringLiteral("color8"), chrome.foreground));
  chrome.urgent =
      colors.value(QStringLiteral("red"),
                   colors.value(QStringLiteral("color1"), chrome.urgent));
  // A wholly absent/invalid file is the built-in palette, not a previous theme.
  return colors.isEmpty() ? StudioChrome{} : chrome;
}

StudioTheme::StudioTheme(QObject *parent, QString path)
    : QObject(parent),
      path_(path.isEmpty()
                ? QDir::homePath() +
                      QStringLiteral(
                          "/.local/state/omarchy/current/theme/colors.toml")
                : std::move(path)) {
  timer_.setInterval(1000);
  connect(&timer_, &QTimer::timeout, this, &StudioTheme::reload);
  connect(&watcher_, &QFutureWatcher<StudioChrome>::finished, this, [this] {
    reading_ = false;
    const StudioChrome next = watcher_.result();
    if (next != chrome_) {
      chrome_ = next;
      emit changed();
    }
    if (std::exchange(pending_, false))
      reload();
  });
  timer_.start();
  reload();
}

void StudioTheme::reload() {
  if (reading_) {
    pending_ = true;
    return;
  }
  reading_ = true;
  const QString path = path_;
  watcher_.setFuture(QtConcurrent::run([path] {
    QFile file(path);
    constexpr qint64 limit = 64 * 1024LL;
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) ||
        file.size() > limit)
      return StudioChrome{};
    const QByteArray text = file.read(limit + 1);
    return text.size() > limit ? StudioChrome{} : parseStudioChrome(text);
  }));
}

QString StudioChrome::styleSheet() const {
  QString sheet = QStringLiteral(R"(
QWidget { color: @text; font-family: monospace; }
QWidget#studioHeader, QWidget#studioInspector, QWidget#timelinePanel, QDialog { background: @background; }
QLabel#muted, QLabel#section { color: @muted; }
QLabel[studioError="true"] { color: @urgent; }
QPushButton {
  background: @surface; border: @borderWidthpx solid @border;
  border-radius: @radiuspx; padding: @paddingYpx @paddingXpx;
}
QPushButton:checked { background: @selected; border-color: @accent; }
QPushButton:hover:enabled { background: @hover; border-color: @text; }
QPushButton:focus:enabled { background: @hover; border-color: @accent; }
QPushButton:pressed:enabled { background: @pressed; border-color: @accent; }
QPushButton:disabled { color: @muted; border-color: @selected; background: @background; }
QPushButton#primary:enabled { background: @accent; color: @onAccent; border-color: @accent; font-weight: 600; }
QPushButton#primary:hover:enabled, QPushButton#primary:focus:enabled { border-color: @text; }
QPushButton#primary:pressed:enabled { background: @text; color: @background; }
QPushButton#play { min-width: 40px; }
QSlider { border: 1px solid transparent; }
QSlider::groove:horizontal { height: 3px; background: @border; border-radius: @radiuspx; }
QSlider::sub-page:horizontal { background: @accent; }
QSlider::handle:horizontal { background: @accent; border: 1px solid @accent; width: 8px; margin: -5px 0; border-radius: @radiuspx; }
QSlider::handle:horizontal:hover, QSlider::handle:horizontal:focus { border-color: @text; background: @text; }
QSlider::handle:horizontal:disabled, QSlider::sub-page:horizontal:disabled { background: @selected; border-color: @border; }
QSlider:focus { border: 1px solid @accent; }
QSpinBox, QComboBox, QLineEdit { background: @surface; color: @text; border: 1px solid @border; border-radius: @radiuspx; padding: 6px; selection-background-color: @selected; selection-color: @text; }
QSpinBox:focus, QComboBox:focus, QLineEdit:focus { background: @hover; border-color: @accent; }
QSpinBox:disabled, QComboBox:disabled { color: @muted; border-color: @selected; }
QSpinBox QLineEdit { background: transparent; border: 0; padding: 0; }
QComboBox::drop-down { width: 18px; border: 0; }
QComboBox::down-arrow { image: none; width: 0; height: 0; border: 0; }
QAbstractItemView { background: @background; color: @text; border: 1px solid @border; selection-background-color: @selected; selection-color: @text; outline: 0; }
QTabWidget::pane { border: 0; }
QTabBar { border: 1px solid transparent; }
QTabBar::tab { background: @background; color: @muted; padding: 8px 10px; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { background: @selected; color: @text; border-bottom-color: @accent; }
QTabBar::tab:hover { background: @hover; color: @text; }
QTabBar:focus { border: 1px solid @accent; }
QScrollArea { border: 0; background: transparent; }
QScrollBar:vertical { background: @background; width: 8px; }
QScrollBar::handle:vertical { background: @border; border-radius: @radiuspx; min-height: 24px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: @background; }
QToolTip, QMenu { background: @background; color: @text; border: 1px solid @border; border-radius: @radiuspx; padding: 6px; }
QMenu::item:selected { background: @selected; }
QSplitter::handle { background: @border; width: 1px; }
)");
  const QHash<QString, QString> values{
      {QStringLiteral("background"), background.name()},
      {QStringLiteral("text"), foreground.name()},
      {QStringLiteral("accent"), accent.name()},
      {QStringLiteral("onAccent"), onAccent().name()},
      {QStringLiteral("urgent"), urgent.name()},
      {QStringLiteral("muted"), mutedText().name()},
      {QStringLiteral("surface"), surface().name()},
      {QStringLiteral("hover"), hover().name()},
      {QStringLiteral("pressed"), pressed().name()},
      {QStringLiteral("selected"), selected().name()},
      {QStringLiteral("border"), border().name()},
      {QStringLiteral("borderWidthpx"),
       QString::number(borderWidth) + QStringLiteral("px")},
      {QStringLiteral("radiuspx"),
       QString::number(radius) + QStringLiteral("px")},
      {QStringLiteral("paddingXpx"),
       QString::number(paddingX) + QStringLiteral("px")},
      {QStringLiteral("paddingYpx"),
       QString::number(paddingY) + QStringLiteral("px")}};
  // Longest keys first: @border must not consume @borderWidthpx.
  QStringList keys = values.keys();
  std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) {
    return a.size() > b.size();
  });
  for (const QString &key : keys)
    sheet.replace(QLatin1Char('@') + key, values.value(key));
  return sheet;
}

void StudioComboBox::paintEvent(QPaintEvent *event) {
  QComboBox::paintEvent(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(
      QPen(isEnabled() ? chrome_.foreground : chrome_.mutedText(), 1.5));
  const qreal x = width() - 12;
  const qreal y = height() / 2.0;
  painter.drawPolyline(
      QPolygonF{{x - 3, y - 1.5}, {x, y + 1.5}, {x + 3, y - 1.5}});
}
