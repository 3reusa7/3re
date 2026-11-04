#include "pixel3d/edit/brush.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace pixel3d {

std::vector<std::pair<std::pair<int, int>, double>> brush_weights(const std::vector<vec2d>& points, const BrushParams& p,
                                                                   int width, int height) {
  std::map<std::pair<int, int>, double> w;
  const double r = std::max(0.0, p.radius);
  auto dab = [&](vec2d c) {
    if (r < 1.0) {  // single pixel
      const int u = static_cast<int>(std::floor(c.x)), v = static_cast<int>(std::floor(c.y));
      if (u >= 0 && v >= 0 && u < width && v < height) w[{u, v}] = 1.0;
      return;
    }
    const int u0 = std::max(0, static_cast<int>(std::floor(c.x - r))), u1 = std::min(width - 1, static_cast<int>(std::ceil(c.x + r)));
    const int v0 = std::max(0, static_cast<int>(std::floor(c.y - r))), v1 = std::min(height - 1, static_cast<int>(std::ceil(c.y + r)));
    const double hard = std::clamp(p.hardness, 0.0, 1.0);
    for (int v = v0; v <= v1; ++v)
      for (int u = u0; u <= u1; ++u) {
        const double t = std::hypot(u + 0.5 - c.x, v + 0.5 - c.y) / r;
        if (t > 1.0) continue;
        const double f = t <= hard ? 1.0 : (hard >= 1.0 ? 1.0 : 1.0 - (t - hard) / (1.0 - hard));
        if (f <= 0) continue;
        double& slot = w[{u, v}];
        slot = std::max(slot, f);
      }
  };
  for (size_t i = 0; i < points.size(); ++i) {
    if (i == 0) {
      dab(points[0]);
      continue;
    }
    const vec2d a = points[i - 1], b = points[i];
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    const double step = std::max(0.25, r * 0.5);
    const int n = std::max(1, static_cast<int>(std::ceil(len / step)));
    for (int k = 1; k <= n; ++k) dab({a.x + (b.x - a.x) * k / n, a.y + (b.y - a.y) * k / n});
  }
  return {w.begin(), w.end()};
}

std::vector<DepthSample> depth_brush_samples(const std::vector<vec2d>& points, const BrushParams& p, int width, int height) {
  std::vector<DepthSample> out;
  for (const auto& [px, weight] : brush_weights(points, p, width, height)) {
    double off = (p.raise ? 1.0 : -1.0) * p.strength * weight;
    if (p.quantum > 0) off = std::round(off / p.quantum) * p.quantum;
    if (std::abs(off) < 1e-9) continue;
    out.push_back(DepthSample{px.first, px.second, off});
  }
  return out;
}

Mask2D mask_brush(const std::vector<vec2d>& points, const BrushParams& p, int width, int height) {
  Mask2D m(width, height);
  for (const auto& [px, weight] : brush_weights(points, p, width, height))
    if (weight > 0) m.set(px.first, px.second, true);
  return m;
}

}  // namespace pixel3d
