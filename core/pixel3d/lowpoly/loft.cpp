#include "pixel3d/lowpoly/loft.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pixel3d::lowpoly {
namespace {

vec3d vertex(const LoftRing& r, int j, int sides, double b_scale) {
  const double th = 2 * std::numbers::pi * j / sides;
  return r.c + r.n * (std::cos(th) * r.a) + vec3d{0, 0, 1} * (std::sin(th) * r.b * b_scale);
}

double signed_volume(const TubeMesh& m) {
  double v = 0;
  for (const auto& t : m.tris) v += dot(m.positions[t[0]], cross(m.positions[t[1]], m.positions[t[2]]));
  return v / 6;
}

int count_folds(const std::vector<LoftRing>& rs, int sides, std::vector<bool>* bad) {
  int folds = 0;
  if (bad) bad->assign(rs.size(), false);

  for (size_t i = 0; i + 1 < rs.size(); ++i) {
    const vec3d d = rs[i + 1].c - rs[i].c;
    if (dot(d, d) < 1e-18) continue;
    for (int j = 0; j < sides; ++j) {
      const double th = 2 * std::numbers::pi * j / sides, c = std::cos(th);
      const vec3d p0 = rs[i].c + rs[i].n * (c * rs[i].a), p1 = rs[i + 1].c + rs[i + 1].n * (c * rs[i + 1].a);
      if (dot(p1 - p0, d) <= 1e-9) {
        ++folds;
        if (bad) (*bad)[i] = (*bad)[i + 1] = true;
        break;
      }
    }
  }
  return folds;
}

}  // namespace

int cap_triangles(Cap c, int sides) { return c == Cap::Apex ? sides : sides - 2; }

int tube_triangles(int sides, int rings, Cap start, Cap end) {
  return 2 * sides * (rings - 1) + cap_triangles(start, sides) + cap_triangles(end, sides);
}

LoftRing ring_from_station(const Station& st, int model_height) {
  LoftRing r;
  r.c = {st.centre.x, model_height - st.centre.y, st.z_c};
  r.n = normalize(vec3d{st.n.x, -st.n.y, 0});
  // A ring never collapses (a zero-width ring makes zero-area faces); tips are apexes, not rings.
  r.a = std::max(st.half_w, 0.25);
  r.b = std::max(st.half_d, 0.25);
  r.s = st.s;
  r.owner = st.owner;
  return r;
}

Station station_at(const std::vector<Station>& dense, double s) {
  if (dense.empty()) return {};
  if (s <= dense.front().s) return dense.front();
  if (s >= dense.back().s) return dense.back();
  const auto it = std::lower_bound(dense.begin(), dense.end(), s, [](const Station& a, double v) { return a.s < v; });
  const Station& b = *it;
  const Station& a = *(it - 1);
  const double t = b.s > a.s ? (s - a.s) / (b.s - a.s) : 0;
  Station r = t < 0.5 ? a : b;
  r.s = s;
  r.centre = a.centre + (b.centre - a.centre) * t;
  const vec2d n = a.n + (b.n - a.n) * t;
  const double ln = length(n);
  r.n = ln > 0 ? n * (1 / ln) : a.n;
  r.half_w = a.half_w + (b.half_w - a.half_w) * t;
  r.z_c = a.z_c + (b.z_c - a.z_c) * t;
  r.half_d = a.half_d + (b.half_d - a.half_d) * t;
  r.measured = a.measured && b.measured;
  return r;
}

int untangle(std::vector<LoftRing>& rings, int sides) {
  // Each folding pair takes the mean of its two cross directions (parallel rings
  // never fold while their centres advance); repeat until nothing folds.
  std::vector<bool> changed(rings.size(), false);
  for (int it = 0; it < 256; ++it) {
    bool any = false;
    for (size_t i = 0; i + 1 < rings.size(); ++i) {
      std::vector<LoftRing> pair{rings[i], rings[i + 1]};
      if (count_folds(pair, sides, nullptr) == 0) continue;
      const vec3d m = normalize(rings[i].n + rings[i + 1].n);
      rings[i].n = rings[i + 1].n = m;
      changed[i] = changed[i + 1] = any = true;
    }
    if (!any) break;
  }
  return static_cast<int>(std::count(changed.begin(), changed.end(), true));
}

TubeMesh build_tube(const TubeShape& t) {
  const int N = t.sides, R = static_cast<int>(t.rings.size());
  double max_sin = 0;
  for (int j = 0; j < N; ++j) max_sin = std::max(max_sin, std::abs(std::sin(2 * std::numbers::pi * j / N)));
  const double b_scale = max_sin > 0 ? 1 / max_sin : 1;
  TubeMesh m;
  for (int i = 0; i < R; ++i)
    for (int j = 0; j < N; ++j) {
      m.positions.push_back(vertex(t.rings[static_cast<size_t>(i)], j, N, b_scale));
      m.vertex_ring.push_back(i);
      m.vertex_side.push_back(j);
    }
  auto idx = [N](int i, int j) { return static_cast<uint32_t>(i * N + ((j % N) + N) % N); };
  for (int i = 0; i + 1 < R; ++i)
    for (int j = 0; j < N; ++j) {
      m.tris.push_back({idx(i, j), idx(i, j + 1), idx(i + 1, j + 1)});
      m.tris.push_back({idx(i, j), idx(i + 1, j + 1), idx(i + 1, j)});
    }
  auto cap = [&](Cap c, int ring, vec3d apex, bool start) {
    if (c == Cap::Apex) {
      const auto a = static_cast<uint32_t>(m.positions.size());
      m.positions.push_back(apex);
      m.vertex_ring.push_back(ring);
      m.vertex_side.push_back(-1);
      for (int j = 0; j < N; ++j)
        m.tris.push_back(start ? std::array<uint32_t, 3>{a, idx(ring, j + 1), idx(ring, j)}
                               : std::array<uint32_t, 3>{a, idx(ring, j), idx(ring, j + 1)});
    } else {
      for (int j = 1; j + 1 < N; ++j)
        m.tris.push_back(start ? std::array<uint32_t, 3>{idx(ring, 0), idx(ring, j + 1), idx(ring, j)}
                               : std::array<uint32_t, 3>{idx(ring, 0), idx(ring, j), idx(ring, j + 1)});
    }
  };
  cap(t.cap_start, 0, t.apex_start, true);
  cap(t.cap_end, R - 1, t.apex_end, false);
  if (signed_volume(m) < 0)
    for (auto& tri : m.tris) std::swap(tri[1], tri[2]);
  return m;
}

}  // namespace pixel3d::lowpoly
