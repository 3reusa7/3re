#include "pixel3d/edit/selection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

using nlohmann::json;

std::string_view to_string(SelectMode m) {
  switch (m) {
    case SelectMode::Object: return "object";
    case SelectMode::Part: return "part";
    case SelectMode::Face: return "face";
  }
  return "part";
}

std::optional<SelectMode> select_mode_from_string(std::string_view s) {
  for (SelectMode m : {SelectMode::Object, SelectMode::Part, SelectMode::Face})
    if (to_string(m) == s) return m;
  return std::nullopt;
}

PickHit pick_ray(const Mesh& m, vec3d o, vec3d dir) {
  PickHit best;
  double best_t = std::numeric_limits<double>::infinity();
  const vec3d d = normalize(dir);
  for (size_t t = 0; t < m.tris.size(); ++t) {
    const vec3d a = m.positions[m.tris[t][0]], b = m.positions[m.tris[t][1]], c = m.positions[m.tris[t][2]];
    // Möller–Trumbore.
    const vec3d e1 = b - a, e2 = c - a, p = cross(d, e2);
    const double det = dot(e1, p);
    if (std::abs(det) < 1e-12) continue;
    const double inv = 1.0 / det;
    const vec3d s = o - a;
    const double uu = dot(s, p) * inv;
    if (uu < -1e-9 || uu > 1 + 1e-9) continue;
    const vec3d q = cross(s, e1);
    const double vv = dot(d, q) * inv;
    if (vv < -1e-9 || uu + vv > 1 + 1e-9) continue;
    const double dist = dot(e2, q) * inv;
    if (dist <= 1e-9 || dist >= best_t) continue;
    best_t = dist;
    best.tri = static_cast<int>(t);
    best.part = t < m.tri_part.size() ? m.tri_part[t] : -1;
    best.face = t < m.tri_region.size() ? m.tri_region[t] : -1;
    best.point = o + d * dist;
    best.distance = dist;
  }
  return best;
}

PickHit pick_view_pixel(const Mesh& m, ViewRole r, Dims d, int u, int v) {
  constexpr double kBehind = 1e4;  // edits may stand out of the drawings' box
  return pick_ray(m, world_from_view(r, d, u + 0.5, v + 0.5, -kBehind), view_dir(r));
}

std::optional<FacePlane> face_plane(const Mesh& m, int face) {
  if (face < 0 || m.tri_region.size() != m.tris.size()) return std::nullopt;
  FacePlane f;
  double best_area = -1;
  std::map<int, double> part_area;
  std::vector<size_t> tris;
  for (size_t t = 0; t < m.tris.size(); ++t) {
    if (m.tri_region[t] != face) continue;
    tris.push_back(t);
    const vec3d raw = tri_normal_raw(m, t);
    const double a = 0.5 * length(raw);
    f.area += a;
    if (t < m.tri_part.size()) part_area[m.tri_part[t]] += a;
    if (a > best_area) {
      best_area = a;
      f.normal = normalize(raw);
      f.origin = m.positions[m.tris[t][0]];
    }
  }
  if (tris.empty() || best_area <= 0) return std::nullopt;
  const vec3d n = f.normal;
  const vec3d a = std::abs(n.x) < 0.9 ? vec3d{1, 0, 0} : vec3d{0, 1, 0};
  f.u = normalize(a - n * dot(a, n));
  f.v = cross(n, f.u);
  Polygons2D rings;
  for (size_t t : tris) {
    Ring ring;
    for (uint32_t i : m.tris[t]) {
      const vec3d p = m.positions[i] - f.origin;
      ring.push_back({dot(p, f.u), dot(p, f.v)});
    }
    rings.push_back(std::move(ring));
  }
  f.polygon = unite(rings, {});
  double best_part = -1;
  for (const auto& [p, a2] : part_area)
    if (a2 > best_part) best_part = a2, f.part = p;
  return f;
}

namespace {

Mesh selected_triangles(const Mesh& m, const Selection& s) {
  Mesh out;
  out.positions = m.positions;
  for (size_t t = 0; t < m.tris.size(); ++t) {
    const int part = t < m.tri_part.size() ? m.tri_part[t] : -1;
    const int face = t < m.tri_region.size() ? m.tri_region[t] : -1;
    const bool in = s.mode == SelectMode::Face ? face == s.face
                                               : std::find(s.parts.begin(), s.parts.end(), part) != s.parts.end();
    if (in) out.tris.push_back(m.tris[t]);
  }
  return out;
}

}  // namespace

std::map<ViewRole, Mask2D> selection_masks(const Mesh& m, const Selection& s, Dims d, const std::vector<ViewRole>& roles) {
  std::map<ViewRole, Mask2D> out;
  const Mesh sel = selected_triangles(m, s);
  for (ViewRole r : roles) {
    const ImageSize sz = view_image_size(r, d);
    out[r] = sel.tris.empty() ? Mask2D(sz.w, sz.h) : render_mesh_flat(sel, camera_for_view(r, d, 1)).coverage;
  }
  return out;
}

json selection_json(const Selection& s, const Mesh& m, Dims d, const std::vector<ViewRole>& roles) {
  json j{{"mode", to_string(s.mode)}, {"parts", s.parts}, {"face", nullptr}, {"revision", s.revision}};
  if (s.hit && s.hit->tri >= 0)
    j["hit"] = {{"triangle", s.hit->tri},
                {"part", s.hit->part},
                {"face", s.hit->face},
                {"point", {s.hit->point.x, s.hit->point.y, s.hit->point.z}}};
  if (s.face >= 0)
    if (const auto f = face_plane(m, s.face))
      j["face"] = {{"id", s.face},
                   {"part", f->part},
                   {"normal", {f->normal.x, f->normal.y, f->normal.z}},
                   {"area", f->area}};
  j["views"] = json::object();
  for (const auto& [r, mask] : selection_masks(m, s, d, roles)) {
    const Mask2D::Box b = mask.bbox();
    j["views"][std::string(to_string(r))] = {{"pixels", mask.count()},
                                             {"box", b.empty() ? json() : json{b.x0, b.y0, b.x1, b.y1}}};
  }
  return j;
}

}  // namespace pixel3d
