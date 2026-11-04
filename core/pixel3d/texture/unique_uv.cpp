#include "pixel3d/texture/unique_uv.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace pixel3d {
namespace {

int find(std::vector<int>& p, int a) {
  while (p[a] != a) a = p[a] = p[p[a]];
  return a;
}

}  // namespace

TexturedMesh add_unique_uv(const TexturedMesh& m, double texels_per_unit, int padding, UniqueUVReport* report) {
  const size_t T = m.indices.size() / 3;
  const double tpu = std::max(texels_per_unit, 1e-6);
  const int pad = std::max(padding, 0);
  std::vector<vec3d> normal(T);
  for (size_t t = 0; t < T; ++t)
    normal[t] = normalize(cross(m.positions[m.indices[3 * t + 1]] - m.positions[m.indices[3 * t]],
                                m.positions[m.indices[3 * t + 2]] - m.positions[m.indices[3 * t]]));
  auto part = [&](size_t t) { return t < m.tri_part.size() ? m.tri_part[t] : -1; };

  // Charts: coplanar triangles of one part joined across shared edges (by position).
  std::map<std::array<double, 3>, uint32_t> pos_id;
  std::vector<uint32_t> pid(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const vec3d p = m.positions[i];
    pid[i] = pos_id.emplace(std::array<double, 3>{p.x, p.y, p.z}, static_cast<uint32_t>(pos_id.size())).first->second;
  }
  std::vector<int> parent(T);
  std::iota(parent.begin(), parent.end(), 0);
  std::map<std::pair<uint32_t, uint32_t>, std::vector<int>> edges;
  for (size_t t = 0; t < T; ++t)
    for (int k = 0; k < 3; ++k) {
      uint32_t a = pid[m.indices[3 * t + k]], b = pid[m.indices[3 * t + (k + 1) % 3]];
      if (a > b) std::swap(a, b);
      edges[{a, b}].push_back(static_cast<int>(t));
    }
  for (const auto& [e, ts] : edges)
    for (size_t i = 0; i < ts.size(); ++i)
      for (size_t j = i + 1; j < ts.size(); ++j)
        if (part(ts[i]) == part(ts[j]) && dot(normal[ts[i]], normal[ts[j]]) > 1 - 1e-9)
          parent[find(parent, ts[i])] = find(parent, ts[j]);

  struct Chart {
    vec3d u, v;
    double min_u = 1e300, min_v = 1e300, max_u = -1e300, max_v = -1e300;
    int w = 0, h = 0, x = 0, y = 0;
  };
  std::map<int, int> chart_of_root;
  std::vector<int> chart(T);
  std::vector<Chart> charts;
  for (size_t t = 0; t < T; ++t) {
    const int root = find(parent, static_cast<int>(t));
    auto [it, added] = chart_of_root.try_emplace(root, static_cast<int>(charts.size()));
    if (added) {
      Chart c;
      const vec3d n = normal[root];
      // u stays horizontal where it can; v = n × u points down the texture.
      const vec3d up = std::abs(n.y) < 0.9 ? vec3d{0, 1, 0} : vec3d{0, 0, -1};
      c.u = normalize(cross(up, n));
      c.v = cross(n, c.u);
      charts.push_back(c);
    }
    chart[t] = it->second;
    Chart& c = charts[it->second];
    for (int k = 0; k < 3; ++k) {
      const vec3d p = m.positions[m.indices[3 * t + k]];
      c.min_u = std::min(c.min_u, dot(p, c.u) * tpu), c.max_u = std::max(c.max_u, dot(p, c.u) * tpu);
      c.min_v = std::min(c.min_v, dot(p, c.v) * tpu), c.max_v = std::max(c.max_v, dot(p, c.v) * tpu);
    }
  }
  // Shelf packing, tallest first, into a power-of-two wide atlas.
  double area = 0;
  int widest = 1;
  for (Chart& c : charts) {
    c.w = static_cast<int>(std::ceil(c.max_u - c.min_u - 1e-9)) + 2 * pad;
    c.h = static_cast<int>(std::ceil(c.max_v - c.min_v - 1e-9)) + 2 * pad;
    c.w = std::max(c.w, 1 + 2 * pad), c.h = std::max(c.h, 1 + 2 * pad);
    area += static_cast<double>(c.w) * c.h;
    widest = std::max(widest, c.w);
  }
  int W = 1;
  while (W < std::max<double>(widest, std::sqrt(area * 1.15))) W *= 2;
  std::vector<int> order(charts.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return charts[a].h > charts[b].h; });
  int x = 0, y = 0, shelf = 0;
  for (int i : order) {
    Chart& c = charts[i];
    if (x + c.w > W) x = 0, y += shelf, shelf = 0;
    c.x = x, c.y = y;
    x += c.w;
    shelf = std::max(shelf, c.h);
  }
  const int H = std::max(4, (y + shelf + 3) / 4 * 4);

  TexturedMesh out = m;
  out.positions.clear(), out.normals.clear(), out.uvs.clear(), out.indices.clear();
  out.uvs1.clear(), out.tangents.clear();
  std::map<std::pair<uint32_t, int>, uint32_t> vmap;
  for (size_t t = 0; t < T; ++t) {
    const Chart& c = charts[chart[t]];
    for (int k = 0; k < 3; ++k) {
      const uint32_t src = m.indices[3 * t + k];
      auto [it, added] = vmap.try_emplace({src, chart[t]}, static_cast<uint32_t>(out.positions.size()));
      if (added) {
        const vec3d p = m.positions[src];
        out.positions.push_back(p);
        out.normals.push_back(src < m.normals.size() ? m.normals[src] : normal[t]);
        out.uvs.push_back(src < m.uvs.size() ? m.uvs[src] : vec2d{});
        out.uvs1.push_back({(c.x + pad + dot(p, c.u) * tpu - c.min_u) / W, (c.y + pad + dot(p, c.v) * tpu - c.min_v) / H});
        out.tangents.push_back({c.u.x, c.u.y, c.u.z, 1.0});
      }
      out.indices.push_back(it->second);
    }
  }
  if (report) *report = UniqueUVReport{static_cast<int>(charts.size()), W, H};
  return out;
}

}  // namespace pixel3d
