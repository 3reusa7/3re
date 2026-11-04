#include "pixel3d/style/bake.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <numbers>

#include "pixel3d/style/raycast.hpp"

namespace pixel3d {
namespace {

// One texel of the unique UV atlas covered by a triangle.
struct Texel {
  int x, y;
  size_t tri;
  double b[3];
};

// Texels whose centre lies in a triangle (in uvs1, scaled to texels). Texels on
// a chart's border are taken by the first triangle that claims them.
std::vector<Texel> chart_texels(const TexturedMesh& m, int W, int H) {
  std::vector<Texel> out;
  std::vector<uint8_t> taken(static_cast<size_t>(W) * H, 0);
  const size_t T = m.indices.size() / 3;
  for (size_t t = 0; t < T; ++t) {
    vec2d q[3];
    for (int k = 0; k < 3; ++k) {
      const vec2d uv = m.uvs1[m.indices[3 * t + k]];
      q[k] = {uv.x * W, uv.y * H};
    }
    const double area = (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[1].y - q[0].y) * (q[2].x - q[0].x);
    if (std::abs(area) < 1e-12) continue;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({q[0].x, q[1].x, q[2].x}) - 0.5)));
    const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({q[0].x, q[1].x, q[2].x}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({q[0].y, q[1].y, q[2].y}) - 0.5)));
    const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({q[0].y, q[1].y, q[2].y}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double cx = x + 0.5, cy = y + 0.5;
        const double w0 = ((q[1].x - cx) * (q[2].y - cy) - (q[1].y - cy) * (q[2].x - cx)) / area;
        const double w1 = ((q[2].x - cx) * (q[0].y - cy) - (q[2].y - cy) * (q[0].x - cx)) / area;
        const double w2 = 1 - w0 - w1;
        if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
        uint8_t& tk = taken[static_cast<size_t>(y) * W + x];
        if (tk) continue;
        tk = 1;
        out.push_back(Texel{x, y, t, {w0, w1, w2}});
      }
  }
  return out;
}

// Texels no chart covers take the nearest covered texel's value (breadth first).
void dilate(ImageRGBA& img, std::vector<uint8_t>& set, Rgba8 fallback) {
  const int W = img.width(), H = img.height();
  std::deque<std::pair<int, int>> q;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (set[static_cast<size_t>(y) * W + x]) q.emplace_back(x, y);
  const int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
  while (!q.empty()) {
    const auto [x, y] = q.front();
    q.pop_front();
    for (int k = 0; k < 4; ++k) {
      const int nx = x + dx[k], ny = y + dy[k];
      if (nx < 0 || ny < 0 || nx >= W || ny >= H || set[static_cast<size_t>(ny) * W + nx]) continue;
      set[static_cast<size_t>(ny) * W + nx] = 1;
      img.at(nx, ny) = img.at(x, y);
      q.emplace_back(nx, ny);
    }
  }
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (!set[static_cast<size_t>(y) * W + x]) img.at(x, y) = fallback;
}

vec3d at(const TexturedMesh& m, const std::vector<vec3d>& v, const Texel& tx) {
  return v[m.indices[3 * tx.tri]] * tx.b[0] + v[m.indices[3 * tx.tri + 1]] * tx.b[1] + v[m.indices[3 * tx.tri + 2]] * tx.b[2];
}

vec3d face_normal(const TexturedMesh& m, size_t t) {
  return normalize(cross(m.positions[m.indices[3 * t + 1]] - m.positions[m.indices[3 * t]],
                         m.positions[m.indices[3 * t + 2]] - m.positions[m.indices[3 * t]]));
}

double radical_inverse(uint32_t i) {
  i = (i << 16u) | (i >> 16u);
  i = ((i & 0x55555555u) << 1u) | ((i & 0xAAAAAAAAu) >> 1u);
  i = ((i & 0x33333333u) << 2u) | ((i & 0xCCCCCCCCu) >> 2u);
  i = ((i & 0x0F0F0F0Fu) << 4u) | ((i & 0xF0F0F0F0u) >> 4u);
  i = ((i & 0x00FF00FFu) << 8u) | ((i & 0xFF00FF00u) >> 8u);
  return i * 2.3283064365386963e-10;
}

// Closest point to p on triangle abc (Ericson 5.1.5).
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

uint8_t byte(double v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255)); }

}  // namespace

ImageRGBA bake_ambient_occlusion(const TexturedMesh& m, int W, int H, const AOOptions& opt) {
  ImageRGBA img(std::max(W, 1), std::max(H, 1), Rgba8{255, 255, 255, 255});
  if (m.uvs1.size() != m.positions.size() || W <= 0 || H <= 0) return img;
  const TriangleBVH bvh(m.positions, m.indices);
  const int rays = std::max(1, opt.rays);
  std::vector<uint8_t> set(static_cast<size_t>(W) * H, 0);
  const bool smooth = m.normals.size() == m.positions.size();
  for (const Texel& tx : chart_texels(m, W, H)) {
    const vec3d nf = face_normal(m, tx.tri);
    vec3d n = smooth ? normalize(at(m, m.normals, tx)) : nf;
    if (length(n) == 0 || dot(n, nf) <= 0) n = nf;
    const vec3d p = at(m, m.positions, tx) + nf * 1e-4;
    const vec3d t = normalize(cross(std::abs(n.y) < 0.9 ? vec3d{0, 1, 0} : vec3d{1, 0, 0}, n)), b = cross(n, t);
    // Per-texel rotation of the stratified pattern (no visible banding).
    const double rot = radical_inverse(static_cast<uint32_t>(tx.x * 73856093u ^ tx.y * 19349663u));
    int hits = 0;
    for (int i = 0; i < rays; ++i) {
      const double u1 = (i + 0.5) / rays, u2 = radical_inverse(static_cast<uint32_t>(i)) + rot;
      const double r = std::sqrt(u1), phi = 2 * std::numbers::pi * u2;
      vec3d d = t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0, 1 - u1));
      if (dot(d, nf) < 0) d = d - nf * (2 * dot(d, nf));  // keep rays above the face
      if (bvh.occluded(p, normalize(d), 1e-6, opt.max_distance)) ++hits;
    }
    const uint8_t v = byte(1 - static_cast<double>(hits) / rays);
    img.at(tx.x, tx.y) = Rgba8{v, v, v, 255};
    set[static_cast<size_t>(tx.y) * W + tx.x] = 1;
  }
  dilate(img, set, Rgba8{255, 255, 255, 255});
  return img;
}

ImageRGBA bake_bevel_normals(const TexturedMesh& m, const Mesh& crisp, int W, int H, const BevelOptions& opt) {
  const Rgba8 flat{128, 128, 255, 255};
  ImageRGBA img(std::max(W, 1), std::max(H, 1), flat);
  if (m.uvs1.size() != m.positions.size() || m.tangents.size() != m.positions.size() || W <= 0 || H <= 0) return img;
  const double R = std::max(opt.radius, 1e-6);
  // Crisp planar faces: normal, a point on the plane, triangles, bounds.
  struct Region {
    vec3d n, origin, lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    std::vector<size_t> tris;
  };
  std::map<int, Region> regions;
  for (size_t t = 0; t < crisp.tris.size(); ++t) {
    const int id = t < crisp.tri_region.size() ? crisp.tri_region[t] : static_cast<int>(t);
    Region& r = regions[id];
    if (r.tris.empty()) r.n = normalize(tri_normal_raw(crisp, t)), r.origin = crisp.positions[crisp.tris[t][0]];
    r.tris.push_back(t);
    for (uint32_t v : crisp.tris[t]) {
      const vec3d p = crisp.positions[v];
      r.lo = {std::min(r.lo.x, p.x), std::min(r.lo.y, p.y), std::min(r.lo.z, p.z)};
      r.hi = {std::max(r.hi.x, p.x), std::max(r.hi.y, p.y), std::max(r.hi.z, p.z)};
    }
  }
  // Thickness of the crisp solid from p along -n: a fillet cannot be larger
  // than half of it on either side (a 2 px post gets at most a 1 px round).
  std::vector<uint32_t> crisp_idx;
  for (const auto& t : crisp.tris) crisp_idx.insert(crisp_idx.end(), t.begin(), t.end());
  const TriangleBVH bvh(crisp.positions, crisp_idx);
  auto thickness = [&](vec3d p, vec3d n) {
    const double t = bvh.hit(p - n * 1e-4, -n, 1e-6, 1e6);
    return t >= 0 ? t : 1e6;
  };
  std::vector<uint8_t> set(static_cast<size_t>(W) * H, 0);
  const bool smooth = m.normals.size() == m.positions.size();
  for (const Texel& tx : chart_texels(m, W, H)) {
    const vec3d nf = face_normal(m, tx.tri);
    vec3d N = smooth ? normalize(at(m, m.normals, tx)) : nf;
    if (length(N) == 0) N = nf;
    const vec3d p = at(m, m.positions, tx);
    const double self = thickness(p, nf);
    // Normal of a round fillet of radius R around every convex crisp edge near p:
    // the direction from the fillet's axis to p, Σ n_r (R − d_r) over the crisp
    // faces within R whose plane p lies behind (concave neighbours are in front).
    vec3d bent{};
    for (const auto& [id, r] : regions) {
      if (p.x < r.lo.x - R || p.y < r.lo.y - R || p.z < r.lo.z - R || p.x > r.hi.x + R || p.y > r.hi.y + R || p.z > r.hi.z + R)
        continue;
      if (dot(p - r.origin, r.n) > 1e-6) continue;
      double dmin = 1e300;
      for (size_t t : r.tris) {
        const auto& tri = crisp.tris[t];
        dmin = std::min(dmin, length(p - closest_on_triangle(p, crisp.positions[tri[0]], crisp.positions[tri[1]],
                                                               crisp.positions[tri[2]])));
      }
      if (dmin >= R) continue;
      const double r_eff = std::min({R, 0.5 * self, 0.5 * thickness(p, r.n)});
      if (dmin < r_eff) bent = bent + r.n * (r_eff - dmin);
    }
    vec3d n = length(bent) > 0 ? normalize(bent) : N;
    if (dot(n, N) < 0.05) n = normalize(n + N * (0.05 - dot(n, N) + 1e-3));
    // Tangent frame as a glTF viewer builds it: T orthogonalised against N, B = N × T · w.
    const auto& tg = m.tangents[m.indices[3 * tx.tri]];
    vec3d T{tg[0], tg[1], tg[2]};
    T = normalize(T - N * dot(T, N));
    const vec3d B = cross(N, T) * tg[3];
    const vec3d ts = normalize(vec3d{dot(n, T), dot(n, B), dot(n, N)});
    img.at(tx.x, tx.y) = Rgba8{byte((ts.x + 1) / 2), byte((ts.y + 1) / 2), byte((ts.z + 1) / 2), 255};
    set[static_cast<size_t>(tx.y) * W + tx.x] = 1;
  }
  dilate(img, set, flat);
  return img;
}

}  // namespace pixel3d
