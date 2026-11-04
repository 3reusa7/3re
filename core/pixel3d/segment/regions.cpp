#include "pixel3d/segment/regions.hpp"

#include <algorithm>
#include <set>

namespace pixel3d {

RegionMap compute_regions(const ImageRGBA& img, const MaterialFamilies& fam, uint8_t alpha_threshold) {
  RegionMap rm;
  rm.w = img.width();
  rm.h = img.height();
  const int w = rm.w, h = rm.h;
  rm.label.assign(static_cast<size_t>(w) * h, -1);
  std::vector<int> family(rm.label.size(), -1);
  enum Kind : uint8_t { kEmpty, kOutline, kFill };
  std::vector<uint8_t> kind(rm.label.size(), kEmpty);
  for (int v = 0; v < h; ++v)
    for (int u = 0; u < w; ++u) {
      const Rgba8 c = img.at(u, v);
      const size_t i = static_cast<size_t>(v) * w + u;
      if (c.a < alpha_threshold) continue;
      family[i] = fam.family_of(c, alpha_threshold);
      kind[i] = (family[i] >= 0 && family[i] == fam.outline_family) ? kOutline : kFill;
    }
  auto kind_at = [&](int u, int v) -> uint8_t {
    return (u < 0 || v < 0 || u >= w || v >= h) ? uint8_t{kEmpty} : kind[size_t(v) * w + u];
  };
  const int du[] = {1, -1, 0, 0}, dv[] = {0, 0, 1, -1};

  // Flood fill same-family fill pixels.
  std::vector<int> stack;
  for (int v = 0; v < h; ++v)
    for (int u = 0; u < w; ++u) {
      const size_t i0 = static_cast<size_t>(v) * w + u;
      if (kind[i0] != kFill || rm.label[i0] >= 0) continue;
      Region r;
      r.id = static_cast<int>(rm.regions.size());
      r.family = family[i0];
      r.box = Mask2D::Box{u, v, u + 1, v + 1};
      stack.assign(1, static_cast<int>(i0));
      rm.label[i0] = r.id;
      while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        r.pixels.push_back(i);
        const int pu = i % w, pv = i / w;
        r.box.x0 = std::min(r.box.x0, pu), r.box.y0 = std::min(r.box.y0, pv);
        r.box.x1 = std::max(r.box.x1, pu + 1), r.box.y1 = std::max(r.box.y1, pv + 1);
        for (int k = 0; k < 4; ++k) {
          const int nu = pu + du[k], nv = pv + dv[k];
          if (kind_at(nu, nv) != kFill) continue;
          const size_t j = static_cast<size_t>(nv) * w + nu;
          if (rm.label[j] >= 0 || family[j] != r.family) continue;
          rm.label[j] = r.id;
          stack.push_back(static_cast<int>(j));
        }
      }
      rm.regions.push_back(std::move(r));
    }

  // Adjacency (direct or across one outline pixel), silhouette contact, luminance.
  for (Region& r : rm.regions) {
    std::set<int> nb;
    double lum = 0;
    for (int i : r.pixels) {
      const int pu = i % w, pv = i / w;
      lum += relative_luminance(img.at(pu, pv));
      for (int k = 0; k < 4; ++k) {
        const int nu = pu + du[k], nv = pv + dv[k];
        const uint8_t kd = kind_at(nu, nv);
        if (kd == kEmpty) {
          r.touches_silhouette = true;
        } else if (kd == kFill) {
          const int o = rm.label[size_t(nv) * w + nu];
          if (o != r.id) nb.insert(o);
        } else {  // outline: look one step further in every direction
          for (int m = 0; m < 4; ++m) {
            const int qu = nu + du[m], qv = nv + dv[m];
            const uint8_t kq = kind_at(qu, qv);
            if (kq == kEmpty) r.touches_silhouette = true;
            else if (kq == kFill && rm.label[size_t(qv) * w + qu] != r.id) nb.insert(rm.label[size_t(qv) * w + qu]);
          }
        }
      }
    }
    r.neighbours.assign(nb.begin(), nb.end());
    r.mean_luminance = r.pixels.empty() ? 0 : lum / static_cast<double>(r.pixels.size());
    r.enclosed = !r.touches_silhouette && !r.neighbours.empty();
  }
  return rm;
}

}  // namespace pixel3d
