#include "pixel3d/edit/single_view.hpp"

#include <algorithm>
#include <cmath>

#include "pixel3d/reconstruct/prism.hpp"

namespace pixel3d {

using nlohmann::json;

bool valid_depth_preset(const std::string& p) {
  return p == "flat" || p == "thin" || p == "medium" || p == "thick" || p == "custom";
}

double single_view_depth(const SingleViewSettings& s, int w, int h) {
  const double side = std::max(1, std::min(w, h));
  if (s.preset == "flat") return 1;
  if (s.preset == "thin") return std::max(1.0, std::round(side * 0.25));
  if (s.preset == "thick") return std::max(1.0, side);
  if (s.preset == "custom") return std::max(0.25, s.depth);
  return std::max(1.0, std::round(side * 0.5));  // medium
}

json single_view_to_json(const SingleViewSettings& s) {
  return {{"enabled", s.enabled},
          {"role", to_string(s.role)},
          {"preset", s.preset},
          {"depth", s.depth},
          {"user_depth", s.user_depth}};
}

Expected<SingleViewSettings> single_view_from_json(const json& j) {
  SingleViewSettings s;
  if (j.is_null()) return s;
  try {
    s.enabled = j.value("enabled", false);
    const auto r = view_role_from_string(j.value("role", std::string("front")));
    if (!r) return make_error("project.invalid", "single view role is unknown");
    s.role = *r;
    s.preset = j.value("preset", std::string("medium"));
    s.depth = j.value("depth", 0.0);
    s.user_depth = j.value("user_depth", false);
  } catch (const std::exception& e) {
    return make_error("project.invalid", std::string("malformed single view settings: ") + e.what());
  }
  if (!valid_depth_preset(s.preset) || !std::isfinite(s.depth) || s.depth < 0 || s.depth > 4096)
    return make_error("project.invalid", "single view depth settings out of range");
  return s;
}

namespace {

// Depth range of a centred slab of thickness t in a box of extent e.
std::pair<double, double> slab(double e, double t) { return {(e - t) / 2.0, (e + t) / 2.0}; }

}  // namespace

PartGeometry single_view_parts(const SourceView& v, Dims d, double depth, const PartMasks& masks,
                               const SimplifyOptions& opt, uint8_t alpha_threshold) {
  PartGeometry g;
  const Mask2D m = opaque_mask(*v.aligned, alpha_threshold);
  const Polygons2D silhouette = simplify_feature_preserving(mask_to_polygons(m), m, opt);
  const double extent = view_depth_extent(v.role, d);
  const auto [d0, d1] = slab(extent, depth);
  manifold::Manifold base = make_prism_range(v.role, d, silhouette, d0, d1);
  for (const auto& [id, up] : masks.parts) {
    const auto it = up.masks.find(v.role);
    if (it == up.masks.end() || it->second.count() == 0 || it->second.width() != m.width() ||
        it->second.height() != m.height())
      continue;
    const Polygons2D region = clip(mask_polygons(it->second, opt), silhouette);
    const auto [p0, p1] = slab(extent, up.depth.value_or(depth));
    base = base - make_prism(v.role, d, region);
    g.parts[id] = make_prism_range(v.role, d, region, p0, p1);
  }
  g.parts[0] = base;
  g.normalize();
  return g;
}

VoxelGrid single_view_volume(const SourceView& v, Dims d, double depth, const PartMasks& masks, uint8_t alpha_threshold) {
  VoxelGrid g(d);
  const Mask2D m = opaque_mask(*v.aligned, alpha_threshold);
  const double extent = view_depth_extent(v.role, d);
  // Per-pixel depth: the first user part whose mask covers the pixel, else the default.
  std::vector<std::pair<const Mask2D*, double>> part_depths;
  for (const auto& [id, up] : masks.parts)
    if (const auto it = up.masks.find(v.role);
        it != up.masks.end() && it->second.width() == m.width() && it->second.height() == m.height())
      part_depths.emplace_back(&it->second, up.depth.value_or(depth));
  for (int z = 0; z < d.D; ++z)
    for (int y = 0; y < d.H; ++y)
      for (int x = 0; x < d.W; ++x) {
        const vec3d uvd = view_from_world(v.role, d, {x + 0.5, y + 0.5, z + 0.5});
        const int u = static_cast<int>(std::floor(uvd.x)), vv = static_cast<int>(std::floor(uvd.y));
        if (!m.get_or_false(u, vv)) continue;
        double t = depth;
        for (const auto& [mask, pd] : part_depths)
          if (mask->get(u, vv)) {
            t = pd;
            break;
          }
        const auto [a, b] = slab(extent, t);
        if (uvd.z >= a && uvd.z <= b) g.set(x, y, z, true);
      }
  return g;
}

}  // namespace pixel3d
