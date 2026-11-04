#pragma once
// The central 3D view. Uses the OpenGL viewport when OpenGL 3.3 works and
// falls back to the core CPU rasterizer (same cameras) otherwise, so the
// application runs on any machine, including remote desktops and VMs.
// Hosts floating controls (camera bar, display toggles, hint) above the view,
// turns clicks and hovers into picked triangles, frames regions with a short
// camera transition, and handles the mouse for both renderers: camera
// navigation by the chosen scheme, and the editing tools, which only report
// what the user did (a click, a brush stroke on a view preset, a push/pull
// drag); MainWindow turns that into Session commands.
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <optional>
#include <vector>

#include "editor_state.hpp"
#include "pixel3d/render/camera.hpp"
#include "view_state.hpp"

class QStackedLayout;
class QVariantAnimation;

namespace p3dgui {

class GlViewport;

class CpuViewport : public QWidget {
  Q_OBJECT
 public:
  CpuViewport(ViewState* state, QWidget* parent = nullptr);
  void setScene(const Scene& scene);
  void setHighlight(const Highlight& h);
  int triangleAt(QPoint pos) const;  // -1 when nothing is drawn there
  // Geometry of the last painted frame (logical widget coordinates).
  bool hasFrame() const { return !ids_target_.isEmpty(); }
  Ray rayAt(QPointF pos) const;
  QPointF project(pixel3d::vec3d p) const;

 signals:
  void left();

 protected:
  void paintEvent(QPaintEvent* e) override;
  void leaveEvent(QEvent* e) override;

 private:
  ViewState* state_;
  Scene scene_;
  Highlight highlight_;
  std::vector<uint8_t> tri_hl_;  // 0 none, 1 hover, 2 selected
  // Last frame, for picking: triangle id per rendered pixel, where it was drawn, and the camera.
  std::vector<int> tri_ids_;
  QSize ids_size_;
  QRectF ids_target_;  // logical pixels
  pixel3d::Camera cam_;
};

class Viewport : public QWidget {
  Q_OBJECT
 public:
  // force_cpu: never try OpenGL (tests, --software-view).
  explicit Viewport(bool force_cpu, QWidget* parent = nullptr);

  void setScene(const Scene& scene);
  const Scene& scene() const { return scene_; }
  void setHighlight(const Highlight& h);
  const Highlight& highlight() const { return highlight_; }
  void setEditor(EditorState* e) { editor_ = e; }
  ViewState& state() { return state_; }
  void refresh();
  bool usingOpenGL() const;
  QString rendererInfo() const;

  void setPreset(pixel3d::ViewRole r);
  void setIso();
  void setTurntable(bool on);
  bool turntable() const { return spin_.isActive(); }
  // Frames a world box from the current angle (160 ms transition).
  void focusOn(const Region3& r, bool animate = true);
  // Floating control placed in a corner (top-left, top-right), top- or
  // bottom-centre; `offset` moves it further in (e.g. a second row).
  void addOverlay(QWidget* w, Qt::Alignment where, QPoint offset = {});
  // Triangle under a point of this widget, or -1.
  int pickAt(QPoint pos) const;
  // Picking geometry, logical widget coordinates.
  Ray rayAt(QPointF pos) const;
  QPointF project(pixel3d::vec3d p) const;
  // The drawing pixel under `pos` (fractional) when the camera is a view preset with a drawing.
  std::optional<pixel3d::vec2d> viewPixelAt(QPointF pos) const;
  QWidget* surface() const;  // the widget that receives the mouse

 signals:
  void viewStateChanged();
  void rendererChanged(const QString& info);
  void triangleClicked(int tri, Qt::KeyboardModifiers mods);  // -1 = empty space
  void clickedAt(QPoint pos);                                // the click above, where
  void triangleHovered(int tri);
  // A brush stroke on a view preset (Depth and Parts tools), in that drawing's pixels.
  void strokeChanged(pixel3d::ViewRole role, const QVector<QPointF>& stroke, bool finished);
  void strokeCancelled();
  // Push/pull of a face: distance along its normal (snapped when snapping is on).
  void pushPullChanged(int face, double distance, bool finished);
  void pushPullCancelled();
  void hint(const QString& text);

 protected:
  void resizeEvent(QResizeEvent* e) override;
  bool eventFilter(QObject* o, QEvent* e) override;

 private:
  enum class Drag { None, Orbit, Pan, Zoom, Click, Stroke, PushPull };
  void useCpu(const QString& why);
  void placeOverlays();
  Drag navFor(Qt::MouseButton b, Qt::KeyboardModifiers m) const;
  void mousePress(QWidget* w, QMouseEvent* e);
  void mouseMove(QWidget* w, QMouseEvent* e);
  void mouseRelease(QWidget* w, QMouseEvent* e);
  void navigate(Drag d, QPoint delta, QWidget* w);
  void cancelDrag();
  double pushPullDistance(QPointF pos) const;

  ViewState state_;
  Scene scene_;
  Highlight highlight_;
  EditorState* editor_ = nullptr;
  GlViewport* gl_ = nullptr;
  CpuViewport* cpu_ = nullptr;
  QStackedLayout* stack_ = nullptr;
  QString fallback_reason_;
  QTimer spin_;
  QVariantAnimation* focus_anim_ = nullptr;
  struct Overlay {
    QWidget* w;
    Qt::Alignment where;
    QPoint offset;
  };
  std::vector<Overlay> overlays_;
  int last_hover_ = -2;
  // Current mouse drag.
  Drag drag_ = Drag::None;
  QPoint press_, last_;
  bool moved_ = false;
  QVector<QPointF> stroke_;
  pixel3d::ViewRole stroke_role_ = pixel3d::ViewRole::Front;
  int pp_face_ = -1;
  QPointF pp_dir_;  // screen direction of +1 along the normal (pixels per unit)
  double pp_last_ = 0;
};

}  // namespace p3dgui
