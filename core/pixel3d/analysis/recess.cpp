#include "pixel3d/analysis/recess.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace pixel3d {
namespace {

// Cells along the ray through pixel (u, v): (first hit depth index, solid run length); (-1, 0) if empty.
std::pair<int, int> first_run(const VoxelGrid& g, ViewRole role, Dims d, int u, int v) {
  const int extent = static_cast<int>(view_depth_extent(role, d));
  int first = -1, run = 0;
  for (int k = 0; k < extent; ++k) {
    const vec3d p = world_from_view(role, d, u + 0.5, v + 0.5, k + 0.5);
    const bool solid = g.get_or_false(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                                      static_cast<int>(std::floor(p.z)));
    if (solid) {
      if (first < 0) first = k;
      ++run;
    } else if (first >= 0) {
      break;
    }
  }
  return {first, run};
}

}  // namespace

std::vector<RecessFinding> analyze_recesses(const std::map<ViewRole, RegionMap>& regions, const VoxelGrid& kept, Dims d,
                                            double delta) {
  std::vector<RecessFinding> out;
  for (const auto& [role, rm] : regions) {
    for (const Region& r : rm.regions) {
      if (!r.enclosed) continue;
      // The region plus the outline pixels hugging it (drawn edges of the same surface).
      std::set<int> px(r.pixels.begin(), r.pixels.end());
      for (int i : r.pixels) {
        const int u = i % rm.w, v = i / rm.w;
        const int du[] = {1, -1, 0, 0}, dv[] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
          const int nu = u + du[k], nv = v + dv[k];
          if (nu < 0 || nv < 0 || nu >= rm.w || nv >= rm.h) continue;
          if (rm.at(nu, nv) == -1 && first_run(kept, role, d, nu, nv).first >= 0) px.insert(nv * rm.w + nu);
        }
      }
      // Rim: the 1-px ring just outside, which must have content.
      std::vector<int> rim_depths;
      for (int i : px) {
        const int u = i % rm.w, v = i / rm.w;
        for (int dv2 = -1; dv2 <= 1; ++dv2)
          for (int du2 = -1; du2 <= 1; ++du2) {
            const int nu = u + du2, nv = v + dv2;
            if (nu < 0 || nv < 0 || nu >= rm.w || nv >= rm.h || px.contains(nv * rm.w + nu)) continue;
            const auto [depth, run] = first_run(kept, role, d, nu, nv);
            if (depth >= 0) rim_depths.push_back(depth);
          }
      }
      if (rim_depths.empty()) continue;
      std::nth_element(rim_depths.begin(), rim_depths.begin() + rim_depths.size() / 2, rim_depths.end());
      const int rim = rim_depths[rim_depths.size() / 2];
      bool flat = true;
      double column = 0;
      Mask2D::Box box{rm.w, rm.h, 0, 0};
      for (int i : px) {
        const int u = i % rm.w, v = i / rm.w;
        const auto [depth, run] = first_run(kept, role, d, u, v);
        if (depth != rim) flat = false;
        column += run;
        box.x0 = std::min(box.x0, u), box.y0 = std::min(box.y0, v);
        box.x1 = std::max(box.x1, u + 1), box.y1 = std::max(box.y1, v + 1);
      }
      if (!flat) continue;  // the silhouettes already gave this region its own depth
      double nb_lum = 0, nb_n = 0;
      for (int n : r.neighbours) {
        nb_lum += rm.regions[n].mean_luminance * rm.regions[n].pixels.size();
        nb_n += rm.regions[n].pixels.size();
      }
      const double ratio = nb_n > 0 && nb_lum > 0 ? r.mean_luminance / (nb_lum / nb_n) : 1.0;
      RecessFinding f;
      f.view = role;
      f.region = r.id;
      f.kind = ratio < 1.0 - delta ? "possible_recess" : "inferred_flush";
      f.area = static_cast<double>(px.size());
      f.luminance_ratio = ratio;
      f.column_volume = column;
      f.box = box;
      out.push_back(f);
    }
  }
  return out;
}

}  // namespace pixel3d
