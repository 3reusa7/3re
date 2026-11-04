#include "pixel3d/polygon/raster.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace pixel3d {

Mask2D rasterize_pixel_centres(const Polygons2D& p, int w, int h) {
  Mask2D m(w, h);
  std::vector<std::pair<double, int>> xs;  // crossing x, winding contribution
  for (int v = 0; v < h; ++v) {
    const double y = v + 0.5;
    xs.clear();
    for (const Ring& r : p)
      for (size_t i = 0; i < r.size(); ++i) {
        const vec2d a = r[i], b = r[(i + 1) % r.size()];
        // Half-open rule so a vertex exactly on the scanline is counted once.
        if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
          const double x = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
          xs.emplace_back(x, b.y > a.y ? 1 : -1);
        }
      }
    std::sort(xs.begin(), xs.end());
    int wind = 0;
    for (size_t k = 0; k + 1 <= xs.size(); ++k) {
      wind += xs[k].second;
      if (wind == 0 || k + 1 == xs.size()) continue;
      // Pixels whose centre lies in [xs[k], xs[k+1]).
      const int u0 = std::max(0, static_cast<int>(std::ceil(xs[k].first - 0.5)));
      const int u1 = std::min(w - 1, static_cast<int>(std::ceil(xs[k + 1].first - 0.5)) - 1);
      for (int u = u0; u <= u1; ++u) m.set(u, v, true);
    }
  }
  return m;
}

}  // namespace pixel3d
