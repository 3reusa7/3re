#include "pixel3d/lowpoly/style_render.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace pixel3d::lowpoly {
namespace {

constexpr Rgba8 kShellColours[] = {{214, 120, 96, 255}, {110, 160, 210, 255}, {140, 196, 120, 255}, {222, 190, 100, 255},
                                   {170, 130, 200, 255}, {120, 200, 190, 255}, {220, 150, 180, 255}, {160, 160, 160, 255}};

struct Frag {
  float depth = std::numeric_limits<float>::infinity();
  int tri = -1;
  float b1 = 0, b2 = 0;  // barycentrics of vertices 1 and 2
};

}  // namespace

double cel_factor(double ndl, int tones) {
  if (tones <= 2) return ndl > 0.15 ? 1.0 : 0.68;
  return ndl > 0.55 ? 1.0 : ndl > 0.05 ? 0.82 : 0.64;
}

ImageRGBA render_style(const SkinnedMesh& m, const ImageRGBA* tex, const std::vector<vec3d>* pos_override,
                       const std::vector<vec3d>* nrm_override, double yaw_deg, double el_deg, vec3d lo, vec3d hi,
                       const StyleRenderOptions& opt) {
  const std::vector<vec3d>& P = pos_override ? *pos_override : m.positions;
  const std::vector<vec3d>& Nv = nrm_override ? *nrm_override : m.normals;
  const int ss = std::clamp(opt.supersample, 1, 4);
  const int W = opt.width * ss, H = opt.height * ss;
  // Orthographic orbit camera fitting the bounding sphere of [lo, hi].
  const double yaw = yaw_deg * std::numbers::pi / 180, el = el_deg * std::numbers::pi / 180;
  const vec3d to_cam{std::sin(yaw) * std::cos(el), std::sin(el), std::cos(yaw) * std::cos(el)};
  const vec3d fwd = -to_cam, right = normalize(cross(fwd, vec3d{0, 1, 0})), up = cross(right, fwd);
  const vec3d centre = (lo + hi) * 0.5;
  // A turntable spins about +Y: fit the half height and the horizontal radius.
  const vec3d h = (hi - lo) * 0.5;
  const double radius = std::max(1e-9, std::max(h.y + std::abs(std::sin(el)) * std::hypot(h.x, h.z), std::hypot(h.x, h.z)));
  const double scale = std::min(W, H) * (0.5 - opt.margin) / radius;
  auto project = [&](vec3d p) {
    const vec3d d = p - centre;
    return vec3d{W / 2.0 + dot(d, right) * scale, H / 2.0 - dot(d, up) * scale, dot(d, fwd)};
  };
  const vec3d L = normalize(right * opt.light_cam.x + up * opt.light_cam.y + to_cam * opt.light_cam.z);

  std::vector<Frag> fb(static_cast<size_t>(W) * H);
  std::vector<vec3d> sp(P.size());
  for (size_t i = 0; i < P.size(); ++i) sp[i] = project(P[i]);
  const size_t nt = m.indices.size() / 3;
  for (size_t t = 0; t < nt; ++t) {
    const vec3d a = sp[m.indices[3 * t]], b = sp[m.indices[3 * t + 1]], c = sp[m.indices[3 * t + 2]];
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    // Screen y points down: front faces (CCW in world) have negative area here.
    const bool hull = m.hull_appended && 3 * t >= m.hull_first_index;
    if (hull ? area <= 0 : area >= 0) continue;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
        const double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
        const double w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const float z = static_cast<float>(w0 * a.z + w1 * b.z + w2 * c.z);
        Frag& f = fb[static_cast<size_t>(y) * W + x];
        if (z < f.depth) f = {z, static_cast<int>(t), static_cast<float>(w1), static_cast<float>(w2)};
      }
  }
  // Shading.
  ImageRGBA big(W, H, opt.background);
  const bool lit = opt.style == Style::Cel || opt.style == Style::CelOutline;
  const bool ink = opt.style == Style::Outline || opt.style == Style::CelOutline;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const Frag& f = fb[static_cast<size_t>(y) * W + x];
      if (f.tri < 0) continue;
      const size_t t = static_cast<size_t>(f.tri);
      const bool hull = m.hull_appended && 3 * t >= m.hull_first_index;
      if (hull) {
        big.at(x, y) = opt.outline_color;
        continue;
      }
      const uint32_t i0 = m.indices[3 * t], i1 = m.indices[3 * t + 1], i2 = m.indices[3 * t + 2];
      const double b0 = 1 - f.b1 - f.b2;
      Rgba8 albedo;
      if (tex && !m.uvs.empty()) {
        const vec2d uv = m.uvs[i0] * b0 + m.uvs[i1] * f.b1 + m.uvs[i2] * f.b2;
        const int tx = std::clamp(static_cast<int>(std::floor(uv.x * tex->width())), 0, tex->width() - 1);
        const int ty = std::clamp(static_cast<int>(std::floor(uv.y * tex->height())), 0, tex->height() - 1);
        albedo = tex->at(tx, ty);
      } else {
        const int s = m.tri_shell.empty() ? 0 : m.tri_shell[t];
        albedo = kShellColours[static_cast<size_t>(s) % std::size(kShellColours)];
      }
      double k = 1;
      if (lit) {
        const vec3d n = normalize(Nv[i0] * b0 + Nv[i1] * f.b1 + Nv[i2] * f.b2);
        k = cel_factor(dot(n, L), opt.tones);
      }
      auto ch = [&](uint8_t v) { return static_cast<uint8_t>(std::clamp(std::lround(v * k), 0L, 255L)); };
      big.at(x, y) = {ch(albedo.r), ch(albedo.g), ch(albedo.b), 255};
    }
  // Screen-space ink: silhouette and depth/shell discontinuities.
  if (ink || opt.show_wire) {
    const int r = std::max(1, static_cast<int>(std::lround(opt.outline_px * ss)));
    const float jump = static_cast<float>(1.5 * scale);  // 1.5 world units of depth
    std::vector<uint8_t> edge(static_cast<size_t>(W) * H, 0);
    auto shell_of = [&](const Frag& f) { return f.tri < 0 || m.tri_shell.empty() ? -1 : m.tri_shell[static_cast<size_t>(f.tri)]; };
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const Frag& f = fb[static_cast<size_t>(y) * W + x];
        bool e = false;
        for (int dy = -r; dy <= r && !e; ++dy)
          for (int dx = -r; dx <= r && !e; ++dx) {
            if (dx * dx + dy * dy > r * r) continue;
            const int xx = x + dx, yy = y + dy;
            const Frag* g = (xx < 0 || yy < 0 || xx >= W || yy >= H) ? nullptr : &fb[static_cast<size_t>(yy) * W + xx];
            const bool fe = f.tri < 0, ge = !g || g->tri < 0;
            if (fe != ge) {
              e = !fe;  // ink on the model side of the silhouette
            } else if (!fe && !ge) {
              // The nearer surface keeps its colour; the line goes on the farther side.
              if (f.depth > g->depth + jump) e = true;
              else if (shell_of(f) != shell_of(*g) && f.depth > g->depth + 0.25f * jump) e = true;
            }
          }
        if (e && ink) edge[static_cast<size_t>(y) * W + x] = 1;
      }
    if (opt.show_wire)
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          const Frag& f = fb[static_cast<size_t>(y) * W + x];
          if (f.tri < 0) continue;
          if (std::min({f.b1, f.b2, 1 - f.b1 - f.b2}) < 0.012f * ss) edge[static_cast<size_t>(y) * W + x] = 2;
        }
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const uint8_t e = edge[static_cast<size_t>(y) * W + x];
        if (e == 1) big.at(x, y) = opt.outline_color;
        else if (e == 2) {
          Rgba8& c = big.at(x, y);
          c = {static_cast<uint8_t>(c.r / 2), static_cast<uint8_t>(c.g / 2), static_cast<uint8_t>(c.b / 2), 255};
        }
      }
  }
  // Box downsample.
  ImageRGBA out(opt.width, opt.height);
  for (int y = 0; y < opt.height; ++y)
    for (int x = 0; x < opt.width; ++x) {
      int r = 0, g = 0, b = 0;
      for (int dy = 0; dy < ss; ++dy)
        for (int dx = 0; dx < ss; ++dx) {
          const Rgba8 c = big.at(x * ss + dx, y * ss + dy);
          r += c.r, g += c.g, b += c.b;
        }
      const int n = ss * ss;
      out.at(x, y) = {static_cast<uint8_t>(r / n), static_cast<uint8_t>(g / n), static_cast<uint8_t>(b / n), 255};
    }
  return out;
}

}  // namespace pixel3d::lowpoly
