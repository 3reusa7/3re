#include "pixel3d/render/rasterizer.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace pixel3d {
namespace {

constexpr int64_t kSub = 256;  // sub-pixel steps per pixel

struct ScreenVert {
  int64_t x, y;  // fixed point
};

int64_t edge(const ScreenVert& a, const ScreenVert& b, int64_t px, int64_t py) {
  return (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
}

// For positive-area triangles (screen y down): top edges run right, left edges run up.
bool top_left(const ScreenVert& a, const ScreenVert& b) { return (a.y == b.y && b.x > a.x) || b.y < a.y; }

// Barycentrics are given in the caller's original vertex order.
using Fragment = std::function<void(int tri, int px, int py, const double bary[3])>;

void rasterize(size_t tri_count, const std::function<void(size_t, ScreenVert[3])>& fetch, int width, int height,
               const Fragment& frag) {
  for (size_t t = 0; t < tri_count; ++t) {
    ScreenVert v[3];
    fetch(t, v);
    int64_t area = edge(v[0], v[1], v[2].x, v[2].y);
    if (area == 0) continue;
    int order[3] = {0, 1, 2};
    if (area < 0) {
      std::swap(v[1], v[2]);
      std::swap(order[1], order[2]);
      area = -area;
    }
    const int64_t minx = std::min({v[0].x, v[1].x, v[2].x}), maxx = std::max({v[0].x, v[1].x, v[2].x});
    const int64_t miny = std::min({v[0].y, v[1].y, v[2].y}), maxy = std::max({v[0].y, v[1].y, v[2].y});
    const int x0 = std::max<int64_t>(0, minx / kSub - 1), x1 = std::min<int64_t>(width - 1, maxx / kSub + 1);
    const int y0 = std::max<int64_t>(0, miny / kSub - 1), y1 = std::min<int64_t>(height - 1, maxy / kSub + 1);
    const bool tl0 = top_left(v[1], v[2]), tl1 = top_left(v[2], v[0]), tl2 = top_left(v[0], v[1]);
    for (int py = y0; py <= y1; ++py)
      for (int px = x0; px <= x1; ++px) {
        const int64_t cx = px * kSub + kSub / 2, cy = py * kSub + kSub / 2;
        const int64_t w0 = edge(v[1], v[2], cx, cy), w1 = edge(v[2], v[0], cx, cy), w2 = edge(v[0], v[1], cx, cy);
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        if ((w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2)) continue;
        double b[3];
        b[order[0]] = static_cast<double>(w0) / area;
        b[order[1]] = static_cast<double>(w1) / area;
        b[order[2]] = static_cast<double>(w2) / area;
        frag(static_cast<int>(t), px, py, b);
      }
  }
}

ScreenVert to_screen(const Camera& cam, vec3d p) {
  const vec3d s = cam.project(p);
  return {std::llround(s.x * kSub), std::llround(s.y * kSub)};
}

RenderTarget make_target(const Camera& cam, Rgba8 bg) {
  RenderTarget rt;
  rt.color = ImageRGBA(cam.width, cam.height, bg);
  rt.depth.assign(static_cast<size_t>(cam.width) * cam.height, std::numeric_limits<float>::infinity());
  rt.coverage = Mask2D(cam.width, cam.height);
  rt.tri_id.assign(rt.depth.size(), -1);
  return rt;
}

}  // namespace

RenderTarget render(const TexturedMesh& m, const Camera& cam, const RenderOptions& opt) {
  RenderTarget rt = make_target(cam, opt.background);
  const size_t ntri = m.indices.size() / 3;
  std::vector<double> vdepth(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); ++i) vdepth[i] = cam.project(m.positions[i]).z;
  const vec3d light = normalize(opt.light_dir);
  rasterize(
      ntri,
      [&](size_t t, ScreenVert out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = to_screen(cam, m.positions[m.indices[3 * t + k]]);
      },
      cam.width, cam.height,
      [&](int t, int px, int py, const double b[3]) {
        const uint32_t i0 = m.indices[3 * t], i1 = m.indices[3 * t + 1], i2 = m.indices[3 * t + 2];
        const double depth = b[0] * vdepth[i0] + b[1] * vdepth[i1] + b[2] * vdepth[i2];
        const size_t idx = static_cast<size_t>(py) * cam.width + px;
        if (depth >= rt.depth[idx]) return;
        rt.depth[idx] = static_cast<float>(depth);
        rt.tri_id[idx] = t;
        rt.coverage.set(px, py, true);
        Rgba8 c{180, 180, 180, 255};
        if (opt.textured && m.texture && !m.uvs.empty()) {
          const vec2d uv = m.uvs[i0] * b[0] + m.uvs[i1] * b[1] + m.uvs[i2] * b[2];
          if (opt.smooth_texture) {
            const double x = uv.x * m.texture->width() - 0.5, y = uv.y * m.texture->height() - 0.5;
            const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
            const double fx = x - x0, fy = y - y0;
            double acc[3] = {0, 0, 0};
            for (int dy = 0; dy < 2; ++dy)
              for (int dx = 0; dx < 2; ++dx) {
                const Rgba8 s = m.texture->at(std::clamp(x0 + dx, 0, m.texture->width() - 1), std::clamp(y0 + dy, 0, m.texture->height() - 1));
                const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
                acc[0] += s.r * w;
                acc[1] += s.g * w;
                acc[2] += s.b * w;
              }
            c = Rgba8{static_cast<uint8_t>(std::lround(acc[0])), static_cast<uint8_t>(std::lround(acc[1])), static_cast<uint8_t>(std::lround(acc[2])), 255};
          } else {
            const int tx = std::clamp(static_cast<int>(std::floor(uv.x * m.texture->width())), 0, m.texture->width() - 1);
            const int ty = std::clamp(static_cast<int>(std::floor(uv.y * m.texture->height())), 0, m.texture->height() - 1);
            c = m.texture->at(tx, ty);
            c.a = 255;
          }
        }
        if (opt.lit) {
          const vec3d n = normalize(cross(m.positions[i1] - m.positions[i0], m.positions[i2] - m.positions[i0]));
          const double k = 0.55 + 0.45 * std::max(0.0, dot(n, light));
          c = Rgba8{static_cast<uint8_t>(std::lround(c.r * k)), static_cast<uint8_t>(std::lround(c.g * k)),
                    static_cast<uint8_t>(std::lround(c.b * k)), 255};
        }
        rt.color.at(px, py) = c;
      });
  return rt;
}

RenderTarget render_mesh_flat(const Mesh& m, const Camera& cam) {
  RenderTarget rt = make_target(cam, Rgba8{0, 0, 0, 0});
  std::vector<double> vdepth(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); ++i) vdepth[i] = cam.project(m.positions[i]).z;
  rasterize(
      m.tris.size(),
      [&](size_t t, ScreenVert out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = to_screen(cam, m.positions[m.tris[t][k]]);
      },
      cam.width, cam.height,
      [&](int t, int px, int py, const double b[3]) {
        const auto& tri = m.tris[t];
        const double depth = b[0] * vdepth[tri[0]] + b[1] * vdepth[tri[1]] + b[2] * vdepth[tri[2]];
        const size_t idx = static_cast<size_t>(py) * cam.width + px;
        if (depth >= rt.depth[idx]) return;
        rt.depth[idx] = static_cast<float>(depth);
        rt.tri_id[idx] = t;
        rt.coverage.set(px, py, true);
        rt.color.at(px, py) = Rgba8{180, 180, 180, 255};
      });
  return rt;
}

}  // namespace pixel3d
