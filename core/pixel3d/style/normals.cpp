#include "pixel3d/style/normals.hpp"

#include <array>
#include <cmath>
#include <map>
#include <numbers>
#include <tuple>

namespace pixel3d {

TexturedMesh smooth_normals(const TexturedMesh& m, const NormalOptions& opt, NormalReport* report) {
  NormalReport rep;
  const size_t nt = m.indices.size() / 3;
  // Corners meet at a position, whatever their UV seams say.
  std::map<std::array<double, 3>, uint32_t> id_of;
  std::vector<uint32_t> pos_id(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const vec3d p = m.positions[i];
    pos_id[i] = id_of.emplace(std::array<double, 3>{p.x, p.y, p.z}, static_cast<uint32_t>(id_of.size())).first->second;
  }
  std::vector<vec3d> face(nt);
  std::vector<std::array<double, 3>> angle(nt);
  std::vector<std::vector<std::pair<uint32_t, int>>> corners(id_of.size());
  for (size_t t = 0; t < nt; ++t) {
    const vec3d p[3] = {m.positions[m.indices[3 * t]], m.positions[m.indices[3 * t + 1]], m.positions[m.indices[3 * t + 2]]};
    face[t] = normalize(cross(p[1] - p[0], p[2] - p[0]));
    for (int k = 0; k < 3; ++k) {
      const vec3d a = normalize(p[(k + 1) % 3] - p[k]), b = normalize(p[(k + 2) % 3] - p[k]);
      angle[t][k] = std::acos(std::clamp(dot(a, b), -1.0, 1.0));
      corners[pos_id[m.indices[3 * t + k]]].push_back({static_cast<uint32_t>(t), k});
    }
  }
  auto part = [&](size_t t) { return t < m.tri_part.size() ? m.tri_part[t] : -1; };
  const double cos_limit = std::cos(std::clamp(opt.smooth_angle_deg, 0.0, 180.0) * std::numbers::pi / 180) - 1e-12;

  // Edge statistics (each undirected edge between two faces).
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
  for (size_t t = 0; t < nt; ++t)
    for (int k = 0; k < 3; ++k) {
      uint32_t a = pos_id[m.indices[3 * t + k]], b = pos_id[m.indices[3 * t + (k + 1) % 3]];
      if (a > b) std::swap(a, b);
      edges[{a, b}].push_back(static_cast<uint32_t>(t));
    }
  for (const auto& [e, ts] : edges) {
    if (ts.size() != 2) continue;
    if (part(ts[0]) != part(ts[1]))
      ++rep.part_edges;
    else if (dot(face[ts[0]], face[ts[1]]) < cos_limit)
      ++rep.hard_edges;
  }

  TexturedMesh out;
  out.texture = m.texture;
  out.tri_part = m.tri_part;
  out.indices.resize(m.indices.size());
  std::map<std::tuple<uint32_t, double, double, int64_t, int64_t, int64_t>, uint32_t> vertex_of;
  for (size_t t = 0; t < nt; ++t)
    for (int k = 0; k < 3; ++k) {
      const uint32_t src = m.indices[3 * t + k];
      vec3d n{};
      for (const auto& [t2, k2] : corners[pos_id[src]])
        if (part(t2) == part(t) && dot(face[t], face[t2]) >= cos_limit) n = n + face[t2] * angle[t2][k2];
      n = length(n) > 0 ? normalize(n) : face[t];
      const vec2d uv = src < m.uvs.size() ? m.uvs[src] : vec2d{};
      auto q = [](double v) { return static_cast<int64_t>(std::llround(v * 1e9)); };
      const auto key = std::make_tuple(pos_id[src], uv.x, uv.y, q(n.x), q(n.y), q(n.z));
      auto [it, added] = vertex_of.emplace(key, static_cast<uint32_t>(out.positions.size()));
      if (added) {
        out.positions.push_back(m.positions[src]);
        out.normals.push_back(n);
        out.uvs.push_back(uv);
      }
      out.indices[3 * t + k] = it->second;
    }
  rep.split_vertices = static_cast<int>(out.positions.size()) - static_cast<int>(id_of.size());
  if (report) *report = rep;
  return out;
}

}  // namespace pixel3d
