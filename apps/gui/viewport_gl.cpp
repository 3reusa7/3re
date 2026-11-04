#include "viewport_gl.hpp"

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLVersionFunctionsFactory>
#include <QPainter>
#include <QtMath>
#include <algorithm>
#include <cmath>

#include "pixel3d/views/view_role.hpp"
#include "theme.hpp"

namespace p3dgui {

using namespace pixel3d;

namespace {

const char* kBackgroundVs = R"(#version 330 core
out float v_t;
void main() {
  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  v_t = p.y * 0.5 + 0.5;
  gl_Position = vec4(p, 0.999, 1.0);
})";

const char* kBackgroundFs = R"(#version 330 core
in float v_t;
uniform vec3 u_top;
uniform vec3 u_bottom;
out vec4 o_color;
void main() { o_color = vec4(mix(u_bottom, u_top, clamp(v_t, 0.0, 1.0)), 1.0); })";

const char* kMeshVs = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in float a_flags;
layout(location = 4) in float a_hl;
uniform mat4 u_mvp;
out vec3 v_normal;
out vec2 v_uv;
out float v_flags;
out float v_hl;
void main() {
  v_normal = a_normal;
  v_uv = a_uv;
  v_flags = a_flags;
  v_hl = a_hl;
  gl_Position = u_mvp * vec4(a_pos, 1.0);
})";

const char* kMeshFs = R"(#version 330 core
in vec3 v_normal;
in vec2 v_uv;
in float v_flags;
in float v_hl;
uniform sampler2D u_tex;
uniform bool u_textured;
uniform bool u_lit;
uniform bool u_flat_color;
uniform bool u_show_issues;
uniform bool u_show_estimated;
uniform vec4 u_color;
uniform vec3 u_light;
uniform vec3 u_review;
uniform vec3 u_info;
uniform vec3 u_accent;
uniform vec4 u_tint;
out vec4 o_color;
void main() {
  if (u_flat_color) { o_color = u_color; return; }
  vec4 c = u_textured ? texture(u_tex, v_uv) : vec4(0.70, 0.71, 0.74, 1.0);
  float shade = 1.0;
  if (u_lit) shade = 0.62 + 0.38 * max(dot(normalize(v_normal), u_light), 0.0);
  vec3 col = mix(c.rgb, u_tint.rgb, u_tint.a) * shade;
  int f = int(v_flags + 0.5);
  if (u_show_issues && (f & 1) != 0) col = mix(col, u_review, 0.42);
  if (u_show_estimated && (f & 6) != 0) col = mix(col, u_info, 0.45);
  if (v_hl > 1.5) col = mix(col, u_accent, 0.30);
  else if (v_hl > 0.5) col = mix(col, vec3(1.0), 0.12);
  o_color = vec4(col, 1.0);
})";

const char* kLineVs = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_color;
uniform mat4 u_mvp;
uniform float u_alpha;
out vec4 v_color;
void main() {
  v_color = vec4(a_color.rgb, a_color.a * u_alpha);
  gl_Position = u_mvp * vec4(a_pos, 1.0);
})";

const char* kLineFs = R"(#version 330 core
in vec4 v_color;
out vec4 o_color;
void main() { o_color = v_color; })";

const char* kOverlayVs = R"(#version 330 core
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec2 a_uv;
uniform mat4 u_mvp;
out vec2 v_uv;
void main() {
  v_uv = a_uv;
  gl_Position = u_mvp * vec4(a_pos, 1.0);
})";

const char* kOverlayFs = R"(#version 330 core
in vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_alpha;
out vec4 o_color;
void main() {
  vec4 c = texture(u_tex, v_uv);
  o_color = vec4(c.rgb, c.a > 0.5 ? u_alpha : 0.0);
})";

QVector3D rgb(const QColor& c) { return QVector3D(c.redF(), c.greenF(), c.blueF()); }

void push_line(std::vector<float>& out, QVector3D a, QVector3D b, const QColor& c, float alpha_a, float alpha_b) {
  for (float x : {a.x(), a.y(), a.z(), c.redF(), c.greenF(), c.blueF(), alpha_a, b.x(), b.y(), b.z(), c.redF(), c.greenF(),
                  c.blueF(), alpha_b})
    out.push_back(x);
}

// Floor grid around the model: minor and major lines and two faint axis lines
// through the model centre, fading out with distance (lines are split into
// segments so the fade follows the distance, not just the end points).
std::vector<float> build_grid(Dims d) {
  std::vector<float> g;
  const theme::Colors& tc = theme::colors();
  const float cx = d.W * 0.5f, cz = d.D * 0.5f;
  const int size = std::max({d.W, d.D, 8});
  int step = 1;
  for (int s : {1, 2, 4, 5, 8, 10, 16, 20, 32, 50, 64, 100})
    if (size / s >= 6) step = s;
  const float R = size * 1.35f + step * 2;
  const int k0 = static_cast<int>(std::floor((cx - R) / step)), k1 = static_cast<int>(std::ceil((cx + R) / step));
  const int m0 = static_cast<int>(std::floor((cz - R) / step)), m1 = static_cast<int>(std::ceil((cz + R) / step));
  auto fade = [&](float x, float z) {
    const float r = std::hypot(x - cx, z - cz) / R;
    const float t = std::clamp((r - 0.45f) / 0.55f, 0.0f, 1.0f);
    return 1.0f - t * t * (3 - 2 * t);
  };
  const QColor line(255, 255, 255);
  const int kSeg = 24;
  auto add = [&](QVector3D a, QVector3D b, const QColor& c, float alpha) {
    for (int i = 0; i < kSeg; ++i) {
      const QVector3D p = a + (b - a) * (static_cast<float>(i) / kSeg), q = a + (b - a) * (static_cast<float>(i + 1) / kSeg);
      push_line(g, p, q, c, alpha * fade(p.x(), p.z()), alpha * fade(q.x(), q.z()));
    }
  };
  for (int k = k0; k <= k1; ++k) {
    const float x = static_cast<float>(k * step);
    add({x, 0, cz - R}, {x, 0, cz + R}, line, k % 4 == 0 ? 0.085f : 0.045f);
  }
  for (int m = m0; m <= m1; ++m) {
    const float z = static_cast<float>(m * step);
    add({cx - R, 0, z}, {cx + R, 0, z}, line, m % 4 == 0 ? 0.085f : 0.045f);
  }
  add({cx - R, 0.01f, cz}, {cx + R, 0.01f, cz}, tc.axis_x, 0.35f);
  add({cx, 0.01f, cz - R}, {cx, 0.01f, cz + R}, tc.axis_z, 0.35f);
  return g;
}

}  // namespace

void append_box_lines(std::vector<float>& out, const Region3& r, const QColor& c, float inflate) {
  const QVector3D lo = r.lo - QVector3D(inflate, inflate, inflate), hi = r.hi + QVector3D(inflate, inflate, inflate);
  auto P = [&](int k) { return QVector3D((k & 1) ? hi.x() : lo.x(), (k & 2) ? hi.y() : lo.y(), (k & 4) ? hi.z() : lo.z()); };
  const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (const auto& e : edges) push_line(out, P(e[0]), P(e[1]), c, 1, 1);
}

void paint_axis_indicator(QPainter& p, const QRectF& area, const QMatrix4x4& view) {
  const theme::Colors& tc = theme::colors();
  const QPointF c = area.center();
  const qreal len = area.width() * 0.34;
  struct Axis {
    QVector3D d;
    QColor col;
    const char* name;
  };
  const Axis axes[] = {{{1, 0, 0}, tc.axis_x, "X"}, {{0, 1, 0}, tc.axis_y, "Y"}, {{0, 0, 1}, tc.axis_z, "Z"}};
  struct Item {
    QPointF end;
    float depth;
    QColor col;
    QString name;
    bool positive;
  };
  std::vector<Item> items;
  for (const Axis& a : axes)
    for (int s : {1, -1}) {
      const QVector3D v = view.mapVector(a.d * static_cast<float>(s));
      items.push_back({c + QPointF(v.x() * len, -v.y() * len), v.z(), a.col, QString::fromLatin1(a.name), s > 0});
    }
  std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.depth < y.depth; });
  p.save();
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(Qt::NoPen);
  p.setBrush(theme::with_alpha(tc.panel, 0.55));
  p.drawEllipse(c, area.width() * 0.5, area.height() * 0.5);
  QFont f = theme::font(theme::Type::Small);
  f.setPointSizeF(7.5);
  f.setBold(true);
  p.setFont(f);
  for (const Item& it : items) {
    if (it.positive) {
      p.setPen(QPen(it.col, 1.6, Qt::SolidLine, Qt::RoundCap));
      p.drawLine(c, it.end);
      p.setPen(Qt::NoPen);
      p.setBrush(it.col);
      p.drawEllipse(it.end, 6.5, 6.5);
      p.setPen(tc.app);
      p.drawText(QRectF(it.end.x() - 7, it.end.y() - 7, 14, 14), Qt::AlignCenter, it.name);
    } else {
      p.setPen(QPen(theme::with_alpha(it.col, 0.7), 1.2));
      p.setBrush(theme::with_alpha(it.col, 0.18));
      p.drawEllipse(it.end, 4.0, 4.0);
    }
  }
  p.restore();
}

struct GlViewport::Gpu {
  QOpenGLShaderProgram background, mesh, lines, overlay;
  QOpenGLVertexArrayObject empty_vao, mesh_vao, lines_vao, overlay_vao, preview_vao;
  QOpenGLBuffer mesh_vbo{QOpenGLBuffer::VertexBuffer}, hl_vbo{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer lines_vbo{QOpenGLBuffer::VertexBuffer}, overlay_vbo{QOpenGLBuffer::VertexBuffer};
  QOpenGLBuffer preview_vbo{QOpenGLBuffer::VertexBuffer}, preview_hl_vbo{QOpenGLBuffer::VertexBuffer};
  GLuint atlas = 0;
  std::map<ViewRole, GLuint> sources, mismatch;
  int vertex_count = 0, preview_count = 0;
  std::unique_ptr<QOpenGLFramebufferObject> lowres;  // pixel preview target
};

GlViewport::GlViewport(ViewState* state, QWidget* parent) : QOpenGLWidget(parent), state_(state) {
  setMinimumSize(320, 240);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
}

GlViewport::~GlViewport() {
  if (!gpu_) return;
  makeCurrent();
  if (gl_) {
    if (gpu_->atlas) gl_->glDeleteTextures(1, &gpu_->atlas);
    for (auto& [r, t] : gpu_->sources) gl_->glDeleteTextures(1, &t);
    for (auto& [r, t] : gpu_->mismatch) gl_->glDeleteTextures(1, &t);
  }
  gpu_.reset();
  doneCurrent();
}

void GlViewport::setScene(const Scene& scene) {
  scene_ = scene;
  wire_.clear();
  if (scene_.wire_edges) {
    const auto& e = *scene_.wire_edges;
    wire_.reserve(e.size() / 3 * 7);
    for (size_t i = 0; i + 5 < e.size(); i += 6)
      push_line(wire_, {e[i], e[i + 1], e[i + 2]}, {e[i + 3], e[i + 4], e[i + 5]}, QColor(13, 13, 15), 1, 1);
  }
  dirty_ = true;
  highlight_dirty_ = true;
  outline_dirty_ = true;
  update();
}

void GlViewport::setHighlight(const Highlight& h) {
  if (h == highlight_) return;
  if (h.parts != highlight_.parts || h.region != highlight_.region) outline_dirty_ = true;
  if (h.preview != highlight_.preview) preview_dirty_ = true;
  highlight_ = h;
  highlight_dirty_ = true;
  update();
}

void GlViewport::initializeGL() {
  gl_ = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(context());
  if (!gl_ || !gl_->initializeOpenGLFunctions()) {
    emit glFailed(tr("OpenGL 3.3 is not available on this system"));
    return;
  }
  gpu_ = std::make_unique<Gpu>();
  auto build = [](QOpenGLShaderProgram& p, const char* vs, const char* fs) {
    return p.addShaderFromSourceCode(QOpenGLShader::Vertex, vs) && p.addShaderFromSourceCode(QOpenGLShader::Fragment, fs) &&
           p.link();
  };
  if (!build(gpu_->background, kBackgroundVs, kBackgroundFs) || !build(gpu_->mesh, kMeshVs, kMeshFs) ||
      !build(gpu_->lines, kLineVs, kLineFs) || !build(gpu_->overlay, kOverlayVs, kOverlayFs)) {
    emit glFailed(tr("cannot compile the viewport shaders: %1").arg(gpu_->mesh.log()));
    gpu_.reset();
    return;
  }
  for (auto* b : {&gpu_->mesh_vbo, &gpu_->hl_vbo, &gpu_->lines_vbo, &gpu_->overlay_vbo, &gpu_->preview_vbo,
                  &gpu_->preview_hl_vbo})
    b->create();
  gpu_->hl_vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
  gpu_->lines_vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
  for (auto* v : {&gpu_->empty_vao, &gpu_->mesh_vao, &gpu_->lines_vao, &gpu_->overlay_vao, &gpu_->preview_vao}) v->create();
  info_ = QStringLiteral("%1 — %2")
              .arg(reinterpret_cast<const char*>(gl_->glGetString(GL_VERSION)),
                   reinterpret_cast<const char*>(gl_->glGetString(GL_RENDERER)));
  ready_ = true;
}

namespace {

// smooth: painted or photographic textures (linear + trilinear mipmaps, so close-ups are not jagged and
// distant views do not shimmer); pixel art and drawings stay nearest.
GLuint make_texture(QOpenGLFunctions_3_3_Core* gl, const ImageRGBA& img, bool smooth = false) {
  GLuint t = 0;
  gl->glGenTextures(1, &t);
  gl->glBindTexture(GL_TEXTURE_2D, t);
  gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  // Row 0 of the image is texture row t = 0, matching glTF UVs (v down).
  gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   img.pixels().data());
  gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
  gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
  gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (smooth) {
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);
    gl->glGenerateMipmap(GL_TEXTURE_2D);
  } else {
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  }
  return t;
}

}  // namespace

void GlViewport::upload() {
  if (!dirty_ || !gpu_) return;
  dirty_ = false;
  if (gpu_->atlas) gl_->glDeleteTextures(1, &gpu_->atlas);
  gpu_->atlas = 0;
  for (auto* texs : {&gpu_->sources, &gpu_->mismatch}) {
    for (auto& [r, t] : *texs) gl_->glDeleteTextures(1, &t);
    texs->clear();
  }
  gpu_->vertex_count = 0;
  if (scene_.empty()) return;
  // Three vertices per triangle, so per-triangle facts (flags, highlight) are exact.
  const TexturedMesh& m = *scene_.mesh;
  const size_t T = scene_.triangles();
  std::vector<float> v;
  v.reserve(T * 3 * 9);
  for (size_t t = 0; t < T; ++t)
    for (int k = 0; k < 3; ++k) {
      const uint32_t i = m.indices[3 * t + k];
      const vec3d p = m.positions[i], n = i < m.normals.size() ? m.normals[i] : vec3d{0, 1, 0};
      const vec2d uv = i < m.uvs.size() ? m.uvs[i] : vec2d{0, 0};
      const double flags = t < scene_.tri_flags.size() ? scene_.tri_flags[t] : 0;
      for (double x : {p.x, p.y, p.z, n.x, n.y, n.z, uv.x, uv.y, flags}) v.push_back(static_cast<float>(x));
    }
  gpu_->mesh_vao.bind();
  gpu_->mesh_vbo.bind();
  gpu_->mesh_vbo.allocate(v.data(), static_cast<int>(v.size() * sizeof(float)));
  const int stride = 9 * sizeof(float);
  gl_->glEnableVertexAttribArray(0);
  gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
  gl_->glEnableVertexAttribArray(1);
  gl_->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
  gl_->glEnableVertexAttribArray(2);
  gl_->glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
  gl_->glEnableVertexAttribArray(3);
  gl_->glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(8 * sizeof(float)));
  gpu_->hl_vbo.bind();
  std::vector<float> zeros(T * 3, 0.0f);
  gpu_->hl_vbo.allocate(zeros.data(), static_cast<int>(zeros.size() * sizeof(float)));
  gl_->glEnableVertexAttribArray(4);
  gl_->glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
  gpu_->mesh_vao.release();
  gpu_->vertex_count = static_cast<int>(T * 3);
  if (scene_.textured && m.texture) gpu_->atlas = make_texture(gl_, *m.texture, scene_.smooth_texture);
  for (const auto& [role, img] : scene_.sources) gpu_->sources[role] = make_texture(gl_, *img);
  for (const auto& [role, img] : scene_.mismatch) gpu_->mismatch[role] = make_texture(gl_, *img);
}

void GlViewport::uploadPreview() {
  if (!preview_dirty_ || !gpu_) return;
  preview_dirty_ = false;
  gpu_->preview_count = 0;
  if (!highlight_.preview || highlight_.preview->indices.empty()) return;
  const TexturedMesh& m = *highlight_.preview;
  std::vector<float> v;
  v.reserve(m.indices.size() * 9);
  for (uint32_t i : m.indices) {
    const vec3d p = m.positions[i], n = i < m.normals.size() ? m.normals[i] : vec3d{0, 1, 0};
    for (double x : {p.x, p.y, p.z, n.x, n.y, n.z, 0.0, 0.0, 0.0}) v.push_back(static_cast<float>(x));
  }
  gpu_->preview_vao.bind();
  gpu_->preview_vbo.bind();
  gpu_->preview_vbo.allocate(v.data(), static_cast<int>(v.size() * sizeof(float)));
  const int stride = 9 * sizeof(float);
  gl_->glEnableVertexAttribArray(0);
  gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
  gl_->glEnableVertexAttribArray(1);
  gl_->glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(3 * sizeof(float)));
  gl_->glEnableVertexAttribArray(2);
  gl_->glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(6 * sizeof(float)));
  gl_->glEnableVertexAttribArray(3);
  gl_->glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(8 * sizeof(float)));
  gpu_->preview_hl_vbo.bind();
  std::vector<float> zeros(m.indices.size(), 0.0f);
  gpu_->preview_hl_vbo.allocate(zeros.data(), static_cast<int>(zeros.size() * sizeof(float)));
  gl_->glEnableVertexAttribArray(4);
  gl_->glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
  gpu_->preview_vao.release();
  gpu_->preview_count = static_cast<int>(m.indices.size());
}

void GlViewport::drawImageQuad(unsigned tex, ViewRole r, Dims d, const QMatrix4x4& mvp, float alpha) {
  const ImageSize sz = view_image_size(r, d);
  std::vector<float> q;
  const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (int i : {0, 1, 2, 0, 2, 3}) {
    const vec3d p = world_from_view(r, d, uvs[i][0] * sz.w, uvs[i][1] * sz.h, -0.05);
    for (float x : {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), uvs[i][0], uvs[i][1]})
      q.push_back(x);
  }
  gl_->glDisable(GL_DEPTH_TEST);
  gpu_->overlay_vao.bind();
  gpu_->overlay_vbo.bind();
  gpu_->overlay_vbo.allocate(q.data(), static_cast<int>(q.size() * sizeof(float)));
  gl_->glEnableVertexAttribArray(0);
  gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
  gl_->glEnableVertexAttribArray(1);
  gl_->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
  gpu_->overlay.bind();
  gpu_->overlay.setUniformValue("u_mvp", mvp);
  gpu_->overlay.setUniformValue("u_alpha", alpha);
  gpu_->overlay.setUniformValue("u_tex", 0);
  gl_->glActiveTexture(GL_TEXTURE0);
  gl_->glBindTexture(GL_TEXTURE_2D, tex);
  gl_->glDrawArrays(GL_TRIANGLES, 0, 6);
  gpu_->overlay_vao.release();
  gl_->glEnable(GL_DEPTH_TEST);
}

void GlViewport::uploadHighlight() {
  if (!highlight_dirty_ || !gpu_) return;
  highlight_dirty_ = false;
  const theme::Colors& tc = theme::colors();
  const size_t T = scene_.triangles();
  std::vector<float> hl(T * 3, 0.0f);
  std::vector<bool> selected(T, false);
  bool any = false;
  for (size_t t = 0; t < T; ++t) {
    const int part = t < scene_.tri_part.size() ? scene_.tri_part[t] : -1;
    const int region = t < scene_.tri_region.size() ? scene_.tri_region[t] : -1;
    const bool sel = highlight_.region >= 0 ? region == highlight_.region : highlight_.parts.contains(part);
    const bool hov = !sel && (highlight_.hover_region >= 0 ? region == highlight_.hover_region
                                                           : highlight_.hover_part >= 0 && part == highlight_.hover_part);
    selected[t] = sel;
    any |= sel;
    const float value = sel ? 2.0f : hov ? 1.0f : 0.0f;
    hl[3 * t] = hl[3 * t + 1] = hl[3 * t + 2] = value;
  }
  if (gpu_->vertex_count > 0) {
    gpu_->hl_vbo.bind();
    gpu_->hl_vbo.write(0, hl.data(), static_cast<int>(hl.size() * sizeof(float)));
    gpu_->hl_vbo.release();
  }
  if (outline_dirty_) {
    outline_dirty_ = false;
    outline_.clear();
    if (any) {
      const std::vector<float> e = outline_edges(scene_, selected);
      for (size_t i = 0; i + 5 < e.size(); i += 6)
        push_line(outline_, {e[i], e[i + 1], e[i + 2]}, {e[i + 3], e[i + 4], e[i + 5]}, tc.accent_hover, 1, 1);
    }
  }
  issue_boxes_.clear();
  for (const Region3& r : highlight_.issue_regions) append_box_lines(issue_boxes_, r, tc.review);
  focus_boxes_.clear();
  for (const Region3& r : highlight_.focus_regions) append_box_lines(focus_boxes_, r, tc.accent_hover, 0.3f);
}

void GlViewport::drawLines(const std::vector<float>& v, const QMatrix4x4& mvp, bool depth_test, bool hidden_hint) {
  if (v.empty()) return;
  gpu_->lines_vao.bind();
  gpu_->lines_vbo.bind();
  gpu_->lines_vbo.allocate(v.data(), static_cast<int>(v.size() * sizeof(float)));
  gl_->glEnableVertexAttribArray(0);
  gl_->glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr);
  gl_->glEnableVertexAttribArray(1);
  gl_->glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));
  gpu_->lines.bind();
  gpu_->lines.setUniformValue("u_mvp", mvp);
  auto draw = [&](float alpha) {
    gpu_->lines.setUniformValue("u_alpha", alpha);
    gl_->glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(v.size() / 7));
  };
  if (depth_test) {
    // Hidden parts faintly (see-through hint), visible parts fully.
    if (hidden_hint) {
      gl_->glDisable(GL_DEPTH_TEST);
      draw(0.22f);
      gl_->glEnable(GL_DEPTH_TEST);
    }
    gl_->glDepthFunc(GL_LEQUAL);
    draw(1.0f);
    gl_->glDepthFunc(GL_LESS);
  } else {
    draw(1.0f);
  }
  gpu_->lines_vao.release();
}

void GlViewport::drawScene(const QMatrix4x4& mvp, Dims d) {
  const theme::Colors& tc = theme::colors();
  // Background: a soft vertical gradient.
  gl_->glDisable(GL_DEPTH_TEST);
  gpu_->background.bind();
  gpu_->background.setUniformValue("u_top", rgb(tc.viewport_top));
  gpu_->background.setUniformValue("u_bottom", rgb(tc.viewport_bottom));
  gpu_->empty_vao.bind();
  gl_->glDrawArrays(GL_TRIANGLES, 0, 3);
  gpu_->empty_vao.release();

  gl_->glEnable(GL_BLEND);
  gl_->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  gl_->glEnable(GL_DEPTH_TEST);

  if (state_->grid) {
    if (!(grid_dims_ == d)) {
      grid_ = build_grid(d);
      grid_dims_ = d;
    }
    gl_->glDepthMask(GL_FALSE);
    drawLines(grid_, mvp, false);
    gl_->glDepthMask(GL_TRUE);
  }

  const bool previewing = highlight_.preview && gpu_->preview_count > 0;
  const int count = previewing ? gpu_->preview_count : gpu_->vertex_count;
  if (count > 0) {
    gl_->glEnable(GL_CULL_FACE);
    gl_->glCullFace(GL_BACK);
    gl_->glEnable(GL_POLYGON_OFFSET_FILL);
    gl_->glPolygonOffset(1.0f, 1.0f);  // outlines on the edges win the depth test
    gpu_->mesh.bind();
    gpu_->mesh.setUniformValue("u_mvp", mvp);
    gpu_->mesh.setUniformValue("u_textured", !previewing && gpu_->atlas != 0 && state_->textured);
    gpu_->mesh.setUniformValue("u_lit", state_->lit);
    gpu_->mesh.setUniformValue("u_flat_color", false);
    gpu_->mesh.setUniformValue("u_show_issues", state_->show_issues && !previewing);
    gpu_->mesh.setUniformValue("u_show_estimated", state_->show_estimated && !previewing);
    gpu_->mesh.setUniformValue("u_light", QVector3D(-0.4f, 0.8f, 0.45f).normalized());
    gpu_->mesh.setUniformValue("u_review", rgb(tc.review));
    gpu_->mesh.setUniformValue("u_info", rgb(tc.info));
    gpu_->mesh.setUniformValue("u_accent", rgb(tc.accent));
    const QVector3D accent = rgb(tc.accent);
    gpu_->mesh.setUniformValue("u_tint", previewing ? QVector4D(accent, 0.35f) : QVector4D(0, 0, 0, 0));
    gpu_->mesh.setUniformValue("u_tex", 0);
    gl_->glActiveTexture(GL_TEXTURE0);
    gl_->glBindTexture(GL_TEXTURE_2D, gpu_->atlas);
    QOpenGLVertexArrayObject& vao = previewing ? gpu_->preview_vao : gpu_->mesh_vao;
    vao.bind();
    gl_->glDrawArrays(GL_TRIANGLES, 0, count);
    gl_->glDisable(GL_POLYGON_OFFSET_FILL);
    if (state_->wireframe && (wire_.empty() || previewing)) {
      gl_->glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
      gpu_->mesh.setUniformValue("u_flat_color", true);
      gpu_->mesh.setUniformValue("u_color", QVector4D(0.05f, 0.05f, 0.06f, 1.0f));
      gl_->glDepthFunc(GL_LEQUAL);
      gl_->glDrawArrays(GL_TRIANGLES, 0, count);
      gl_->glDepthFunc(GL_LESS);
      gl_->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    vao.release();
    gl_->glDisable(GL_CULL_FACE);
    if (state_->wireframe && !wire_.empty() && !previewing) drawLines(wire_, mvp, true, false);
  }

  if (!previewing) {
    drawLines(outline_, mvp, true);
    if (state_->show_issues) drawLines(issue_boxes_, mvp, true);
    drawLines(focus_boxes_, mvp, true);
  }
  if (state_->normals && scene_.solid && !previewing) {
    const Mesh& sm = *scene_.solid;
    std::vector<float> lines;
    lines.reserve(sm.tris.size() * 14);
    for (size_t t = 0; t < sm.tris.size(); ++t) {
      const auto& tri = sm.tris[t];
      const vec3d c = (sm.positions[tri[0]] + sm.positions[tri[1]] + sm.positions[tri[2]]) / 3.0;
      const vec3d e = c + normalize(tri_normal_raw(sm, t)) * 0.75;
      push_line(lines, QVector3D(c.x, c.y, c.z), QVector3D(e.x, e.y, e.z), tc.info, 1, 1);
    }
    drawLines(lines, mvp, true);
  }

  // The source drawing over its own view (mismatches show as misaligned art),
  // and with the issues overlay the pixels where model and drawing disagree.
  if (state_->preset) {
    const ViewRole r = *state_->preset;
    if (state_->overlay && gpu_->sources.contains(r)) drawImageQuad(gpu_->sources[r], r, d, mvp, 0.45f);
    if (state_->show_issues && !previewing && gpu_->mismatch.contains(r)) drawImageQuad(gpu_->mismatch[r], r, d, mvp, 0.85f);
  }
  gl_->glDisable(GL_BLEND);
  gl_->glDisable(GL_DEPTH_TEST);
}

void GlViewport::paintGL() {
  if (!gl_ || !gpu_) return;
  upload();
  uploadHighlight();
  uploadPreview();
  const theme::Colors& tc = theme::colors();
  const qreal dpr = devicePixelRatioF();
  const int w = static_cast<int>(width() * dpr), h = static_cast<int>(height() * dpr);
  const Dims d = scene_.dims.W > 0 ? scene_.dims : Dims{16, 16, 16};
  const CameraMatrices cam = camera_matrices(*state_, d, w, h);
  const QMatrix4x4 mvp = cam.proj * cam.view;
  if (state_->pixel_preview && !scene_.empty()) {
    // About one rendered pixel per source pixel, then enlarged without smoothing.
    const float radius = 0.5f * QVector3D(d.W, d.H, d.D).length();
    const float half = state_->ortho ? radius * 1.12f / std::max(state_->zoom, 0.01f)
                                     : radius * 3.2f / std::max(state_->zoom, 0.01f) * std::tan(qDegreesToRadians(18.0f));
    const int k = std::max(1, static_cast<int>(std::floor(h / (2.0f * half))));
    const QSize low(std::max(1, w / k), std::max(1, h / k));
    if (!gpu_->lowres || gpu_->lowres->size() != low)
      gpu_->lowres = std::make_unique<QOpenGLFramebufferObject>(low, QOpenGLFramebufferObject::Depth);
    gpu_->lowres->bind();
    gl_->glViewport(0, 0, low.width(), low.height());
    gl_->glClearColor(tc.viewport_bottom.redF(), tc.viewport_bottom.greenF(), tc.viewport_bottom.blueF(), 1.0f);
    gl_->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawScene(mvp, d);
    gl_->glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu_->lowres->handle());
    gl_->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, defaultFramebufferObject());
    gl_->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl_->glBlitFramebuffer(0, 0, low.width(), low.height(), 0, 0, low.width() * k, low.height() * k, GL_COLOR_BUFFER_BIT,
                           GL_NEAREST);
    gl_->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
    gl_->glViewport(0, 0, w, h);
    return;
  }
  gl_->glViewport(0, 0, w, h);
  gl_->glClearColor(tc.viewport_bottom.redF(), tc.viewport_bottom.greenF(), tc.viewport_bottom.blueF(), 1.0f);
  gl_->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  drawScene(mvp, d);
}

void GlViewport::leaveEvent(QEvent*) { emit left(); }

}  // namespace p3dgui
