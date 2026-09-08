/** @fileoverview GPU video surface and worker-prepared texture planes. */
#pragma once

#include <QByteArray>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QVideoFrame>

#include <array>
#include <functional>

struct StudioVideoFrame {
  QSize size;
  qint64 positionMs = 0;
  // 0 = RGBA, 1 = planar YUV, 2 = interleaved UV.
  int layout = 0;
  std::array<QByteArray, 3> planes;
  std::array<QSize, 3> textures;
  QVector4D coefficients;
  QVector4D range;
};

/** Mapping/readback and uncommon-format conversion belong on a worker. */
[[nodiscard]] StudioVideoFrame prepareStudioVideoFrame(QVideoFrame frame,
                                                       bool forceRgba = false);

class StudioVideoSurface final : public QOpenGLWidget,
                                 protected QOpenGLFunctions {
public:
  explicit StudioVideoSurface(QWidget *parent);
  ~StudioVideoSurface() override;
  void setFrame(StudioVideoFrame frame);
  void setFrame(int slot, StudioVideoFrame frame);
  int primary = 0;
  int secondary = -1;
  double primaryOpacity = 1;
  double secondaryOpacity = 0;
  std::array<QPointF, 2> offsets{};
  std::array<QRectF, 2> clips{QRectF(0, 0, 1, 1), QRectF(0, 0, 1, 1)};
  std::array<QRectF, 2> fits{QRectF(0, 0, 1, 1), QRectF(0, 0, 1, 1)};
  std::array<int, 2> rotations{};
  QRectF drawn;
  QRectF canvas;
  // Content frame letterboxed inside the card; empty means same as drawn.
  QRectF content;
  QColor background;
  // Three-stop linear gradient replacing the flat clear when set. Unit
  // endpoints share StudioStyle's top-left origin; an unlinked gradient
  // program falls back to the flat `background` (the middle stop).
  bool backgroundIsGradient = false;
  std::array<QVector3D, 3> backgroundStops{};
  QPointF backgroundStart{};
  QPointF backgroundEnd{1, 1};
  // Shadow strength 0..1 darkening the canvas under the content silhouette.
  float shadowStrength = 0;
  // Stretched wallpaper winning over both, uploaded once per image. The key
  // is the image cache key, so identical pixels never re-upload.
  QImage wallpaper;
  qint64 wallpaperTextureKey = 0;
  QColor workspace{QStringLiteral("#101315")};
  qreal radius = 0;
  QRectF source{0, 0, 1, 1};
  // Display-oriented source rectangle inside the canonical composition.
  QRectF fit{0, 0, 1, 1};
  int rotation = 0;
  std::function<void(QPainter &)> overlay;
  std::function<void(const QString &)> failed;

protected:
  void initializeGL() override;
  void paintGL() override;

private:
  void releaseResources();
  void drawFrame(int slot, double opacity, bool additive);
  struct TextureBank {
    StudioVideoFrame frame;
    std::array<GLuint, 3> textures{};
    std::array<QSize, 3> allocated{};
    int allocatedLayout = -1;
    bool dirty = false;
  };
  std::array<TextureBank, 2> banks_;
  bool multiFrame_ = false;
  QOpenGLShaderProgram program_;
  QOpenGLShaderProgram gradientProgram_;
  QOpenGLShaderProgram wallpaperProgram_;
  QOpenGLShaderProgram shadowProgram_;
  GLuint wallpaperTexture = 0;
  void ensureWallpaperTexture();
};
