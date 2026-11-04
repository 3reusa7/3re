// Bilateral symmetry of the generated geometry (a vertical plane). Only a plane the mesh itself
// verifies is used, and only to suggest colour regions for surfaces the picture does not show.
#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include "auto_texture_internal.hpp"

namespace p3dasset::autotex::detail {
using namespace pixel3d;
namespace {
struct Grid {
  double cell;
  std::unordered_map<int64_t, std::vector<uint32_t>> cells;
  static int64_t key(int64_t x, int64_t y, int64_t z) { return (x * 73856093) ^ (y * 19349663) ^ (z * 83492791); }
  Grid(const Mesh& m, double c) : cell(c) {
    cells.reserve(m.positions.size() / 2);
    for (size_t i = 0; i < m.positions.size(); ++i) {
      const auto p = m.positions[i];
      cells[key(int64_t(std::floor(p.x / cell)), int64_t(std::floor(p.y / cell)), int64_t(std::floor(p.z / cell)))].push_back(uint32_t(i));
    }
  }
  std::pair<int32_t, double> nearest(const Mesh& m, vec3d p) const {
    const int64_t cx = int64_t(std::floor(p.x / cell)), cy = int64_t(std::floor(p.y / cell)), cz = int64_t(std::floor(p.z / cell));
    int32_t best = -1;
    double bd = 1e300;
    for (int64_t dz = -1; dz <= 1; ++dz)
      for (int64_t dy = -1; dy <= 1; ++dy)
        for (int64_t dx = -1; dx <= 1; ++dx) {
          auto it = cells.find(key(cx + dx, cy + dy, cz + dz));
          if (it == cells.end()) continue;
          for (auto v : it->second) {
            const vec3d d = m.positions[v] - p;
            const double d2 = dot(d, d);
            if (d2 < bd) { bd = d2; best = int32_t(v); }
          }
        }
    return {best, best >= 0 ? std::sqrt(bd) : 2 * cell};
  }
};
vec3d plane_normal(double deg) {
  const double a = deg * std::numbers::pi / 180;
  return {std::cos(a), 0, std::sin(a)};
}
}  // namespace

Symmetry detect_symmetry(const Mesh& m, double diag, const Deadline& deadline) {
  Symmetry s;
  const auto started = std::chrono::steady_clock::now();
  const Grid grid(m, diag / 160);
  vec3d centre{};
  for (auto p : m.positions) centre = centre + p;
  centre = centre / double(m.positions.size());
  auto error = [&](double deg, double offset, size_t samples) {
    const vec3d n = plane_normal(deg);
    const size_t step = std::max<size_t>(1, m.positions.size() / samples);
    double sum = 0;
    size_t count = 0;
    for (size_t i = 0; i < m.positions.size(); i += step) {
      const vec3d p = m.positions[i];
      const vec3d r = p - n * (2 * (dot(p, n) - offset));
      sum += std::min(grid.nearest(m, r).second, 2 * grid.cell);
      ++count;
    }
    return sum / double(std::max<size_t>(count, 1)) / diag;
  };
  // Candidate vertical planes: the world axes and the principal horizontal axes of the vertices.
  double sxx = 0, sxz = 0, szz = 0;
  for (auto p : m.positions) {
    const vec3d d = p - centre;
    sxx += d.x * d.x; sxz += d.x * d.z; szz += d.z * d.z;
  }
  const double pca = 0.5 * std::atan2(2 * sxz, sxx - szz) * 180 / std::numbers::pi;
  double best_deg = 0, best_off = 0, best = 1e300;
  for (double deg : {0.0, 90.0, pca, pca + 90}) {
    const double c = dot(centre, plane_normal(deg));
    for (int k = -12; k <= 12; ++k) {
      const double off = c + k * 0.0025 * diag;
      const double e = error(deg, off, 1500);
      if (e < best) { best = e; best_deg = deg; best_off = off; }
    }
  }
  deadline.check("symmetry");
  for (double step : {3.0, 1.0}) {
    const double d0 = best_deg, o0 = best_off;
    for (int a = -2; a <= 2; ++a)
      for (int k = -4; k <= 4; ++k) {
        const double deg = d0 + a * step, off = o0 + k * 0.001 * diag * step;
        const double e = error(deg, off, 3000);
        if (e < best) { best = e; best_deg = deg; best_off = off; }
      }
  }
  s.normal = plane_normal(best_deg);
  s.offset = best_off;
  s.error = error(best_deg, best_off, 20000);
  // Generated meshes are never exactly symmetric; 0.6% of the size is "the same shape" at 512 voxels.
  s.accepted = s.error < 0.006;
  s.quality = std::clamp(1 - s.error / 0.006, 0.0, 1.0);
  if (s.accepted) {
    s.partner.assign(m.positions.size(), -1);
    for (size_t i = 0; i < m.positions.size(); ++i) {
      const vec3d p = m.positions[i];
      const auto [v, d] = grid.nearest(m, p - s.normal * (2 * (dot(p, s.normal) - s.offset)));
      if (v >= 0 && d < 0.012 * diag) s.partner[i] = v;
    }
  }
  s.evidence = {{"plane_normal", {s.normal.x, s.normal.y, s.normal.z}}, {"plane_offset", s.offset}, {"mean_mirror_error", s.error},
                {"accepted", s.accepted}, {"threshold", 0.006},
                {"use", "only for surfaces the picture does not show: low-pass colour regions of the verified mirror side, never text or fine detail"},
                {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
  return s;
}
}  // namespace p3dasset::autotex::detail
