#include "pixel3d/engine/lowpoly_stage.hpp"
#include "pixel3d/lowpoly/rig.hpp"
#include "view_state.hpp"

#include <QtMath>
#include <algorithm>
#include <cmath>
#include <set>
#include <unordered_map>

#include "pixel3d/image/mask.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace p3dgui {

using namespace pixel3d;

std::shared_ptr<const TexturedMesh> flat_mesh(const Mesh& src) {
  auto m = std::make_shared<TexturedMesh>();
  m->positions.reserve(src.tris.size() * 3);
  for (size_t t = 0; t < src.tris.size(); ++t) {
    const vec3d n = normalize(tri_normal_raw(src, t));
    for (uint32_t i : src.tris[t]) {
      m->indices.push_back(static_cast<uint32_t>(m->positions.size()));
      m->positions.push_back(src.positions[i]);
      m->normals.push_back(n);
      m->uvs.push_back({0, 0});
    }
    m->tri_part.push_back(t < src.tri_part.size() ? src.tri_part[t] : -1);
  }
  return m;
}

namespace {

// Red = the model covers a pixel the drawing leaves empty; blue = the reverse.
std::shared_ptr<const ImageRGBA> mismatch_image(const Mesh& solid, const SourceView& v, Dims d, uint8_t alpha) {
  const Mask2D drawn = opaque_mask(*v.aligned, alpha);
  const RenderTarget rt = render_mesh_flat(solid, camera_for_view(v.role, d, 1));
  struct Rgb {
    uint8_t r, g, b;
  };
  const Rgb extra{235, 87, 79}, missing{61, 160, 230};  // the theme's fail and a clear blue
  auto img = std::make_shared<ImageRGBA>(drawn.width(), drawn.height());
  for (int y = 0; y < drawn.height(); ++y)
    for (int x = 0; x < drawn.width(); ++x) {
      const bool a = drawn.get(x, y), b = rt.coverage.get_or_false(x, y);
      if (a && !b) img->at(x, y) = Rgba8{missing.r, missing.g, missing.b, 255};
      else if (b && !a) img->at(x, y) = Rgba8{extra.r, extra.g, extra.b, 255};
    }
  return img;
}

}  // namespace

Scene scene_from(const Document& d, bool lowpoly_pose) {
  Scene s;
  s.revision = d.revision;
  if (d.align) s.dims = d.align->dims;
  for (const SourceView& v : d.views)
    if (v.aligned) s.sources[v.role] = v.aligned;
  if (d.settings.lowpoly.enabled) {
    // LOW-POLY STUDIO: the character (rest pose or test pose), textured with its baked atlas.
    const LowpolyBuild* b = current_lowpoly(d);
    if (!b) return s;
    const lowpoly::SkinnedMesh& m = b->result.mesh();
    auto tm = std::make_shared<TexturedMesh>();
    tm->positions = m.positions;
    tm->normals = m.normals;
    if (lowpoly_pose) {
      const lowpoly::PoseResult pr =
          lowpoly::apply_pose(m, b->result.layout.skeleton, lowpoly::test_pose(b->result.layout));
      tm->positions = pr.positions;
      tm->normals = pr.normals;
    }
    tm->uvs = m.uvs;
    tm->indices = m.indices;
    tm->texture = b->result.bake.texture;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
      const bool hull = m.hull_appended && t >= m.hull_first_index;
      tm->tri_part.push_back(hull ? -1 : m.vertex_part[m.indices[t]]);
    }
    s.mesh = std::move(tm);
    s.textured = true;
    s.lowpoly = true;
    const size_t T = s.triangles();
    s.tri_part = s.mesh->tri_part;
    s.tri_region.assign(T, -1);
    s.tri_flags.assign(T, 0);
    return s;
  }
  if (d.styled) {
    // The polished model (built at validation) is what is checked and exported: show it.
    s.mesh = std::shared_ptr<const TexturedMesh>(d.styled, &d.styled->texture.mesh);
    s.textured = true;
    s.styled = true;
  } else if (d.texture) {
    s.mesh = std::shared_ptr<const TexturedMesh>(d.texture, &d.texture->mesh);  // aliasing: shares ownership
    s.textured = true;
  } else if (d.solid) {
    // Untextured preview: one vertex per triangle corner with the face normal.
    auto m = std::make_shared<TexturedMesh>();
    const Mesh& src = d.solid->mesh;
    for (size_t t = 0; t < src.tris.size(); ++t) {
      const vec3d n = normalize(tri_normal_raw(src, t));
      for (uint32_t i : src.tris[t]) {
        m->indices.push_back(static_cast<uint32_t>(m->positions.size()));
        m->positions.push_back(src.positions[i]);
        m->normals.push_back(n);
        m->uvs.push_back({0, 0});
      }
      m->tri_part.push_back(t < src.tri_part.size() ? src.tri_part[t] : -1);
    }
    s.mesh = std::move(m);
  }
  if (!s.mesh) return s;
  const size_t T = s.triangles();
  s.tri_part.assign(T, -1);
  for (size_t t = 0; t < T && t < s.mesh->tri_part.size(); ++t) s.tri_part[t] = s.mesh->tri_part[t];
  s.tri_region.assign(T, -1);
  if (!s.styled && d.solid && d.solid->mesh.tri_region.size() == T) s.tri_region = d.solid->mesh.tri_region;
  std::map<int, CandStatus> part_status;
  if (d.correspondence)
    for (const Part3D& p : d.correspondence->parts) part_status[p.id] = p.status;
  s.tri_flags.assign(T, 0);
  for (size_t t = 0; t < T; ++t) {
    const auto it = part_status.find(s.tri_part[t]);
    if (it != part_status.end()) {
      if (it->second == CandStatus::Ambiguous) s.tri_flags[t] |= kTriUndecided;
      if (it->second == CandStatus::InferredGrid || it->second == CandStatus::InferredContinuation ||
          it->second == CandStatus::InferredFill)
        s.tri_flags[t] |= kTriEstimatedPart;
    }
    const TextureResult* tex = d.styled ? &d.styled->texture : d.texture.get();
    if (tex && t < tex->faces.size() && tex->faces[t].inferred) s.tri_flags[t] |= kTriEstimatedSurface;
  }
  // M4: parts whose masks the geometry does not cover need a look too; one drawing estimates everything.
  std::set<int> mask_conflict_parts;
  if (d.mask_conflicts)
    for (const MaskConflict& c : *d.mask_conflicts) mask_conflict_parts.insert(c.part);
  s.single_view = d.single_view.enabled;
  for (size_t t = 0; t < T; ++t) {
    if (mask_conflict_parts.contains(s.tri_part[t])) s.tri_flags[t] |= kTriUndecided;
    if (s.single_view) s.tri_flags[t] |= kTriEstimatedPart;
  }
  if (d.solid) {
    s.solid = std::shared_ptr<const Mesh>(d.solid, &d.solid->mesh);
    if (d.align)
      for (const SourceView& v : d.views)
        if (v.aligned) s.mismatch[v.role] = mismatch_image(d.solid->mesh, v, d.align->dims, d.settings.alpha_threshold);
  }
  return s;
}

void ViewState::set_preset(ViewRole r) {
  preset = r;
  ortho = true;
  zoom = 1;
  pan = {};
  pitch = 0;
  switch (r) {
    case ViewRole::Front: yaw = 0; break;
    case ViewRole::Back: yaw = 180; break;
    case ViewRole::Side: yaw = -90; break;
    case ViewRole::Right: yaw = 90; break;
    case ViewRole::Top: yaw = 0, pitch = 90; break;
    case ViewRole::Bottom: yaw = 0, pitch = -90; break;
  }
}

void ViewState::set_iso() {
  preset.reset();
  ortho = false;
  yaw = 35;
  pitch = 22;
  zoom = 1;
  pan = {};
}

void ViewState::orbit(float dyaw, float dpitch) {
  preset.reset();
  yaw = std::fmod(yaw + dyaw + 540.0f, 360.0f) - 180.0f;
  pitch = std::clamp(pitch + dpitch, -90.0f, 90.0f);
}

CameraMatrices camera_matrices(const ViewState& v, Dims d, int width, int height) {
  CameraMatrices c;
  const QVector3D size(std::max(d.W, 1), std::max(d.H, 1), std::max(d.D, 1));
  const float radius = 0.5f * size.length();
  c.target = size * 0.5f + v.pan;
  const float y = qDegreesToRadians(v.yaw), p = qDegreesToRadians(v.pitch);
  const QVector3D dir(std::sin(y) * std::cos(p), std::sin(p), std::cos(y) * std::cos(p));
  const QVector3D up(-std::sin(y) * std::sin(p), std::cos(p), -std::cos(y) * std::sin(p));
  const float aspect = static_cast<float>(std::max(width, 1)) / static_cast<float>(std::max(height, 1));
  const float dist = radius * 3.2f / std::max(v.zoom, 0.01f);
  c.eye = c.target + dir * (v.ortho ? radius * 4 : dist);
  c.view.lookAt(c.eye, c.target, up);
  if (v.ortho) {
    const float half = radius * 1.12f / std::max(v.zoom, 0.01f);
    c.proj.ortho(-half * aspect, half * aspect, -half, half, 0.01f, radius * 10);
  } else {
    c.proj.perspective(36.0f, aspect, std::max(0.05f, dist - radius * 2), dist + radius * 4);
  }
  return c;
}

Ray gl_ray_at(const ViewState& v, Dims d, int width, int height, QPointF pos) {
  const CameraMatrices cam = camera_matrices(v, d, width, height);
  const QMatrix4x4 inv = (cam.proj * cam.view).inverted();
  const float x = static_cast<float>(2.0 * pos.x() / std::max(1, width) - 1.0);
  const float y = static_cast<float>(1.0 - 2.0 * pos.y() / std::max(1, height));
  const QVector3D a = inv.map(QVector3D(x, y, -1)), b = inv.map(QVector3D(x, y, 1));
  const QVector3D dir = (b - a).normalized();
  return Ray{{a.x(), a.y(), a.z()}, {dir.x(), dir.y(), dir.z()}};
}

QPointF gl_project(const ViewState& v, Dims d, int width, int height, vec3d p) {
  const CameraMatrices cam = camera_matrices(v, d, width, height);
  const QVector3D n =
      (cam.proj * cam.view).map(QVector3D(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)));
  return {(n.x() + 1.0) * 0.5 * width, (1.0 - n.y()) * 0.5 * height};
}

vec2d view_pixel_of_ray(ViewRole r, Dims d, const Ray& ray) {
  const vec3d uvd = view_from_world(r, d, ray.origin);
  return {uvd.x, uvd.y};
}

void frame_region(const ViewState&, Dims d, const Region3& r, QVector3D* pan, float* zoom) {
  const QVector3D size(std::max(d.W, 1), std::max(d.H, 1), std::max(d.D, 1));
  const float model = 0.5f * size.length();
  const float region = std::max(0.5f * (r.hi - r.lo).length(), 2.0f);
  *pan = r.center() - size * 0.5f;
  *zoom = std::clamp(model / region * 0.45f, 1.0f, 4.0f);
}

int pick_triangle(const Scene& s, const CameraMatrices& cam, QPointF ndc) {
  if (s.empty()) return -1;
  bool ok = false;
  const QMatrix4x4 inv = (cam.proj * cam.view).inverted(&ok);
  if (!ok) return -1;
  const QVector3D a = inv.map(QVector3D(static_cast<float>(ndc.x()), static_cast<float>(ndc.y()), -1.0f));
  const QVector3D b = inv.map(QVector3D(static_cast<float>(ndc.x()), static_cast<float>(ndc.y()), 1.0f));
  return ray_cast(s, a, b - a);
}

int ray_cast(const Scene& s, QVector3D a, QVector3D dir, float* t_out) {
  if (s.empty()) return -1;
  const TexturedMesh& m = *s.mesh;
  int best = -1;
  float best_t = 1e30f;
  auto P = [&](uint32_t i) {
    const vec3d& p = m.positions[i];
    return QVector3D(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
  };
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const QVector3D v0 = P(m.indices[t]), v1 = P(m.indices[t + 1]), v2 = P(m.indices[t + 2]);
    // Möller–Trumbore, both sides.
    const QVector3D e1 = v1 - v0, e2 = v2 - v0;
    const QVector3D pv = QVector3D::crossProduct(dir, e2);
    const float det = QVector3D::dotProduct(e1, pv);
    if (std::fabs(det) < 1e-12f) continue;
    const float inv_det = 1.0f / det;
    const QVector3D tv = a - v0;
    const float u = QVector3D::dotProduct(tv, pv) * inv_det;
    if (u < 0 || u > 1) continue;
    const QVector3D qv = QVector3D::crossProduct(tv, e1);
    const float v = QVector3D::dotProduct(dir, qv) * inv_det;
    if (v < 0 || u + v > 1) continue;
    const float hit = QVector3D::dotProduct(e2, qv) * inv_det;
    if (hit >= 0 && hit < best_t) {
      best_t = hit;
      best = static_cast<int>(t / 3);
    }
  }
  if (t_out) *t_out = best_t;
  return best;
}

namespace {

struct EdgeKey {
  int64_t a, b;
  bool operator==(const EdgeKey&) const = default;
};
struct EdgeHash {
  // Unsigned arithmetic: the multiply wraps by definition (a signed overflow would be undefined).
  size_t operator()(const EdgeKey& k) const {
    return std::hash<uint64_t>()(static_cast<uint64_t>(k.a) * 1000003u ^ static_cast<uint64_t>(k.b));
  }
};

int64_t vkey(const vec3d& p) {
  const auto q = [](double x) { return static_cast<int64_t>(std::llround(x * 64.0)) & 0x1fffff; };
  return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
}

}  // namespace

std::vector<float> outline_edges(const Scene& s, const std::vector<bool>& member) {
  std::vector<float> out;
  if (s.empty()) return out;
  const TexturedMesh& m = *s.mesh;
  struct Use {
    int count = 0;
    vec3d n0, n1;
    vec3d p0, p1;
  };
  std::unordered_map<EdgeKey, Use, EdgeHash> edges;
  const size_t T = s.triangles();
  for (size_t t = 0; t < T && t < member.size(); ++t) {
    if (!member[t]) continue;
    const vec3d p[3] = {m.positions[m.indices[3 * t]], m.positions[m.indices[3 * t + 1]], m.positions[m.indices[3 * t + 2]]};
    const vec3d n = normalize(cross(p[1] - p[0], p[2] - p[0]));
    for (int k = 0; k < 3; ++k) {
      const vec3d &u = p[k], &w = p[(k + 1) % 3];
      int64_t ka = vkey(u), kb = vkey(w);
      if (ka > kb) std::swap(ka, kb);
      Use& e = edges[EdgeKey{ka, kb}];
      if (e.count == 0) e.n0 = n, e.p0 = u, e.p1 = w;
      else e.n1 = n;
      ++e.count;
    }
  }
  for (const auto& [k, e] : edges) {
    const bool border = e.count == 1;
    const bool crease = e.count == 2 && dot(e.n0, e.n1) < 0.94;
    if (!border && !crease) continue;
    for (const vec3d& p : {e.p0, e.p1})
      for (double x : {p.x, p.y, p.z}) out.push_back(static_cast<float>(x));
  }
  return out;
}

Region3 bounds(const std::vector<Region3>& rs) {
  Region3 b;
  bool first = true;
  for (const Region3& r : rs) {
    if (first) b = r;
    else {
      b.lo = QVector3D(std::min(b.lo.x(), r.lo.x()), std::min(b.lo.y(), r.lo.y()), std::min(b.lo.z(), r.lo.z()));
      b.hi = QVector3D(std::max(b.hi.x(), r.hi.x()), std::max(b.hi.y(), r.hi.y()), std::max(b.hi.z(), r.hi.z()));
    }
    first = false;
  }
  return b;
}

Region3 triangles_bounds(const Scene& s, const std::vector<bool>& member) {
  std::vector<Region3> rs;
  if (s.empty()) return {};
  const TexturedMesh& m = *s.mesh;
  for (size_t t = 0; t < s.triangles() && t < member.size(); ++t) {
    if (!member[t]) continue;
    for (int k = 0; k < 3; ++k) {
      const vec3d& p = m.positions[m.indices[3 * t + k]];
      const QVector3D q(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
      rs.push_back({q, q});
    }
  }
  return bounds(rs);
}

}  // namespace p3dgui
