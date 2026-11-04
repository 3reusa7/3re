#include "pixel3d/style/symmetry.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace pixel3d {

SymmetryResult detect_mirror_symmetry(const std::map<ViewRole, Mask2D>& masks, Dims d) {
  SymmetryResult r;
  for (ViewRole role : {ViewRole::Front, ViewRole::Back, ViewRole::Top, ViewRole::Bottom}) {
    const auto it = masks.find(role);
    if (it == masks.end()) continue;
    const Mask2D& m = it->second;
    r.checked.push_back(role);
    for (int y = 0; y < m.height(); ++y)
      for (int x = 0; x < m.width(); ++x)
        if (m.get(x, y) != m.get(m.width() - 1 - x, y)) {
          r.reason = "the " + std::string(to_string(role)) + " drawing is not its own mirror image at (" +
                     std::to_string(x) + ", " + std::to_string(y) + ")";
          return r;
        }
  }
  if (r.checked.empty()) {
    r.reason = "no drawing shows left and right";
    return r;
  }
  r.symmetric = true;
  r.axis_x = d.W / 2.0;
  return r;
}

PartGeometry mirror_parts(const PartGeometry& g, double axis_x, MirrorReport* report) {
  constexpr double eps = 0.01;  // px
  MirrorReport rep;
  auto mirror = [&](const manifold::Manifold& m) { return m.Mirror({1, 0, 0}).Translate({2 * axis_x, 0, 0}); };
  std::map<int, manifold::Box> box;
  for (const auto& [id, m] : g.parts) box[id] = m.BoundingBox();
  auto near = [](double a, double b) { return std::abs(a - b) <= eps; };
  auto counterpart = [&](const manifold::Box& a, const manifold::Box& b) {
    return near(a.min.x, 2 * axis_x - b.max.x) && near(a.max.x, 2 * axis_x - b.min.x) && near(a.min.y, b.min.y) &&
           near(a.max.y, b.max.y) && near(a.min.z, b.min.z) && near(a.max.z, b.max.z);
  };
  PartGeometry out = g;
  std::set<int> used;
  for (const auto& [id, m] : g.parts) {
    const manifold::Box& b = box[id];
    if (b.min.x < axis_x - eps && b.max.x > axis_x + eps) {
      // Crosses the plane: keep x ≤ axis and mirror it.
      const manifold::Manifold half = m.TrimByPlane({-1, 0, 0}, -axis_x);
      out.parts[id] = half + mirror(half);
      ++rep.halved;
      continue;
    }
    if (b.min.x < axis_x - eps) continue;  // left side: the source of its counterpart
    int match = -1;
    for (const auto& [lid, lm] : g.parts)
      if (lid != id && !used.contains(lid) && box[lid].max.x <= axis_x + eps && counterpart(box[lid], b) &&
          std::abs(lm.Volume() - m.Volume()) <= 0.02 * std::max(m.Volume(), 1e-9)) {
        match = lid;
        break;
      }
    if (match < 0) {
      rep.skipped.push_back(id);
      continue;
    }
    used.insert(match);
    out.parts[id] = mirror(g.parts.at(match));
    ++rep.mirrored_pairs;
  }
  for (const auto& [id, m] : g.parts)
    if (box[id].max.x <= axis_x + eps && box[id].min.x < axis_x - eps && !used.contains(id)) rep.skipped.push_back(id);
  std::sort(rep.skipped.begin(), rep.skipped.end());
  out.normalize();
  if (report) *report = rep;
  return out;
}

}  // namespace pixel3d
