#include "pixel3d/reconstruct/prism.hpp"

#include <algorithm>
#include <limits>

#include "pixel3d/image/mask.hpp"

namespace pixel3d {

manifold::Manifold make_prism(ViewRole role, Dims d, const Polygons2D& region) {
  return make_prism_range(role, d, region, -1.0, view_depth_extent(role, d) + 1.0);
}

manifold::Manifold make_prism_range(ViewRole role, Dims d, const Polygons2D& region, double depth0, double depth1) {
  manifold::Polygons polys;
  for (const Ring& r : region) {
    manifold::SimplePolygon sp;
    for (const vec2d& p : r) sp.push_back({p.x, p.y});
    polys.push_back(std::move(sp));
  }
  if (polys.empty() || depth1 <= depth0) return manifold::Manifold();
  // Local frame (u, v, depth) → world is affine with determinant +1 for every
  // role (§4), so outer rings with positive area extrude outward-facing.
  manifold::Manifold m = manifold::Manifold::Extrude(polys, depth1 - depth0).Translate({0, 0, depth0});
  const vec3d o = world_from_view(role, d, 0, 0, 0);
  const vec3d eu = world_from_view(role, d, 1, 0, 0) - o;
  const vec3d ev = world_from_view(role, d, 0, 1, 0) - o;
  const vec3d ed = world_from_view(role, d, 0, 0, 1) - o;
  const manifold::mat3x4 xf({eu.x, eu.y, eu.z}, {ev.x, ev.y, ev.z}, {ed.x, ed.y, ed.z}, {o.x, o.y, o.z});
  return m.Transform(xf);
}

std::map<ViewRole, ViewPolygons> build_view_polygons(const std::vector<SourceView>& views, const SimplifyOptions& opt,
                                                     uint8_t alpha_threshold) {
  std::map<ViewRole, ViewPolygons> out;
  for (const SourceView& v : views) {
    if (!v.aligned) continue;
    const Mask2D m = opaque_mask(*v.aligned, alpha_threshold);
    ViewPolygons vp;
    vp.role = v.role;
    vp.exact = mask_to_polygons(m);
    vp.simplified = simplify_feature_preserving(vp.exact, m, opt);
    out.emplace(v.role, std::move(vp));
  }
  return out;
}

Polygons2D atom_region(const ViewPolygons& vp, const Band& band, const BandView& bv, const Atom& atom, int H) {
  constexpr double kFar = 1e6;
  const Strand& s = bv.strands[atom.strand];
  // Neighbouring atoms of the same strand share an exact split column.
  int first = atom.id, last = atom.id;
  for (const Atom& a : bv.atoms)
    if (a.strand == atom.strand) first = std::min(first, a.id), last = std::max(last, a.id);
  Polygons2D corridor;
  for (size_t r = 0; r < s.runs.size(); ++r) {
    const Run& run = s.runs[r];
    double left = atom.a, right = atom.b;
    if (atom.id == first) {
      left = -kFar;
      for (const Strand& o : bv.strands)
        if (o.id != s.id && o.runs[r].b <= run.a) left = std::max(left, (o.runs[r].b + run.a) / 2.0);
    }
    if (atom.id == last) {
      right = kFar;
      for (const Strand& o : bv.strands)
        if (o.id != s.id && o.runs[r].a >= run.b) right = std::min(right, (run.b + o.runs[r].a) / 2.0);
    }
    const double v0 = H - 1 - run.y;
    for (const Ring& ring : rect_polys(left, v0, right, v0 + 1)) corridor.push_back(ring);
  }
  (void)band;
  return clip(vp.simplified, unite(corridor, {}));
}

}  // namespace pixel3d
