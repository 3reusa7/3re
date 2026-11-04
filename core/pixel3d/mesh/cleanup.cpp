#include "pixel3d/mesh/cleanup.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <tuple>

#include "pixel3d/common/union_find.hpp"

namespace pixel3d {
namespace {

using EdgeKey = std::pair<uint32_t, uint32_t>;
EdgeKey key(uint32_t a, uint32_t b) { return {std::min(a, b), std::max(a, b)}; }

std::map<EdgeKey, std::vector<int>> edge_map(const std::vector<std::array<uint32_t, 3>>& tris) {
  std::map<EdgeKey, std::vector<int>> e;
  for (size_t i = 0; i < tris.size(); ++i)
    for (int k = 0; k < 3; ++k) e[key(tris[i][k], tris[i][(k + 1) % 3])].push_back(static_cast<int>(i));
  return e;
}

bool has_directed(const std::array<uint32_t, 3>& t, uint32_t a, uint32_t b) {
  for (int k = 0; k < 3; ++k)
    if (t[k] == a && t[(k + 1) % 3] == b) return true;
  return false;
}

}  // namespace

Mesh cleanup_mesh(const Mesh& in, const CleanupOptions& opt, CleanupReport* report, const WeldOptions& weld) {
  CleanupReport rep;
  const bool has_regions = in.tri_region.size() == in.tris.size();

  // 1. Weld vertices that coincide within weld_eps.
  std::vector<vec3d> pos;
  std::vector<uint32_t> remap(in.positions.size());
  std::map<std::tuple<long long, long long, long long>, uint32_t> grid;
  for (size_t i = 0; i < in.positions.size(); ++i) {
    const vec3d p = in.positions[i];
    if (!weld.enabled) {
      remap[i] = static_cast<uint32_t>(pos.size());
      pos.push_back(p);
      continue;
    }
    const auto k = std::make_tuple(std::llround(p.x / weld.eps), std::llround(p.y / weld.eps), std::llround(p.z / weld.eps));
    auto [it, ins] = grid.try_emplace(k, static_cast<uint32_t>(pos.size()));
    if (ins) pos.push_back(p);
    remap[i] = it->second;
  }
  rep.welded = static_cast<int>(in.positions.size() - pos.size());

  // 2. Drop degenerate triangles.
  struct Tri {
    std::array<uint32_t, 3> v;
    int part;
    int region;
  };
  std::vector<Tri> tris;
  for (size_t i = 0; i < in.tris.size(); ++i) {
    const std::array<uint32_t, 3> v = {remap[in.tris[i][0]], remap[in.tris[i][1]], remap[in.tris[i][2]]};
    const double area2 = length(cross(pos[v[1]] - pos[v[0]], pos[v[2]] - pos[v[0]]));
    if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2] || area2 * 0.5 < 1e-12) {
      ++rep.removed_zero_area;
      continue;
    }
    tris.push_back({v, i < in.tri_part.size() ? in.tri_part[i] : -1, has_regions ? in.tri_region[i] : -1});
  }

  // 3. Duplicates (same orientation) keep one; coincident opposite pairs are internal: drop all.
  {
    std::map<std::array<uint32_t, 3>, std::vector<size_t>> by_set;
    for (size_t i = 0; i < tris.size(); ++i) {
      auto s = tris[i].v;
      std::sort(s.begin(), s.end());
      by_set[s].push_back(i);
    }
    std::vector<uint8_t> drop(tris.size(), 0);
    for (const auto& [s, ids] : by_set) {
      if (ids.size() < 2) continue;
      const auto& t0 = tris[ids[0]].v;
      bool mixed = false;
      for (size_t k = 1; k < ids.size(); ++k) mixed |= !has_directed(tris[ids[k]].v, t0[0], t0[1]);
      if (mixed) {
        for (size_t id : ids) drop[id] = 1;
        rep.removed_internal += static_cast<int>(ids.size());
      } else {
        for (size_t k = 1; k < ids.size(); ++k) drop[ids[k]] = 1;
        rep.removed_duplicates += static_cast<int>(ids.size() - 1);
      }
    }
    std::vector<Tri> kept;
    for (size_t i = 0; i < tris.size(); ++i)
      if (!drop[i]) kept.push_back(tris[i]);
    tris.swap(kept);
  }

  // 4. Consistent winding: propagate orientation across manifold edges.
  std::vector<std::array<uint32_t, 3>> tv;
  for (const Tri& t : tris) tv.push_back(t.v);
  {
    const auto edges = edge_map(tv);
    std::vector<uint8_t> seen(tv.size(), 0);
    for (size_t seed = 0; seed < tv.size(); ++seed) {
      if (seen[seed]) continue;
      std::queue<int> q;
      q.push(static_cast<int>(seed));
      seen[seed] = 1;
      while (!q.empty()) {
        const int t = q.front();
        q.pop();
        for (int k = 0; k < 3; ++k) {
          const uint32_t a = tv[t][k], b = tv[t][(k + 1) % 3];
          const auto& users = edges.at(key(a, b));
          if (users.size() != 2) continue;
          const int o = users[0] == t ? users[1] : users[0];
          if (seen[o]) continue;
          if (has_directed(tv[o], a, b)) {  // same direction: neighbour is flipped
            std::swap(tv[o][1], tv[o][2]);
            ++rep.reoriented_faces;
          }
          seen[o] = 1;
          q.push(o);
        }
      }
    }
  }

  // 5. Shells: outward orientation, drop tiny accidental fragments.
  UnionFind uf(tv.size());
  for (const auto& [k, users] : edge_map(tv))
    for (size_t u = 1; u < users.size(); ++u) uf.unite(users[0], users[u]);
  int shell_count = 0;
  const std::vector<int> shell = uf.components(&shell_count);
  std::vector<double> vol(shell_count, 0);
  for (size_t i = 0; i < tv.size(); ++i)
    vol[shell[i]] += dot(pos[tv[i][0]], cross(pos[tv[i][1]], pos[tv[i][2]])) / 6.0;
  std::vector<uint8_t> keep_shell(shell_count, 1);
  for (int s = 0; s < shell_count; ++s) {
    if (std::abs(vol[s]) < opt.min_fragment_volume) {
      keep_shell[s] = 0;
      ++rep.removed_fragments;
      continue;
    }
    if (vol[s] < 0) ++rep.flipped_shells;
    rep.kept_shell_volumes.push_back(std::abs(vol[s]));
  }

  // 6. Compact.
  Mesh out;
  std::vector<int64_t> new_index(pos.size(), -1);
  for (size_t i = 0; i < tv.size(); ++i) {
    if (!keep_shell[shell[i]]) continue;
    std::array<uint32_t, 3> v = tv[i];
    if (vol[shell[i]] < 0) std::swap(v[1], v[2]);
    for (uint32_t& x : v) {
      if (new_index[x] < 0) {
        new_index[x] = static_cast<int64_t>(out.positions.size());
        out.positions.push_back(pos[x]);
      }
      x = static_cast<uint32_t>(new_index[x]);
    }
    out.tris.push_back(v);
    out.tri_part.push_back(tris[i].part);
    if (has_regions) out.tri_region.push_back(tris[i].region);
  }
  if (report) *report = rep;
  return out;
}

void label_planar_regions(Mesh& m, double angle_eps) {
  const size_t n = m.tris.size();
  std::vector<vec3d> normal(n);
  std::vector<double> offset(n);
  double scale = 1;
  for (const vec3d& p : m.positions) scale = std::max({scale, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
  for (size_t i = 0; i < n; ++i) {
    normal[i] = normalize(tri_normal_raw(m, i));
    offset[i] = dot(normal[i], m.positions[m.tris[i][0]]);
  }
  UnionFind uf(n);
  for (const auto& [k, users] : edge_map(m.tris))
    for (size_t a = 0; a < users.size(); ++a)
      for (size_t b = a + 1; b < users.size(); ++b) {
        const int i = users[a], j = users[b];
        if (dot(normal[i], normal[j]) > 1 - angle_eps && std::abs(offset[i] - offset[j]) < 1e-7 * scale)
          uf.unite(i, j);
      }
  m.tri_region = uf.components();
}

}  // namespace pixel3d
