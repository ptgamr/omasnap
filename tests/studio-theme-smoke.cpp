/** @fileoverview Palette contract, atomic reload, fallback, and visual
 * isolation. */
#include "studio-theme-smoke.hpp"
#include "studio-preview.hpp"
#include "studio-theme.hpp"
#include "studio.hpp"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {
bool writePalette(const QString &path, const QByteArray &text) {
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(text) == text.size() &&
         file.commit();
}
} // namespace

bool runStudioThemeChecks(QString &error) {
  const auto require = [&error](bool ok, const char *message) {
    if (!ok)
      error = QString::fromLatin1(message);
    return ok;
  };
  const StudioChrome fallback;
  const QByteArray dark =
      "background = \"#101820\"\nforeground = '#dedede'\naccent = "
      "\"#70b080\"\ncolor4 = \"#aa0000\"\nmuted = \"#555555\"\nred = "
      "\"#ee7788\"\n";
  const QByteArray light = "background = \"#fafafa\"\nforeground = "
                           "\"#202020\"\naccent = \"#3256a0\"\n";
  const auto parsed = parseStudioChrome(dark);
  if (!require(parsed.background == QColor("#101820") &&
                   parsed.foreground == QColor("#dedede") &&
                   parsed.accent == QColor("#70b080") &&
                   parsed.urgent == QColor("#ee7788"),
               "explicit palette roles or accent precedence are wrong"))
    return false;
  const auto ansi =
      parseStudioChrome("color0 = '#123456' # bg\ncolor7 = '#abcdef'\ncolor4 = "
                        "'#345678'\ncolor8 = '#777777'\ncolor1 = '#ff0000'\n");
  if (!require(ansi.background == QColor("#123456") &&
                   ansi.foreground == QColor("#abcdef") &&
                   ansi.accent == QColor("#345678") &&
                   ansi.urgent == QColor("#ff0000"),
               "ANSI palette fallbacks are wrong"))
    return false;
  if (!require(parseStudioChrome({}) == fallback &&
                   parseStudioChrome(
                       "accent = '#12345z'\nforeground = "
                       "'#123456junk'\nbackground = 'red'; injected\n") ==
                       fallback,
               "empty or malformed palette did not use defaults"))
    return false;
  for (const StudioChrome &chrome :
       {fallback, parsed, parseStudioChrome(light)}) {
    if (!require(
            !chrome.styleSheet().contains(QLatin1Char('@')) &&
                chrome.styleSheet().contains(
                    QStringLiteral("border-radius: 0px")) &&
                chrome.styleSheet().contains(chrome.background.name()) &&
                chrome.onAccent() != chrome.accent,
            "stylesheet tokens, square corners, or accent contrast are wrong"))
      return false;
  }

  QTemporaryDir scratch;
  const QString directory = scratch.filePath(QStringLiteral("theme"));
  QDir().mkpath(directory);
  const QString path = directory + QStringLiteral("/colors.toml");
  if (!require(writePalette(path, dark), "could not write palette fixture"))
    return false;
  StudioTheme theme(nullptr, path);
  QSignalSpy changes(&theme, &StudioTheme::changed);
  if (!require(QTest::qWaitFor([&] { return theme.chrome() == parsed; }, 3000),
               "async initial palette load failed"))
    return false;
  const qsizetype count = changes.size();
  theme.reload();
  QTest::qWait(100);
  if (!require(changes.size() == count,
               "unchanged palette emitted redundant restyling"))
    return false;
  if (!require(writePalette(path, light), "could not replace palette fixture"))
    return false;
  if (!require(
          QTest::qWaitFor(
              [&] { return theme.chrome() == parseStudioChrome(light); }, 3000),
          "automatic reload missed atomic replacement"))
    return false;
  // A theme switch can replace the containing directory rather than the file.
  if (!require(QDir().rename(directory, directory + QStringLiteral("-old")) &&
                   QDir().mkpath(directory) && writePalette(path, dark),
               "could not replace theme directory"))
    return false;
  if (!require(QTest::qWaitFor([&] { return theme.chrome() == parsed; }, 3000),
               "automatic reload stayed attached to old theme directory"))
    return false;
  if (!require(writePalette(path, QByteArray(65537, 'x')),
               "could not write oversized fixture"))
    return false;
  theme.reload();
  if (!require(
          QTest::qWaitFor([&] { return theme.chrome() == fallback; }, 3000),
          "oversized palette was not rejected"))
    return false;
  if (!require(writePalette(path, light), "could not restore palette fixture"))
    return false;
  theme.reload();
  if (!require(
          QTest::qWaitFor([&] { return theme.chrome() != fallback; }, 3000),
          "palette did not recover from fallback"))
    return false;
  QFile::remove(path);
  theme.reload();
  if (!require(
          QTest::qWaitFor([&] { return theme.chrome() == fallback; }, 3000),
          "missing palette retained stale theme"))
    return false;

  StudioPreview preview;
  preview.resize(400, 300);
  preview.setCanvasInset(20);
  preview.setPickable(false);
  QImage source(320, 180, QImage::Format_RGB32);
  source.fill(Qt::red);
  preview.setFrame(source);
  const StudioStyle style{1, 10, 32};
  preview.setStyle(style);
  preview.setChrome(parsed);
  const QImage before = preview.grab().toImage();
  preview.setChrome(parseStudioChrome(light));
  const QImage after = preview.grab().toImage();
  const qreal scale = before.devicePixelRatio();
  if (!require(
          before.pixelColor(0, 0) == parsed.background &&
              after.pixelColor(0, 0) == parseStudioChrome(light).background &&
              before.copy(QRect(qRound(20 * scale), qRound(49 * scale),
                                qRound(360 * scale), qRound(202 * scale))) ==
                  after.copy(QRect(qRound(20 * scale), qRound(49 * scale),
                                   qRound(360 * scale), qRound(202 * scale))),
          "theme change recolored the video canvas or missed the surround"))
    return false;
  // Chrome is not an export argument, even for styled compositions.
  const auto args = studioExportArguments("source.mp4", "out.mp4", 0, 1000, {},
                                          {{320, 180}, 30, 1, 0, false}, style);
  preview.setChrome(fallback);
  return require(
      args == studioExportArguments("source.mp4", "out.mp4", 0, 1000, {},
                                    {{320, 180}, 30, 1, 0, false}, style),
      "chrome changed export styling");
}
