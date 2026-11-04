#include "pixel3d/texture/transfer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

namespace pixel3d {
namespace {

// Closest point to p on triangle abc (Ericson, Real-Time Collision Detection 5.1.5).
vec3d closest_on_triangle(vec3d p, vec3d a, vec3d b, vec3d c) {
  const vec3d ab = b - a, ac = c - a, ap = p - a;
  const double d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return a;
  const vec3d bp = p - b;
  const double d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return b;
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
  const vec3d cp = p - c;
  const double d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return c;
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const double denom = 1 / (va + vb + vc);
  return a + ab * (vb * denom) + ac * (vc * denom);
}

// Barycentric coordinates of p's projection onto the plane of abc.
std::array<double, 3> plane_barycentric(vec3d p, vec3d a, vec3d b, vec3d c) {
  const vec3d v0 = b - a, v1 = c - a, v2 = p - a;
  const double d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
  const double den = d00 * d11 - d01 * d01;
  if (std::abs(den) < 1e-18) return {1, 0, 0};
  const double v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
  return {1 - v - w, v, w};
}

bool projected(const FaceTexture& f, const TexturedMesh& tm, size_t t) {
  if (f.mode == SurfaceMode::Seen || f.mode == SurfaceMode::OppositeProjection) return true;
  if (f.mode != SurfaceMode::Manual) return false;
  // A manual Flat Palette face has one UV for all three corners.
  const vec2d a = tm.uvs[tm.indices[3 * t]], b = tm.uvs[tm.indices[3 * t + 1]], c = tm.uvs[tm.indices[3 * t + 2]];
  return !(a == b && b == c);
}

}  // namespace

TextureResult transfer_textures(const Mesh& styled, const Mesh& crisp, const TextureResult& crisp_tex, Dims d) {
  TextureResult res;
  res.atlas = crisp_tex.atlas;
  res.messages = crisp_tex.messages;
  res.texel_density_min = crisp_tex.texel_density_min;
  res.texel_density_max = crisp_tex.texel_density_max;
  const size_t T = styled.tris.size(), C = crisp.tris.size();
  res.faces.resize(T);
  std::vector<vec3d> crisp_normal(C);
  for (size_t c = 0; c < C; ++c) crisp_normal[c] = normalize(tri_normal_raw(crisp, c));
  auto part = [](const Mesh& m, size_t t) { return t < m.tri_part.size() ? m.tri_part[t] : -1; };

  // Edge Extend stretches the texels of a seen edge. Crisp picks the edge per
  // triangle, so the two triangles of one flat face may stretch different
  // edges (a diagonal seam). Here every Edge Extend triangle of a planar region
  // follows the map of the region's largest one.
  std::map<int, size_t> stretch_of_region;
  for (size_t k = 0; k < C; ++k) {
    if (crisp_tex.faces[k].mode != SurfaceMode::EdgeExtend || k >= crisp.tri_region.size()) continue;
    auto [it, added] = stretch_of_region.try_emplace(crisp.tri_region[k], k);
    if (!added && length(tri_normal_raw(crisp, k)) > length(tri_normal_raw(crisp, it->second))) it->second = k;
  }

  TexturedMesh& tm = res.mesh;
  tm.texture = crisp_tex.mesh.texture;
  std::map<std::tuple<uint32_t, long long, long long, long long, long long, long long>, uint32_t> vmap;  // flat normals per face
  for (SurfaceMode m : {SurfaceMode::Seen, SurfaceMode::OppositeProjection, SurfaceMode::EdgeExtend,
                        SurfaceMode::FlatPalette, SurfaceMode::Manual})
    res.area_by_mode[m] = 0;
  for (size_t t = 0; t < T; ++t) {
    const vec3d a = styled.positions[styled.tris[t][0]], b = styled.positions[styled.tris[t][1]],
                c = styled.positions[styled.tris[t][2]];
    const vec3d centre = (a + b + c) / 3.0, n = normalize(cross(b - a, c - a));
    // Nearest crisp face of the same part facing the same way; ties go to the
    // better aligned face, then the lower index (deterministic).
    size_t best = C;
    double best_d = 1e300, best_align = -2;
    for (int pass = 0; pass < 2 && best == C; ++pass)
      for (size_t k = 0; k < C; ++k) {
        if (pass == 0 && part(crisp, k) != part(styled, t)) continue;
        const double align = dot(crisp_normal[k], n);
        if (align <= 1e-6) continue;
        const vec3d q = closest_on_triangle(centre, crisp.positions[crisp.tris[k][0]], crisp.positions[crisp.tris[k][1]],
                                            crisp.positions[crisp.tris[k][2]]);
        const double dist = length(q - centre);
        if (dist < best_d - 1e-9 || (std::abs(dist - best_d) <= 1e-9 && align > best_align + 1e-12)) {
          best = k, best_d = dist, best_align = align;
        }
      }
    if (best == C) best = 0;  // degenerate input: any face keeps the mesh renderable
    const FaceTexture& src = crisp_tex.faces[best];
    res.faces[t] = src;
    const bool proj = projected(src, crisp_tex.mesh, best);
    if (src.mode == SurfaceMode::EdgeExtend && best < crisp.tri_region.size()) best = stretch_of_region.at(crisp.tri_region[best]);
    const vec3d ca = crisp.positions[crisp.tris[best][0]], cb = crisp.positions[crisp.tris[best][1]],
                cc = crisp.positions[crisp.tris[best][2]];
    const vec2d uv0 = crisp_tex.mesh.uvs[crisp_tex.mesh.indices[3 * best]],
                uv1 = crisp_tex.mesh.uvs[crisp_tex.mesh.indices[3 * best + 1]],
                uv2 = crisp_tex.mesh.uvs[crisp_tex.mesh.indices[3 * best + 2]];
    for (int k = 0; k < 3; ++k) {
      const uint32_t vi = styled.tris[t][k];
      const vec3d p = styled.positions[vi];
      vec2d uv;
      if (proj) {
        const vec3d uvd = view_from_world(src.view, d, p);
        uv = res.atlas.uv(src.view, uvd.x, uvd.y);
      } else {
        const auto w = plane_barycentric(p, ca, cb, cc);
        uv = uv0 * w[0] + uv1 * w[1] + uv2 * w[2];
      }
      const auto key = std::make_tuple(vi, std::llround(uv.x * 1e9), std::llround(uv.y * 1e9), std::llround(n.x * 1e9),
                                       std::llround(n.y * 1e9), std::llround(n.z * 1e9));
      auto [it, added] = vmap.try_emplace(key, static_cast<uint32_t>(tm.positions.size()));
      if (added) {
        tm.positions.push_back(p);
        tm.normals.push_back(n);
        tm.uvs.push_back(uv);
      }
      tm.indices.push_back(it->second);
    }
    tm.tri_part.push_back(part(styled, t));
    res.area_by_mode[src.mode] += length(cross(b - a, c - a)) / 2;
  }
  return res;
}

}  // namespace pixel3d
