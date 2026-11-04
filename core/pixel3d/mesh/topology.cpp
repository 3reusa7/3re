#include "pixel3d/mesh/topology.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "pixel3d/common/union_find.hpp"
#include "pixel3d/mesh/cleanup.hpp"

namespace pixel3d {

TopologyReport analyze_topology(const Mesh& m, double area_eps) {
  TopologyReport t;
  t.triangle_count = static_cast<int>(m.tris.size());

  // Directed uses per undirected edge.
  std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<uint32_t, uint32_t>>> edges;
  std::set<uint32_t> used;
  for (const auto& tri : m.tris)
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = tri[k], b = tri[(k + 1) % 3];
      edges[{std::min(a, b), std::max(a, b)}].push_back({a, b});
      used.insert(a);
    }
  t.vertex_count = static_cast<int>(used.size());
  for (const auto& [key, uses] : edges) {
    if (uses.size() == 1) ++t.boundary_edges;
    else if (uses.size() > 2) ++t.nonmanifold_edges;
    else if (uses[0] == uses[1]) ++t.inconsistent_winding_edges;
  }
  t.watertight = t.boundary_edges == 0 && t.nonmanifold_edges == 0;

  // Duplicate and internal faces via the sorted vertex triple.
  std::map<std::array<uint32_t, 3>, std::vector<size_t>> by_set;
  for (size_t i = 0; i < m.tris.size(); ++i) {
    auto s = m.tris[i];
    std::sort(s.begin(), s.end());
    by_set[s].push_back(i);
  }
  auto same_orientation = [&](size_t i, size_t j) {
    const auto& a = m.tris[i];
    const auto& b = m.tris[j];
    for (int r = 0; r < 3; ++r)
      if (a[0] == b[r] && a[1] == b[(r + 1) % 3] && a[2] == b[(r + 2) % 3]) return true;
    return false;
  };
  for (const auto& [key, ids] : by_set) {
    if (ids.size() < 2) continue;
    int fwd = 1, rev = 0;
    for (size_t k = 1; k < ids.size(); ++k) (same_orientation(ids[0], ids[k]) ? fwd : rev)++;
    t.duplicate_faces += (fwd - 1) + std::max(0, rev - 1);
    t.internal_faces += std::min(fwd, rev);
  }

  for (size_t i = 0; i < m.tris.size(); ++i) {
    if (length(tri_normal_raw(m, i)) * 0.5 < area_eps) ++t.zero_area_faces;
    const vec3d a = m.positions[m.tris[i][0]], b = m.positions[m.tris[i][1]], c = m.positions[m.tris[i][2]];
    t.signed_volume += dot(a, cross(b, c)) / 6.0;
  }

  // Shells: triangles connected through shared edges.
  UnionFind tri_uf(m.tris.size());
  std::map<std::pair<uint32_t, uint32_t>, int> first_tri;
  for (size_t i = 0; i < m.tris.size(); ++i)
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = m.tris[i][k], b = m.tris[i][(k + 1) % 3];
      auto [it, ins] = first_tri.try_emplace({std::min(a, b), std::max(a, b)}, static_cast<int>(i));
      if (!ins) tri_uf.unite(static_cast<int>(i), it->second);
    }
  tri_uf.components(&t.shells);

  // Non-manifold vertices: incident triangles must form a single edge-connected fan.
  std::map<uint32_t, std::vector<int>> incident;
  for (size_t i = 0; i < m.tris.size(); ++i)
    for (uint32_t v : m.tris[i]) incident[v].push_back(static_cast<int>(i));
  for (const auto& [v, tris] : incident) {
    UnionFind fan(tris.size());
    for (size_t x = 0; x < tris.size(); ++x)
      for (size_t y = x + 1; y < tris.size(); ++y) {
        int shared = 0;
        for (uint32_t p : m.tris[tris[x]])
          for (uint32_t q : m.tris[tris[y]]) shared += (p == q && p != v) ? 1 : 0;
        if (shared > 0) fan.unite(static_cast<int>(x), static_cast<int>(y));
      }
    int fans = 0;
    fan.components(&fans);
    if (fans > 1) ++t.nonmanifold_vertices;
  }

  if (m.tri_region.size() == m.tris.size() && !m.tris.empty()) {
    t.planar_regions = *std::max_element(m.tri_region.begin(), m.tri_region.end()) + 1;
  } else {
    Mesh copy = m;
    label_planar_regions(copy);
    t.planar_regions = copy.tri_region.empty() ? 0 : *std::max_element(copy.tri_region.begin(), copy.tri_region.end()) + 1;
  }
  return t;
}

}  // namespace pixel3d
