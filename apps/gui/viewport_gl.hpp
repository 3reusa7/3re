#pragma once
// OpenGL 3.3 core viewport: the textured model with NEAREST sampling (every
// texel is a source pixel), a calm gradient background, a fading floor grid,
// selection/hover/issue highlighting, estimated-geometry tint, outlines of
// selected parts and open issues, the source drawing blended over a matching
// view preset, and the M4 display options: live edit preview, face normals,
// pixel preview, texture on/off, mismatch pixels in view presets. Mouse input
// is handled by the Viewport host.
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <memory>

#include "view_state.hpp"

class QOpenGLFunctions_3_3_Core;
class QPainter;

namespace p3dgui {

class GlViewport : public QOpenGLWidget {
  Q_OBJECT
 public:
  GlViewport(ViewState* state, QWidget* parent = nullptr);
  ~GlViewport() override;

  void setScene(const Scene& scene);
  void setHighlight(const Highlight& h);
  bool glReady() const { return ready_; }
  QString glInfo() const { return info_; }

 signals:
  void glFailed(const QString& why);
  void left();

 protected:
  void initializeGL() override;
  void paintGL() override;
  void leaveEvent(QEvent* e) override;

 private:
  struct Gpu;
  void upload();
  void uploadHighlight();
  void uploadPreview();
  void drawScene(const QMatrix4x4& mvp, pixel3d::Dims d);
  // depth_test: visible lines fully, hidden ones faintly unless `hidden_hint` is off.
  void drawLines(const std::vector<float>& xyzrgba, const QMatrix4x4& mvp, bool depth_test, bool hidden_hint = true);
  void drawImageQuad(unsigned tex, pixel3d::ViewRole r, pixel3d::Dims d, const QMatrix4x4& mvp, float alpha);

  ViewState* state_;
  Scene scene_;
  Highlight highlight_;
  bool dirty_ = true, highlight_dirty_ = true, outline_dirty_ = true, preview_dirty_ = true;
  bool ready_ = false;
  QString info_;
  QOpenGLFunctions_3_3_Core* gl_ = nullptr;
  std::unique_ptr<Gpu> gpu_;
  std::vector<float> outline_, issue_boxes_, focus_boxes_, grid_, wire_;  // wire_: scene_.wire_edges, coloured
  pixel3d::Dims grid_dims_{-1, -1, -1};
};

// Line segments (x, y, z, r, g, b, a) of the 12 edges of each box.
void append_box_lines(std::vector<float>& out, const Region3& r, const QColor& c, float inflate = 0.15f);
// Labelled X/Y/Z indicator (OpenGL and software views).
void paint_axis_indicator(QPainter& p, const QRectF& area, const QMatrix4x4& view);

}  // namespace p3dgui
