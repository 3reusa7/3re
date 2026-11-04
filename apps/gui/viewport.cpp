#include "viewport.hpp"

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStackedLayout>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#include "pixel3d/edit/selection.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"
#include "qt_util.hpp"
#include "theme.hpp"
#include "viewport_gl.hpp"

namespace p3dgui {

using namespace pixel3d;

namespace {

// Same tints as the OpenGL shader, applied to a rendered pixel.
Rgba8 tint(Rgba8 c, uint8_t flags, uint8_t hl, const ViewState& s) {
  const theme::Colors& tc = theme::colors();
  auto mix = [&](const QColor& to, double f) {
    c.r = static_cast<uint8_t>(c.r + (to.red() - c.r) * f);
    c.g = static_cast<uint8_t>(c.g + (to.green() - c.g) * f);
    c.b = static_cast<uint8_t>(c.b + (to.blue() - c.b) * f);
  };
  if (s.show_issues && (flags & kTriUndecided)) mix(tc.review, 0.42);
  if (s.show_estimated && (flags & (kTriEstimatedPart | kTriEstimatedSurface))) mix(tc.info, 0.45);
  if (hl == 2) mix(tc.accent, 0.30);
  else if (hl == 1) mix(tc.hover_tint, 0.12);
  return c;
}

}  // namespace

// ---- CPU fallback -----------------------------------------------------------

CpuViewport::CpuViewport(ViewState* state, QWidget* parent) : QWidget(parent), state_(state) {
  setMinimumSize(320, 240);
  setAutoFillBackground(false);
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
}

void CpuViewport::setScene(const Scene& scene) {
  scene_ = scene;
  setHighlight(highlight_);
  update();
}

void CpuViewport::setHighlight(const Highlight& h) {
  highlight_ = h;
  const size_t T = scene_.triangles();
  tri_hl_.assign(T, 0);
  for (size_t t = 0; t < T; ++t) {
    const int part = t < scene_.tri_part.size() ? scene_.tri_part[t] : -1;
    const int region = t < scene_.tri_region.size() ? scene_.tri_region[t] : -1;
    const bool sel = h.region >= 0 ? region == h.region : h.parts.contains(part);
    const bool hov = h.hover_region >= 0 ? region == h.hover_region : h.hover_part >= 0 && part == h.hover_part;
    tri_hl_[t] = sel ? 2 : hov ? 1 : 0;
  }
  update();
}

int CpuViewport::triangleAt(QPoint pos) const {
  if (tri_ids_.empty() || !ids_target_.contains(pos)) return -1;
  const int x = static_cast<int>((pos.x() - ids_target_.x()) / ids_target_.width() * ids_size_.width());
  const int y = static_cast<int>((pos.y() - ids_target_.y()) / ids_target_.height() * ids_size_.height());
  if (x < 0 || y < 0 || x >= ids_size_.width() || y >= ids_size_.height()) return -1;
  return tri_ids_[static_cast<size_t>(y) * ids_size_.width() + x];
}

Ray CpuViewport::rayAt(QPointF pos) const {
  // Rendered pixel coordinates of `pos`, then back through the orthographic camera.
  const double sx = (pos.x() - ids_target_.x()) / ids_target_.width() * ids_size_.width();
  const double sy = (pos.y() - ids_target_.y()) / ids_target_.height() * ids_size_.height();
  const vec3d on_plane = cam_.eye + cam_.right * ((sx - cam_.offset.x) / cam_.scale) +
                         cam_.up * ((cam_.offset.y - sy) / cam_.scale);
  const double back = 4.0 * length(vec3d{double(scene_.dims.W), double(scene_.dims.H), double(scene_.dims.D)}) + 1;
  return Ray{on_plane - cam_.forward * back, cam_.forward};
}

QPointF CpuViewport::project(vec3d p) const {
  const vec3d s = cam_.project(p);
  return {ids_target_.x() + s.x / ids_size_.width() * ids_target_.width(),
          ids_target_.y() + s.y / ids_size_.height() * ids_target_.height()};
}

void CpuViewport::paintEvent(QPaintEvent*) {
  const theme::Colors& tc = theme::colors();
  QPainter p(this);
  QLinearGradient bg(0, 0, 0, height());
  bg.setColorAt(0, tc.viewport_top);
  bg.setColorAt(1, tc.viewport_bottom);
  p.fillRect(rect(), bg);
  tri_ids_.clear();
  ids_target_ = {};
  if (scene_.empty()) return;
  const qreal dpr = devicePixelRatioF();
  const int w = std::max(16, static_cast<int>(width() * dpr)), h = std::max(16, static_cast<int>(height() * dpr));
  const bool previewing = highlight_.preview != nullptr;
  const TexturedMesh& mesh = previewing ? *highlight_.preview : *scene_.mesh;
  RenderOptions ro;
  ro.lit = state_->lit;
  ro.textured = scene_.textured && state_->textured && !previewing;
  ro.smooth_texture = scene_.smooth_texture;
  Camera cam;
  int shown_w = 0, shown_h = 0;  // device pixels the rendered image covers
  if (state_->preset) {
    // Exactly the source-view camera, scaled to fit; pixel preview renders 1:1 and enlarges.
    const ImageSize sz = view_image_size(*state_->preset, scene_.dims);
    const int ppu = std::clamp(static_cast<int>(std::floor(std::min(w / std::max(1, sz.w), h / std::max(1, sz.h)) * 0.9 *
                                                           state_->zoom)),
                               1, 64);
    cam = camera_for_view(*state_->preset, scene_.dims, state_->pixel_preview ? 1 : ppu);
    shown_w = sz.w * ppu;
    shown_h = sz.h * ppu;
  } else {
    const int side = std::min(std::min(w, h), 1024);
    const double radius = 0.5 * length(vec3d{double(scene_.dims.W), double(scene_.dims.H), double(scene_.dims.D)});
    const int k = state_->pixel_preview
                      ? std::max(1, static_cast<int>(std::floor(side * 0.42 / std::max(radius, 1e-6) * state_->zoom)))
                      : 1;
    const int rs = std::max(8, side / k);
    cam = camera_orbit(scene_.dims, state_->yaw, state_->pitch, rs, rs);
    cam.scale *= state_->zoom;
    cam.offset = {rs / 2.0, rs / 2.0};
    // Centre the orbit target (box centre + pan) after zooming.
    const vec3d c{scene_.dims.W / 2.0 + state_->pan.x(), scene_.dims.H / 2.0 + state_->pan.y(),
                  scene_.dims.D / 2.0 + state_->pan.z()};
    const vec3d sc = cam.project(c);
    cam.offset = {cam.offset.x + rs / 2.0 - sc.x, cam.offset.y + rs / 2.0 - sc.y};
    shown_w = shown_h = rs * k;
  }
  RenderTarget rt = pixel3d::render(mesh, cam, ro);
  if (previewing) {
    for (int y = 0; y < rt.color.height(); ++y)
      for (int x = 0; x < rt.color.width(); ++x)
        if (rt.coverage.get(x, y)) {
          Rgba8& px = rt.color.at(x, y);
          px.r = static_cast<uint8_t>(px.r + (tc.accent.red() - px.r) * 0.35);
          px.g = static_cast<uint8_t>(px.g + (tc.accent.green() - px.g) * 0.35);
          px.b = static_cast<uint8_t>(px.b + (tc.accent.blue() - px.b) * 0.35);
        }
  } else {
    for (size_t i = 0; i < rt.tri_id.size(); ++i) {
      const int t = rt.tri_id[i];
      if (t < 0 || static_cast<size_t>(t) >= scene_.triangles()) continue;
      Rgba8& px = rt.color.at(static_cast<int>(i % rt.color.width()), static_cast<int>(i / rt.color.width()));
      px = tint(px, t < static_cast<int>(scene_.tri_flags.size()) ? scene_.tri_flags[t] : 0,
                t < static_cast<int>(tri_hl_.size()) ? tri_hl_[t] : 0, *state_);
    }
  }
  const QImage img = to_qimage(rt.color);
  const QRectF target((w - shown_w) / 2.0 / dpr, (h - shown_h) / 2.0 / dpr, shown_w / dpr, shown_h / dpr);
  p.drawImage(target, img);  // nearest-neighbour: pixel preview stays crisp
  if (!previewing) tri_ids_ = std::move(rt.tri_id);
  else tri_ids_.assign(static_cast<size_t>(img.width()) * img.height(), -1);
  ids_size_ = img.size();
  ids_target_ = target;
  cam_ = cam;
  if (state_->preset) {
    if (state_->overlay && scene_.sources.contains(*state_->preset)) {
      p.setOpacity(0.45);
      p.drawImage(target, to_qimage(*scene_.sources.at(*state_->preset)));
      p.setOpacity(1);
    }
    if (state_->show_issues && !previewing && scene_.mismatch.contains(*state_->preset)) {
      p.setOpacity(0.85);
      p.drawImage(target, to_qimage(*scene_.mismatch.at(*state_->preset)));
      p.setOpacity(1);
    }
  }
  if (state_->normals && scene_.solid && !previewing) {
    const Mesh& sm = *scene_.solid;
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(theme::with_alpha(tc.info, 0.9), 1));
    for (size_t t = 0; t < sm.tris.size(); ++t) {
      const auto& tri = sm.tris[t];
      const vec3d c = (sm.positions[tri[0]] + sm.positions[tri[1]] + sm.positions[tri[2]]) / 3.0;
      p.drawLine(project(c), project(c + normalize(tri_normal_raw(sm, t)) * 0.75));
    }
  }
  // Outlines of the open issue's regions.
  auto boxes = [&](const std::vector<Region3>& rs, const QColor& col) {
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(col, 1.5));
    for (const Region3& r : rs) {
      QPointF q[8];
      for (int k = 0; k < 8; ++k)
        q[k] = project({(k & 1) ? r.hi.x() : r.lo.x(), (k & 2) ? r.hi.y() : r.lo.y(), (k & 4) ? r.hi.z() : r.lo.z()});
      const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      for (const auto& k : e) p.drawLine(q[k[0]], q[k[1]]);
    }
  };
  if (state_->show_issues) boxes(highlight_.issue_regions, tc.review);
  boxes(highlight_.focus_regions, tc.accent_hover);
}

// ---- axis indicator -------------------------------------------------------------

namespace {

// Labelled X/Y/Z indicator in a corner, following the camera.
class AxisOverlay : public QWidget {
 public:
  AxisOverlay(ViewState* state, const Scene* scene, QWidget* parent) : QWidget(parent), state_(state), scene_(scene) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
  }
  QSize sizeHint() const override { return {68, 68}; }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    const Dims d = scene_->dims.W > 0 ? scene_->dims : Dims{16, 16, 16};
    paint_axis_indicator(p, QRectF(2, 2, width() - 4, height() - 4), camera_matrices(*state_, d, 1, 1).view);
  }

 private:
  ViewState* state_;
  const Scene* scene_;
};

}  // namespace

void CpuViewport::leaveEvent(QEvent*) { emit left(); }

// ---- host -------------------------------------------------------------------

Viewport::Viewport(bool force_cpu, QWidget* parent) : QWidget(parent) {
  stack_ = new QStackedLayout(this);
  stack_->setContentsMargins(0, 0, 0, 0);
  cpu_ = new CpuViewport(&state_, this);
  cpu_->installEventFilter(this);
  stack_->addWidget(cpu_);
  connect(cpu_, &CpuViewport::left, this, [this] {
    if (last_hover_ != -1) emit triangleHovered(last_hover_ = -1);
  });
  if (!force_cpu) {
    gl_ = new GlViewport(&state_, this);
    gl_->installEventFilter(this);
    stack_->addWidget(gl_);
    stack_->setCurrentWidget(gl_);
    connect(gl_, &GlViewport::glFailed, this, &Viewport::useCpu);
    connect(gl_, &GlViewport::left, this, [this] {
      if (last_hover_ != -1) emit triangleHovered(last_hover_ = -1);
    });
  } else {
    fallback_reason_ = tr("software view requested");
  }
  spin_.setInterval(33);
  connect(&spin_, &QTimer::timeout, this, [this] {
    state_.preset.reset();
    state_.yaw = std::fmod(state_.yaw + 1.5f + 180.0f, 360.0f) - 180.0f;
    refresh();
  });
  focus_anim_ = new QVariantAnimation(this);
  focus_anim_->setDuration(theme::motion::normal);
  focus_anim_->setEasingCurve(QEasingCurve::OutCubic);
  auto* axes = new AxisOverlay(&state_, &scene_, this);
  addOverlay(axes, Qt::AlignLeft | Qt::AlignBottom, QPoint(0, theme::space::sm));
  connect(this, &Viewport::viewStateChanged, axes, qOverload<>(&QWidget::update));
}

void Viewport::useCpu(const QString& why) {
  fallback_reason_ = why;
  stack_->setCurrentWidget(cpu_);
  if (gl_) {
    gl_->hide();
    gl_->deleteLater();
    gl_ = nullptr;
  }
  cpu_->setScene(scene_);
  cpu_->setHighlight(highlight_);
  for (const Overlay& ov : overlays_) ov.w->raise();
  emit rendererChanged(rendererInfo());
}

bool Viewport::usingOpenGL() const { return gl_ != nullptr && gl_->glReady(); }

QString Viewport::rendererInfo() const {
  if (usingOpenGL()) return tr("OpenGL: %1").arg(gl_->glInfo());
  return tr("Software renderer (%1)").arg(fallback_reason_.isEmpty() ? tr("OpenGL not initialised yet") : fallback_reason_);
}

QWidget* Viewport::surface() const { return stack_->currentWidget(); }

void Viewport::setScene(const Scene& scene) {
  scene_ = scene;
  if (gl_) gl_->setScene(scene);
  cpu_->setScene(scene);
}

void Viewport::setHighlight(const Highlight& h) {
  if (h == highlight_) return;
  highlight_ = h;
  if (gl_) gl_->setHighlight(h);
  cpu_->setHighlight(h);
}

int Viewport::pickAt(QPoint pos) const {
  if (scene_.empty()) return -1;
  if (stack_->currentWidget() == cpu_) return cpu_->triangleAt(pos);
  const QWidget* w = stack_->currentWidget();
  const CameraMatrices cam = camera_matrices(state_, scene_.dims, w->width(), w->height());
  const QPointF ndc(2.0 * (pos.x() + 0.5) / std::max(1, w->width()) - 1.0, 1.0 - 2.0 * (pos.y() + 0.5) / std::max(1, w->height()));
  return pick_triangle(scene_, cam, ndc);
}

Ray Viewport::rayAt(QPointF pos) const {
  const Dims d = scene_.dims.W > 0 ? scene_.dims : Dims{16, 16, 16};
  if (stack_->currentWidget() == cpu_ && cpu_->hasFrame()) return cpu_->rayAt(pos);
  const QWidget* s = surface();
  return gl_ray_at(state_, d, s->width(), s->height(), pos);
}

QPointF Viewport::project(vec3d p) const {
  const Dims d = scene_.dims.W > 0 ? scene_.dims : Dims{16, 16, 16};
  if (stack_->currentWidget() == cpu_ && cpu_->hasFrame()) return cpu_->project(p);
  const QWidget* s = surface();
  return gl_project(state_, d, s->width(), s->height(), p);
}

std::optional<vec2d> Viewport::viewPixelAt(QPointF pos) const {
  if (!state_.preset || !scene_.sources.contains(*state_.preset) || scene_.dims.W <= 0) return std::nullopt;
  const vec2d uv = view_pixel_of_ray(*state_.preset, scene_.dims, rayAt(pos));
  const ImageSize sz = view_image_size(*state_.preset, scene_.dims);
  if (uv.x < 0 || uv.y < 0 || uv.x >= sz.w || uv.y >= sz.h) return std::nullopt;
  return uv;
}

void Viewport::refresh() {
  if (gl_) gl_->update();
  cpu_->update();
  emit viewStateChanged();
}

void Viewport::setPreset(ViewRole r) {
  setTurntable(false);
  focus_anim_->stop();
  state_.set_preset(r);
  refresh();
}

void Viewport::setIso() {
  setTurntable(false);
  focus_anim_->stop();
  state_.set_iso();
  refresh();
}

void Viewport::setTurntable(bool on) {
  if (on) spin_.start();
  else spin_.stop();
}

void Viewport::focusOn(const Region3& r, bool animate) {
  if (r.empty() || scene_.dims.W <= 0) return;
  setTurntable(false);
  QVector3D pan;
  float zoom = 1;
  frame_region(state_, scene_.dims, r, &pan, &zoom);
  const QVector3D pan0 = state_.pan;
  const float zoom0 = state_.zoom;
  focus_anim_->stop();
  focus_anim_->disconnect(this);
  if (!animate || !isVisible()) {
    state_.pan = pan;
    state_.zoom = zoom;
    refresh();
    return;
  }
  focus_anim_->setStartValue(0.0);
  focus_anim_->setEndValue(1.0);
  connect(focus_anim_, &QVariantAnimation::valueChanged, this, [this, pan0, zoom0, pan, zoom](const QVariant& v) {
    const float t = v.toFloat();
    state_.pan = pan0 + (pan - pan0) * t;
    state_.zoom = zoom0 + (zoom - zoom0) * t;
    refresh();
  });
  focus_anim_->start();
}

void Viewport::addOverlay(QWidget* w, Qt::Alignment where, QPoint offset) {
  w->setParent(this);
  w->installEventFilter(this);
  overlays_.push_back({w, where, offset});
  w->raise();
  w->show();
  placeOverlays();
}

void Viewport::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  placeOverlays();
}

void Viewport::placeOverlays() {
  const int m = theme::space::md;
  for (const Overlay& ov : overlays_) {
    QWidget* w = ov.w;
    if (w->isHidden()) continue;
    const QSize s = w->sizeHint().boundedTo(QSize(width() - 2 * m, height() - 2 * m));
    w->resize(s);
    int x = m, y = m;
    if (ov.where & Qt::AlignRight) x = width() - s.width() - m;
    if (ov.where & Qt::AlignHCenter) x = (width() - s.width()) / 2;
    if (ov.where & Qt::AlignBottom) y = height() - s.height() - m - theme::space::sm;
    w->move(x + ov.offset.x(), y + ov.offset.y());
    w->raise();
  }
}

// ---- mouse ------------------------------------------------------------------

Viewport::Drag Viewport::navFor(Qt::MouseButton b, Qt::KeyboardModifiers m) const {
  const CameraScheme s = editor_ ? editor_->cameraScheme() : CameraScheme::Pixel3D;
  switch (s) {
    case CameraScheme::Pixel3D:
      if (b == Qt::RightButton) return Drag::Pan;
      if (b == Qt::MiddleButton) return (m & Qt::ShiftModifier) ? Drag::Pan : Drag::Orbit;
      return Drag::None;
    case CameraScheme::Blender:
      if (b == Qt::MiddleButton)
        return (m & Qt::ShiftModifier) ? Drag::Pan : (m & Qt::ControlModifier) ? Drag::Zoom : Drag::Orbit;
      return Drag::None;
    case CameraScheme::Maya:
      if (!(m & Qt::AltModifier)) return Drag::None;
      if (b == Qt::LeftButton) return Drag::Orbit;
      if (b == Qt::MiddleButton) return Drag::Pan;
      if (b == Qt::RightButton) return Drag::Zoom;
      return Drag::None;
  }
  return Drag::None;
}

bool Viewport::eventFilter(QObject* o, QEvent* e) {
  if (e->type() == QEvent::LayoutRequest || e->type() == QEvent::Show || e->type() == QEvent::Resize) {
    for (const Overlay& ov : overlays_)
      if (ov.w == o) {
        QMetaObject::invokeMethod(this, &Viewport::placeOverlays, Qt::QueuedConnection);
        break;
      }
  }
  auto* w = qobject_cast<QWidget*>(o);
  if (!w || (w != gl_ && w != cpu_)) return QWidget::eventFilter(o, e);
  switch (e->type()) {
    case QEvent::MouseButtonPress:
      w->setFocus();
      mousePress(w, static_cast<QMouseEvent*>(e));
      return true;
    case QEvent::MouseMove:
      mouseMove(w, static_cast<QMouseEvent*>(e));
      return true;
    case QEvent::MouseButtonRelease:
      mouseRelease(w, static_cast<QMouseEvent*>(e));
      return true;
    case QEvent::MouseButtonDblClick:
      return true;
    case QEvent::Wheel: {
      auto* we = static_cast<QWheelEvent*>(e);
      state_.zoom = std::clamp(state_.zoom * std::pow(1.0015f, static_cast<float>(we->angleDelta().y())), 0.1f, 40.0f);
      refresh();
      return true;
    }
    case QEvent::KeyPress:
      if (static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && drag_ != Drag::None) {
        cancelDrag();
        return true;
      }
      break;
    default: break;
  }
  return QWidget::eventFilter(o, e);
}

void Viewport::mousePress(QWidget*, QMouseEvent* e) {
  press_ = last_ = e->position().toPoint();
  moved_ = false;
  if (drag_ != Drag::None) return;  // a second button during a drag is ignored
  drag_ = navFor(e->button(), e->modifiers());
  if (drag_ != Drag::None || e->button() != Qt::LeftButton) return;
  const Tool tool = editor_ ? editor_->tool() : Tool::Select;
  drag_ = Drag::Click;  // decided on the first move: a click, a stroke, a push/pull or an orbit
  stroke_.clear();
  if ((tool == Tool::Depth || tool == Tool::Parts) && viewPixelAt(e->position())) {
    stroke_role_ = *state_.preset;
    const vec2d uv = *viewPixelAt(e->position());
    stroke_ = {QPointF(uv.x, uv.y)};
  } else if (tool == Tool::PushPull && scene_.solid) {
    const Ray ray = rayAt(e->position());
    const PickHit hit = pick_ray(*scene_.solid, ray.origin, ray.dir);
    if (hit.face < 0) return;
    const auto fp = face_plane(*scene_.solid, hit.face);
    if (!fp) return;
    pp_face_ = hit.face;
    pp_last_ = 0;
    // Screen direction of one unit along the normal; a face turned to the camera uses screen up.
    const QPointF s0 = project(hit.point), s1 = project(hit.point + fp->normal);
    QPointF dir = s1 - s0;
    if (std::hypot(dir.x(), dir.y()) < 4.0) {
      vec3d side = cross(ray.dir, vec3d{0, 1, 0});
      if (length(side) < 1e-6) side = cross(ray.dir, vec3d{1, 0, 0});
      const QPointF s2 = project(hit.point + normalize(side));
      dir = QPointF(0, -std::hypot((s2 - s0).x(), (s2 - s0).y()));
    }
    pp_dir_ = dir;
  }
}

double Viewport::pushPullDistance(QPointF pos) const {
  const double len2 = pp_dir_.x() * pp_dir_.x() + pp_dir_.y() * pp_dir_.y();
  if (len2 < 1e-9) return 0;
  const QPointF d = pos - QPointF(press_);
  double dist = (d.x() * pp_dir_.x() + d.y() * pp_dir_.y()) / len2;
  if (!editor_ || editor_->snap) dist = std::round(dist);
  return dist;
}

void Viewport::navigate(Drag d, QPoint delta, QWidget* w) {
  switch (d) {
    case Drag::Orbit: state_.orbit(-delta.x() * 0.4f, delta.y() * 0.4f); break;
    case Drag::Pan: {
      const Dims dims = scene_.dims.W > 0 ? scene_.dims : Dims{16, 16, 16};
      const CameraMatrices cam = camera_matrices(state_, dims, w->width(), w->height());
      const QVector3D right(cam.view(0, 0), cam.view(0, 1), cam.view(0, 2));
      const QVector3D up(cam.view(1, 0), cam.view(1, 1), cam.view(1, 2));
      const float units = 0.5f * QVector3D(dims.W, dims.H, dims.D).length() * 2.4f / std::max(1, w->height()) / state_.zoom;
      state_.pan += (-right * delta.x() + up * delta.y()) * units;
      break;
    }
    case Drag::Zoom:
      state_.zoom = std::clamp(state_.zoom * std::pow(1.01f, static_cast<float>(-delta.y())), 0.1f, 40.0f);
      break;
    default: return;
  }
  refresh();
}

void Viewport::mouseMove(QWidget* w, QMouseEvent* e) {
  const QPoint p = e->position().toPoint(), delta = p - last_;
  last_ = p;
  if (drag_ == Drag::None) {
    // Hover: what a click would select.
    const int t = pickAt(p);
    if (t != last_hover_) emit triangleHovered(last_hover_ = t);
    return;
  }
  if ((p - press_).manhattanLength() > 3) moved_ = true;
  switch (drag_) {
    case Drag::Orbit:
    case Drag::Pan:
    case Drag::Zoom: navigate(drag_, delta, w); return;
    case Drag::Click: {
      if (!moved_) return;
      if (!stroke_.isEmpty()) {
        drag_ = Drag::Stroke;
      } else if (pp_face_ >= 0) {
        drag_ = Drag::PushPull;
        emit pushPullChanged(pp_face_, 0, false);
      } else if (!editor_ || editor_->cameraScheme() == CameraScheme::Pixel3D) {
        drag_ = Drag::Orbit;  // a left drag that is no tool gesture orbits
        navigate(drag_, p - press_, w);
        return;
      } else {
        return;
      }
      [[fallthrough]];
    }
    case Drag::Stroke:
      if (drag_ == Drag::Stroke) {
        if (const auto uv = viewPixelAt(e->position())) stroke_.push_back(QPointF(uv->x, uv->y));
        emit strokeChanged(stroke_role_, stroke_, false);
        return;
      }
      [[fallthrough]];
    case Drag::PushPull: {
      const double dist = pushPullDistance(e->position());
      if (dist != pp_last_) {
        pp_last_ = dist;
        emit pushPullChanged(pp_face_, dist, false);
      }
      return;
    }
    case Drag::None: return;
  }
}

void Viewport::mouseRelease(QWidget*, QMouseEvent* e) {
  const Drag d = drag_;
  drag_ = Drag::None;
  switch (d) {
    case Drag::Click:
      stroke_.clear();
      pp_face_ = -1;
      if (e->button() != Qt::LeftButton) return;
      emit clickedAt(e->position().toPoint());
      emit triangleClicked(pickAt(e->position().toPoint()), e->modifiers());
      return;
    case Drag::Stroke:
      if (const auto uv = viewPixelAt(e->position())) stroke_.push_back(QPointF(uv->x, uv->y));
      emit strokeChanged(stroke_role_, stroke_, true);
      stroke_.clear();
      return;
    case Drag::PushPull:
      emit pushPullChanged(pp_face_, pushPullDistance(e->position()), true);
      pp_face_ = -1;
      return;
    default: return;
  }
}

void Viewport::cancelDrag() {
  if (drag_ == Drag::Stroke) emit strokeCancelled();
  if (drag_ == Drag::PushPull) emit pushPullCancelled();
  drag_ = Drag::None;
  stroke_.clear();
  pp_face_ = -1;
}

}  // namespace p3dgui
