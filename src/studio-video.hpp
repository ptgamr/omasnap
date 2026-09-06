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
  QRectF drawn;
  QRectF canvas;
  QColor background;
  QColor workspace{QStringLiteral("#101315")};
  qreal radius = 0;
  QRectF source{0, 0, 1, 1};
  int rotation = 0;
  std::function<void(QPainter &)> overlay;
  std::function<void(const QString &)> failed;

protected:
  void initializeGL() override;
  void paintGL() override;

private:
  void releaseResources();
  StudioVideoFrame frame_;
  QOpenGLShaderProgram program_;
  std::array<GLuint, 3> textures_{};
  std::array<QSize, 3> allocated_{};
  int allocatedLayout_ = -1;
  bool dirty_ = false;
};
