#include "pixel3d/render/beauty.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace pixel3d {

std::string_view to_string(RenderStyle s) { return s == RenderStyle::Beauty ? "beauty" : "validation"; }

std::optional<RenderStyle> render_style_from_string(std::string_view s) {
  if (s == "validation") return RenderStyle::Validation;
  if (s == "beauty") return RenderStyle::Beauty;
  return std::nullopt;
}

namespace {

struct Rgb {
  double r = 0, g = 0, b = 0;
  friend Rgb operator+(Rgb a, Rgb b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
  friend Rgb operator*(Rgb a, double s) { return {a.r * s, a.g * s, a.b * s}; }
  friend Rgb operator*(Rgb a, Rgb b) { return {a.r * b.r, a.g * b.g, a.b * b.b}; }
};

constexpr vec3d kUp{0, 1, 0};

double to_linear(double srgb) { return std::pow(std::clamp(srgb, 0.0, 1.0), 2.2); }
uint8_t to_byte(double linear) {
  return static_cast<uint8_t>(std::lround(std::pow(std::clamp(linear, 0.0, 1.0), 1 / 2.2) * 255));
}
Rgb srgb(double r, double g, double b) { return {to_linear(r), to_linear(g), to_linear(b)}; }

// Linear up to 0.8, then a soft shoulder towards 1: mid tones keep the drawn
// palette colour, highlights roll off instead of clipping.
double filmic(double x) {
  constexpr double t = 0.8;
  x = std::max(0.0, x);
  return x <= t ? x : t + (1 - t) * (1 - std::exp(-(x - t) / (1 - t)));
}

// Pixel-centre coverage of one triangle in floating-point screen space;
// frag(px, py, w0, w1, w2) gets screen-space barycentrics. Edge samples go to
// both neighbours (the depth test picks one), so shared edges never leave gaps.
template <class Frag>
void raster_tri(const vec2d s[3], int w, int h, Frag&& frag) {
  const double area = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[1].y - s[0].y) * (s[2].x - s[0].x);
  if (std::abs(area) < 1e-12) return;
  const double minx = std::min({s[0].x, s[1].x, s[2].x}), maxx = std::max({s[0].x, s[1].x, s[2].x});
  const double miny = std::min({s[0].y, s[1].y, s[2].y}), maxy = std::max({s[0].y, s[1].y, s[2].y});
  const int x0 = std::max(0, static_cast<int>(std::floor(minx - 0.5)));
  const int x1 = std::min(w - 1, static_cast<int>(std::ceil(maxx - 0.5)));
  const int y0 = std::max(0, static_cast<int>(std::floor(miny - 0.5)));
  const int y1 = std::min(h - 1, static_cast<int>(std::ceil(maxy - 0.5)));
  constexpr double eps = -1e-9;
  for (int py = y0; py <= y1; ++py)
    for (int px = x0; px <= x1; ++px) {
      const double cx = px + 0.5, cy = py + 0.5;
      const double w0 = ((s[2].x - s[1].x) * (cy - s[1].y) - (s[2].y - s[1].y) * (cx - s[1].x)) / area;
      const double w1 = ((s[0].x - s[2].x) * (cy - s[2].y) - (s[0].y - s[2].y) * (cx - s[2].x)) / area;
      const double w2 = ((s[1].x - s[0].x) * (cy - s[0].y) - (s[1].y - s[0].y) * (cx - s[0].x)) / area;
      if (w0 < eps || w1 < eps || w2 < eps) continue;
      frag(px, py, w0, w1, w2);
    }
}

// Deterministic Vogel spiral: well spread filter taps in the unit disc.
const std::array<vec2d, 16>& disc_taps() {
  static const std::array<vec2d, 16> taps = [] {
    std::array<vec2d, 16> t{};
    for (size_t i = 0; i < t.size(); ++i) {
      const double r = std::sqrt((i + 0.5) / t.size()), a = i * 2.399963229728653;
      t[i] = {r * std::cos(a), r * std::sin(a)};
    }
    return t;
  }();
  return taps;
}

// Orthographic depth map along a light direction; lit() is a soft
// percentage-closer lookup (fraction of filter taps that see the light).
class ShadowMap {
 public:
  ShadowMap(const TexturedMesh& m, vec3d travel, int size, vec3d lo, vec3d hi, double pad) : f_(normalize(travel)), n_(size) {
    r_ = normalize(cross(f_, std::abs(f_.y) < 0.99 ? kUp : vec3d{0, 0, 1}));
    u_ = cross(r_, f_);
    double ax = 1e300, ay = 1e300, bx = -1e300, by = -1e300;
    for (int k = 0; k < 8; ++k) {
      const vec3d c{k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z};
      ax = std::min(ax, dot(c, r_)), bx = std::max(bx, dot(c, r_));
      ay = std::min(ay, dot(c, u_)), by = std::max(by, dot(c, u_));
    }
    ox_ = ax - pad, oy_ = ay - pad;
    scale_ = n_ / std::max({bx - ax + 2 * pad, by - ay + 2 * pad, 1e-9});
    depth_.assign(static_cast<size_t>(n_) * n_, std::numeric_limits<float>::infinity());
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
      vec2d s[3];
      double z[3];
      for (int k = 0; k < 3; ++k) {
        const vec3d p = m.positions[m.indices[t + k]];
        s[k] = {(dot(p, r_) - ox_) * scale_, (dot(p, u_) - oy_) * scale_};
        z[k] = dot(p, f_);
      }
      raster_tri(s, n_, n_, [&](int px, int py, double w0, double w1, double w2) {
        float& d = depth_[static_cast<size_t>(py) * n_ + px];
        d = std::min(d, static_cast<float>(w0 * z[0] + w1 * z[1] + w2 * z[2]));
      });
    }
  }

  double lit(vec3d p, double radius, double bias) const {
    const double x = (dot(p, r_) - ox_) * scale_, y = (dot(p, u_) - oy_) * scale_, z = dot(p, f_) - bias;
    int seen = 0;
    for (const vec2d& t : disc_taps()) {
      const int px = static_cast<int>(std::floor(x + t.x * radius * scale_));
      const int py = static_cast<int>(std::floor(y + t.y * radius * scale_));
      if (px < 0 || py < 0 || px >= n_ || py >= n_ || z <= depth_[static_cast<size_t>(py) * n_ + px]) ++seen;
    }
    return static_cast<double>(seen) / disc_taps().size();
  }

 private:
  vec3d f_, r_, u_;
  int n_;
  double ox_ = 0, oy_ = 0, scale_ = 1;
  std::vector<float> depth_;
};

// Bilinear sample of an 8-bit map at uv (glTF convention, clamped), channels 0–1.
vec3d sample_map(const ImageRGBA& img, vec2d uv) {
  const double x = std::clamp(uv.x * img.width() - 0.5, 0.0, img.width() - 1.0);
  const double y = std::clamp(uv.y * img.height() - 0.5, 0.0, img.height() - 1.0);
  const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
  const int x1 = std::min(x0 + 1, img.width() - 1), y1 = std::min(y0 + 1, img.height() - 1);
  const double fx = x - x0, fy = y - y0;
  auto px = [&](int a, int b) {
    const Rgba8 c = img.at(a, b);
    return vec3d{c.r / 255.0, c.g / 255.0, c.b / 255.0};
  };
  return (px(x0, y0) * (1 - fx) + px(x1, y0) * fx) * (1 - fy) + (px(x0, y1) * (1 - fx) + px(x1, y1) * fx) * fy;
}

}  // namespace

ImageRGBA render_beauty(const TexturedMesh& m, double yaw_deg, double elevation_deg, const BeautyOptions& opt) {
  const int ss = std::clamp(opt.supersample, 1, 4);
  const int W = std::max(1, opt.width) * ss, H = std::max(1, opt.height) * ss;

  vec3d lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  for (const vec3d& p : m.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const bool empty = m.indices.size() < 3;
  if (empty) lo = hi = vec3d{};
  const vec3d centre = (lo + hi) * 0.5;
  const double radius = std::max(length(hi - lo) / 2, 1e-6);

  // Camera frame (same angles as camera_orbit). The right axis stays horizontal,
  // so straight-down views keep the front of the model at the bottom.
  const double yaw = yaw_deg * std::numbers::pi / 180, el = elevation_deg * std::numbers::pi / 180;
  const vec3d to_cam{std::sin(yaw) * std::cos(el), std::sin(el), std::cos(yaw) * std::cos(el)};
  const vec3d fwd = -to_cam, right{std::cos(yaw), 0, -std::sin(yaw)}, up = cross(right, fwd);
  const double half_fov = std::clamp(opt.fov_deg, 5.0, 120.0) * std::numbers::pi / 360;
  const vec3d eye = centre + to_cam * (radius / std::sin(half_fov) * 1.1);
  const double focal = std::min(W, H) / 2.0 / std::tan(half_fov);

  // Studio lights, placed relative to the camera: warm key upper left, cool fill
  // right, rim from behind. Directions point towards the light.
  const vec3d toward{std::sin(yaw), 0, std::cos(yaw)};
  const vec3d key = normalize(right * -0.6 + kUp * 1.1 + toward * 0.75);
  const vec3d fill = normalize(right * 0.9 + kUp * 0.3 + toward * 0.6);
  const vec3d rim = normalize(right * 0.35 + kUp * 0.8 - toward * 1.0);
  const Rgb key_col{1.0, 0.94, 0.86}, fill_col{0.72, 0.82, 1.0}, rim_col{1.0, 1.0, 1.0};
  const Rgb sky{0.62, 0.68, 0.80}, bounce{0.40, 0.35, 0.31};

  const double pad = radius * 1.5;
  std::optional<ShadowMap> key_map, down_map;
  if (opt.shadows && !empty) key_map.emplace(m, -key, 1536, lo, hi, pad);
  if (opt.ground && !empty) down_map.emplace(m, -kUp, 512, lo, hi, pad);

  // G-buffer: nearest triangle and perspective-correct barycentrics per sample.
  const size_t N = static_cast<size_t>(W) * H;
  std::vector<float> zbuf(N, std::numeric_limits<float>::infinity());
  std::vector<int> tri(N, -1);
  std::vector<std::array<float, 3>> bary(N);
  std::vector<vec2d> screen(m.positions.size());
  std::vector<double> inv_z(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const vec3d q = m.positions[i] - eye;
    const double z = std::max(dot(q, fwd), 1e-6);
    inv_z[i] = 1 / z;
    screen[i] = {W / 2.0 + dot(q, right) / z * focal, H / 2.0 - dot(q, up) / z * focal};
  }
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const uint32_t i[3] = {m.indices[t], m.indices[t + 1], m.indices[t + 2]};
    const vec2d s[3] = {screen[i[0]], screen[i[1]], screen[i[2]]};
    raster_tri(s, W, H, [&](int px, int py, double w0, double w1, double w2) {
      const double iz = w0 * inv_z[i[0]] + w1 * inv_z[i[1]] + w2 * inv_z[i[2]];
      const size_t idx = static_cast<size_t>(py) * W + px;
      const float z = static_cast<float>(1 / iz);
      if (z >= zbuf[idx]) return;
      zbuf[idx] = z;
      tri[idx] = static_cast<int>(t / 3);
      bary[idx] = {static_cast<float>(w0 * inv_z[i[0]] / iz), static_cast<float>(w1 * inv_z[i[1]] / iz),
                   static_cast<float>(w2 * inv_z[i[2]] / iz)};
    });
  }

  const Rgb bg_top = srgb(0.30, 0.32, 0.36), bg_bottom = srgb(0.15, 0.16, 0.18);
  const bool smooth = m.normals.size() == m.positions.size();
  const double exposure = 0.5;  // a face turned to the camera shows about its drawn colour
  std::vector<Rgb> col(N);
  std::vector<double> alpha(N, 0);
  for (int py = 0; py < H; ++py) {
    const Rgb bg = bg_top * (1 - (py + 0.5) / H) + bg_bottom * ((py + 0.5) / H);
    for (int px = 0; px < W; ++px) {
      const size_t idx = static_cast<size_t>(py) * W + px;
      if (tri[idx] >= 0) {
        const size_t t = static_cast<size_t>(tri[idx]) * 3;
        const uint32_t i0 = m.indices[t], i1 = m.indices[t + 1], i2 = m.indices[t + 2];
        const auto& b = bary[idx];
        const vec3d p = m.positions[i0] * b[0] + m.positions[i1] * b[1] + m.positions[i2] * b[2];
        const vec3d face = normalize(cross(m.positions[i1] - m.positions[i0], m.positions[i2] - m.positions[i0]));
        vec3d n = smooth ? normalize(m.normals[i0] * b[0] + m.normals[i1] * b[1] + m.normals[i2] * b[2]) : face;
        if (length(n) == 0) n = face;
        const bool has_uv1 = m.uvs1.size() == m.positions.size();
        const vec2d uv1 = has_uv1 ? m.uvs1[i0] * b[0] + m.uvs1[i1] * b[1] + m.uvs1[i2] * b[2] : vec2d{};
        if (has_uv1 && m.normal_map && m.tangents.size() == m.positions.size()) {
          // Tangent-space normal map, frame as glTF viewers build it.
          const auto& tg = m.tangents[i0];
          vec3d T{tg[0], tg[1], tg[2]};
          T = normalize(T - n * dot(T, n));
          const vec3d B = cross(n, T) * tg[3];
          const vec3d ts = sample_map(*m.normal_map, uv1) * 2.0 - vec3d{1, 1, 1};
          const vec3d bent = normalize(T * ts.x + B * ts.y + n * ts.z);
          if (length(bent) > 0) n = bent;
        }
        const double occlusion = has_uv1 && m.occlusion ? sample_map(*m.occlusion, uv1).x : 1.0;
        Rgb albedo = srgb(0.72, 0.72, 0.72);
        if (m.texture && m.uvs.size() == m.positions.size()) {
          const vec2d uv = m.uvs[i0] * b[0] + m.uvs[i1] * b[1] + m.uvs[i2] * b[2];
          const ImageRGBA& tex = *m.texture;
          const int tx = std::clamp(static_cast<int>(std::floor(uv.x * tex.width())), 0, tex.width() - 1);
          const int ty = std::clamp(static_cast<int>(std::floor(uv.y * tex.height())), 0, tex.height() - 1);
          const Rgba8 c = tex.at(tx, ty);
          albedo = srgb(c.r / 255.0, c.g / 255.0, c.b / 255.0);
        }
        const vec3d v = normalize(eye - p);
        const double shadow = key_map ? key_map->lit(p + face * 0.08, 0.3, 0.06) : 1.0;
        const double ndk = std::max(0.0, dot(n, key));
        const double fres = std::pow(1 - std::clamp(dot(n, v), 0.0, 1.0), 3);
        const Rgb hemi = bounce * (0.5 - 0.5 * n.y) + sky * (0.5 + 0.5 * n.y);
        const Rgb light = key_col * (2.3 * ndk * shadow) + fill_col * (0.5 * occlusion * std::max(0.0, dot(n, fill))) +
                          hemi * (0.55 * occlusion) +
                          rim_col * (1.3 * std::max(0.0, dot(n, rim)) * (0.35 + 0.65 * fres));
        const double spec = ndk > 0 ? 0.12 * shadow * std::pow(std::max(0.0, dot(n, normalize(key + v))), 32) : 0.0;
        const Rgb c = albedo * light + Rgb{spec, spec, spec};
        col[idx] = {filmic(c.r * exposure), filmic(c.g * exposure), filmic(c.b * exposure)};
        alpha[idx] = 1;
        continue;
      }
      double dark = 0;
      if (down_map) {
        const vec3d d = normalize(right * ((px + 0.5 - W / 2.0) / focal) + up * (-(py + 0.5 - H / 2.0) / focal) + fwd);
        const double tg = d.y < -1e-9 ? (lo.y - eye.y) / d.y : -1;
        if (tg > 0) {
          const vec3d g = eye + d * tg;
          const double cast = key_map ? 1 - key_map->lit(g, 0.9, 0.08) : 0;
          const double contact = 1 - down_map->lit(g, 2.5, 0.08);
          const vec3d off = g - centre;
          const double fade = std::pow(std::clamp(1 - std::hypot(off.x, off.z) / (radius * 2.5), 0.0, 1.0), 2);
          dark = std::min(0.8, (0.55 * cast + 0.4 * contact) * fade);
        }
      }
      if (opt.transparent) {
        col[idx] = Rgb{};
        alpha[idx] = dark;
      } else {
        col[idx] = bg * (1 - dark);
        alpha[idx] = 1;
      }
    }
  }

  // Box-filter the samples (premultiplied, linear light) into output pixels.
  ImageRGBA out(W / ss, H / ss);
  for (int y = 0; y < out.height(); ++y)
    for (int x = 0; x < out.width(); ++x) {
      Rgb acc;
      double a = 0;
      for (int sy = 0; sy < ss; ++sy)
        for (int sx = 0; sx < ss; ++sx) {
          const size_t idx = static_cast<size_t>(y * ss + sy) * W + (x * ss + sx);
          acc = acc + col[idx] * alpha[idx];
          a += alpha[idx];
        }
      const Rgb c = a > 0 ? acc * (1 / a) : Rgb{};
      out.at(x, y) = Rgba8{to_byte(c.r), to_byte(c.g), to_byte(c.b),
                           static_cast<uint8_t>(std::lround(a / (ss * ss) * 255))};
    }
  return out;
}

}  // namespace pixel3d
