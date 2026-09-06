/** @fileoverview Bounded video preparation and GPU YUV/crop rendering. */
#include "studio-video.hpp"

#include <QOpenGLContext>
#include <QPainter>

#include <utility>

StudioVideoFrame prepareStudioVideoFrame(QVideoFrame frame, bool forceRgba) {
  StudioVideoFrame result;
  result.positionMs = qMax<qint64>(0, frame.startTime() / 1000);
  const auto format = frame.surfaceFormat();
  const auto pixel = frame.pixelFormat();
  const bool planar = pixel == QVideoFrameFormat::Format_YUV420P ||
                      pixel == QVideoFrameFormat::Format_YV12;
  const bool nv12 = pixel == QVideoFrameFormat::Format_NV12;
  // Qt handles less common and HDR formats on the worker, never inside an
  // input/frame callback. Screen recordings take the direct YUV path.
  if (forceRgba || (!planar && !nv12) || !frame.map(QVideoFrame::ReadOnly)) {
    const QImage image =
        frame.toImage().convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
      return result;
    result.size = image.size();
    result.textures[0] = {static_cast<int>(image.bytesPerLine() / 4),
                          image.height()};
    result.planes[0] = QByteArray(
        reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
    return result;
  }
  result.size = frame.size();
  result.layout = nv12 ? 2 : 1;
  for (int plane = 0; plane < frame.planeCount(); ++plane) {
    const int index = pixel == QVideoFrameFormat::Format_YV12 && plane > 0
                          ? 3 - plane
                          : plane;
    const int channels = nv12 && plane == 1 ? 2 : 1;
    const int height = plane == 0 ? frame.height() : (frame.height() + 1) / 2;
    result.textures[index] = {frame.bytesPerLine(plane) / channels, height};
    result.planes[index] =
        QByteArray(reinterpret_cast<const char *>(frame.bits(plane)),
                   static_cast<qsizetype>(frame.bytesPerLine(plane)) * height);
  }
  frame.unmap();
  float kr = 0.2126F;
  float kb = 0.0722F;
  // Match FFmpeg's unspecified-YUV conversion. Treating untagged SD media as
  // BT.709 dulls green substantially and makes preview blends disagree with
  // export. Explicit BT.709 recordings retain their declared coefficients.
  if (format.colorSpace() == QVideoFrameFormat::ColorSpace_BT601 ||
      format.colorSpace() == QVideoFrameFormat::ColorSpace_Undefined) {
    kr = 0.299F;
    kb = 0.114F;
  } else if (format.colorSpace() == QVideoFrameFormat::ColorSpace_BT2020) {
    kr = 0.2627F;
    kb = 0.0593F;
  }
  const float kg = 1.0F - kr - kb;
  result.coefficients = {2 * (1 - kr), 2 * (1 - kb), 2 * kb * (1 - kb) / kg,
                         2 * kr * (1 - kr) / kg};
  result.range =
      format.colorRange() == QVideoFrameFormat::ColorRange_Full
          ? QVector4D(0, 1, 128.0F / 255, 1)
          : QVector4D(16.0F / 255, 255.0F / 219, 128.0F / 255, 255.0F / 224);
  return result;
}

StudioVideoSurface::StudioVideoSurface(QWidget *parent)
    : QOpenGLWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setFocusPolicy(Qt::NoFocus);
  QSurfaceFormat fmt;
  fmt.setVersion(2, 0);
  fmt.setProfile(QSurfaceFormat::NoProfile);
  fmt.setDepthBufferSize(0);
  fmt.setStencilBufferSize(0);
  setFormat(fmt);
}

StudioVideoSurface::~StudioVideoSurface() { releaseResources(); }

void StudioVideoSurface::releaseResources() {
  if (!context())
    return;
  disconnect(context(), &QOpenGLContext::aboutToBeDestroyed, this,
             &StudioVideoSurface::releaseResources);
  makeCurrent();
  for (auto &bank : banks_) {
    glDeleteTextures(3, bank.textures.data());
    bank.textures = {};
    bank.allocated = {};
    bank.allocatedLayout = -1;
    bank.dirty = true;
  }
  program_.removeAllShaders();
  doneCurrent();
}

void StudioVideoSurface::setFrame(StudioVideoFrame frame) {
  primary = 0;
  secondary = -1;
  primaryOpacity = 1;
  setFrame(0, std::move(frame));
  multiFrame_ = false;
}

void StudioVideoSurface::setFrame(int slot, StudioVideoFrame frame) {
  multiFrame_ = true;
  banks_[slot].frame = std::move(frame);
  banks_[slot].dirty = true;
  update();
}

void StudioVideoSurface::initializeGL() {
  initializeOpenGLFunctions();
  connect(context(), &QOpenGLContext::aboutToBeDestroyed, this,
          &StudioVideoSurface::releaseResources, Qt::DirectConnection);
  for (auto &bank : banks_)
    glGenTextures(3, bank.textures.data());
  const bool vertexOk =
      program_.addShaderFromSourceCode(QOpenGLShader::Vertex, R"(
    attribute vec2 position;
    attribute vec2 texcoord;
    attribute vec2 corner;
    varying vec2 uv;
    varying vec2 local;
    void main() { uv = texcoord; local = corner; gl_Position = vec4(position, 0.0, 1.0); }
  )");
  const bool fragmentOk =
      program_.addShaderFromSourceCode(QOpenGLShader::Fragment, R"(
    #ifdef GL_ES
    precision highp float;
    #endif
    varying vec2 uv;
    varying vec2 local;
    uniform vec2 cardSize;
    uniform float radius;
    uniform sampler2D plane0;
    uniform sampler2D plane1;
    uniform sampler2D plane2;
    uniform vec3 widths;
    uniform vec4 coefficients;
    uniform vec4 range;
    uniform int videoLayout;
    uniform float gain;
    void main() {
      vec4 first = texture2D(plane0, vec2(uv.x * widths.x, uv.y));
      vec2 q = abs(local - 0.5) * cardSize - (cardSize * 0.5 - radius);
      float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
      float alpha = clamp(0.5 - d, 0.0, 1.0);
      if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, alpha); return;
      }
      if (videoLayout == 0) { gl_FragColor = vec4(first.rgb * gain, first.a * alpha); return; }
      vec4 second = texture2D(plane1, vec2(uv.x * widths.y, uv.y));
      float y = (first.r - range.x) * range.y;
      float u = (second.r - range.z) * range.w;
      float v = ((videoLayout == 2 ? second.a :
        texture2D(plane2, vec2(uv.x * widths.z, uv.y)).r) - range.z) * range.w;
      gl_FragColor = vec4(clamp(vec3(y + coefficients.x * v,
        y - coefficients.z * u - coefficients.w * v,
        y + coefficients.y * u), 0.0, 1.0) * gain, alpha);
    }
  )");
  if (!vertexOk || !fragmentOk || !program_.link()) {
    if (failed)
      failed(QStringLiteral("Could not initialize GPU preview: %1")
                 .arg(program_.log()));
    return;
  }
  for (auto &bank : banks_)
    bank.dirty = true;
}

void StudioVideoSurface::paintGL() {
  glClearColor(workspace.redF(), workspace.greenF(), workspace.blueF(), 1);
  glClear(GL_COLOR_BUFFER_BIT);
  const qreal dpr = devicePixelRatioF();
  glEnable(GL_SCISSOR_TEST);
  glScissor(qRound(canvas.x() * dpr),
            qRound((height() - canvas.bottom()) * dpr),
            qRound(canvas.width() * dpr), qRound(canvas.height() * dpr));
  glClearColor(background.redF(), background.greenF(), background.blueF(), 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_SCISSOR_TEST);
  if (banks_[primary].frame.size.isEmpty() ||
      (secondary >= 0 && secondaryOpacity > 0 &&
       banks_[secondary].frame.size.isEmpty()) ||
      drawn.isEmpty() || !program_.isLinked() || !program_.bind())
    return;
  // Each source is fitted/rotated in canonical coordinates before RGB is
  // weighted. The first pass establishes the black rounded card; additive
  // RGB on the second leaves its style background and alpha edge unchanged.
  drawFrame(primary, primaryOpacity, false);
  if (secondary >= 0 && !banks_[secondary].frame.size.isEmpty())
    drawFrame(secondary, secondaryOpacity, true);
  program_.release();
  glActiveTexture(GL_TEXTURE0);
  if (overlay) {
    QPainter painter(this);
    overlay(painter);
  }
}

void StudioVideoSurface::drawFrame(int slot, double opacity, bool additive) {
  auto &bank = banks_[slot];
  const auto &frame_ = bank.frame;
  const qreal dpr = devicePixelRatioF();
  const int planes = frame_.layout == 0 ? 1 : frame_.layout == 1 ? 3 : 2;
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  for (int plane = 0; plane < planes; ++plane) {
    glActiveTexture(GL_TEXTURE0 + plane);
    glBindTexture(GL_TEXTURE_2D, bank.textures[plane]);
    if (!bank.dirty)
      continue;
    const GLenum format = frame_.layout == 0 ? GL_RGBA
                          : frame_.layout == 2 && plane == 1
                              ? GL_LUMINANCE_ALPHA
                              : GL_LUMINANCE;
    const QSize size = frame_.textures[plane];
    if (bank.allocated[plane] != size ||
        bank.allocatedLayout != frame_.layout) {
      glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), size.width(),
                   size.height(), 0, format, GL_UNSIGNED_BYTE,
                   frame_.planes[plane].constData());
      bank.allocated[plane] = size;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.width(), size.height(),
                      format, GL_UNSIGNED_BYTE,
                      frame_.planes[plane].constData());
    }
  }
  bank.dirty = false;
  bank.allocatedLayout = frame_.layout;
  program_.setUniformValue("gain", static_cast<float>(opacity));
  program_.setUniformValue("plane0", 0);
  program_.setUniformValue("plane1", 1);
  program_.setUniformValue("plane2", 2);
  program_.setUniformValue("videoLayout", frame_.layout);
  program_.setUniformValue("coefficients", frame_.coefficients);
  program_.setUniformValue("range", frame_.range);
  program_.setUniformValue("cardSize",
                           QVector2D(static_cast<float>(drawn.width() * dpr),
                                     static_cast<float>(drawn.height() * dpr)));
  program_.setUniformValue("radius", static_cast<float>(radius * dpr));
  const int chromaPixels = (frame_.size.width() + 1) / 2;
  const float chromaWidth = static_cast<float>(chromaPixels);
  program_.setUniformValue(
      "widths",
      QVector3D(static_cast<float>(frame_.size.width()) /
                    static_cast<float>(frame_.textures[0].width()),
                chromaWidth /
                    static_cast<float>(qMax(1, frame_.textures[1].width())),
                chromaWidth /
                    static_cast<float>(qMax(1, frame_.textures[2].width()))));
  const QRectF fit = multiFrame_ ? fits[slot] : this->fit;
  const int rotation = multiFrame_ ? rotations[slot] : this->rotation;
  const auto uv = [fit, rotation](QPointF p) {
    p = {(p.x() - fit.x()) / fit.width(), (p.y() - fit.y()) / fit.height()};
    if (rotation == 90)
      return QPointF(p.y(), 1 - p.x());
    if (rotation == 180)
      return QPointF(1 - p.x(), 1 - p.y());
    if (rotation == 270)
      return QPointF(1 - p.y(), p.x());
    return p;
  };
  const std::array<QPointF, 4> points{drawn.topLeft(), drawn.bottomLeft(),
                                      drawn.topRight(), drawn.bottomRight()};
  const std::array<QPointF, 4> coords{
      uv(source.topLeft()), uv(source.bottomLeft()), uv(source.topRight()),
      uv(source.bottomRight())};
  std::array<GLfloat, 8> positions{}, coordinates{};
  for (size_t i = 0; i < points.size(); ++i) {
    positions[i * 2] = static_cast<float>(points[i].x() / width() * 2 - 1);
    positions[i * 2 + 1] = static_cast<float>(1 - points[i].y() / height() * 2);
    coordinates[i * 2] = static_cast<float>(coords[i].x());
    coordinates[i * 2 + 1] = static_cast<float>(coords[i].y());
  }
  program_.enableAttributeArray("position");
  program_.enableAttributeArray("texcoord");
  const std::array<GLfloat, 8> corners{0, 0, 0, 1, 1, 0, 1, 1};
  program_.enableAttributeArray("corner");
  program_.setAttributeArray("position", GL_FLOAT, positions.data(), 2);
  program_.setAttributeArray("texcoord", GL_FLOAT, coordinates.data(), 2);
  program_.setAttributeArray("corner", GL_FLOAT, corners.data(), 2);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisable(GL_BLEND);
  program_.disableAttributeArray("position");
  program_.disableAttributeArray("texcoord");
  program_.disableAttributeArray("corner");
}
