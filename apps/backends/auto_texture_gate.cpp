// Hidden-side quality gate (spec 2026-10-20-hybrid-texture-design §6). The older checks measure the
// texture where the picture shows the surface; this one looks at what the picture does NOT show: the
// back, the sides and the back three-quarters of the registered camera, rendered unlit (base colour only)
// together with a UV-chart-id buffer. Measures only; never edits.
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include "auto_texture_internal.hpp"

namespace p3dasset::autotex {
using namespace pixel3d;
using namespace detail;
namespace detail {
Frame render_ids(const Mesh& geo, const TexturedMesh& m, double yaw, double elevation, vec3d centre, double radius, int n) {
  const Pose p = Pose::make(yaw, elevation);
  Frame fr;
  fr.n = n;
  fr.face.assign(size_t(n) * n, -1);
  fr.depth.assign(size_t(n) * n, std::numeric_limits<float>::infinity());
  fr.lab.assign(size_t(n) * n, {0, 0, 0});
  fr.pos.assign(size_t(n) * n, {0, 0, 0});
  const double s = 0.46 * n / radius;
  fr.pixel_world = 1 / s;
  std::vector<vec2d> q(geo.positions.size());
  std::vector<double> d(geo.positions.size());
  for (size_t i = 0; i < q.size(); ++i) {
    const vec3d r = geo.positions[i] - centre;
    q[i] = {0.5 * n + s * dot(r, p.right), 0.5 * n - s * dot(r, p.up)};
    d[i] = dot(r, p.forward);
  }
  std::vector<std::array<float, 3>> bary(size_t(n) * n);
  for (size_t f = 0; f < geo.tris.size(); ++f) {
    const auto& t = geo.tris[f];
    const vec2d a = q[t[0]], b = q[t[1]], c = q[t[2]];
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area) < 1e-12) continue;
    const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(n - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(n - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) / area;
        const double w1 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) / area;
        const double w2 = 1 - w0 - w1;
        if (w0 < -1e-7 || w1 < -1e-7 || w2 < -1e-7) continue;
        const size_t i = size_t(y) * n + x;
        const float z = float(w0 * d[t[0]] + w1 * d[t[1]] + w2 * d[t[2]]);
        if (z < fr.depth[i]) {
          fr.depth[i] = z;
          fr.face[i] = int32_t(f);
          bary[i] = {float(w0), float(w1), float(w2)};
        }
      }
  }
  for (size_t i = 0; i < fr.face.size(); ++i) {
    if (fr.face[i] < 0) continue;
    const auto& t = geo.tris[size_t(fr.face[i])];
    const vec2d uv = m.uvs[t[0]] * bary[i][0] + m.uvs[t[1]] * bary[i][1] + m.uvs[t[2]] * bary[i][2];
    fr.lab[i] = oklab(sample_texture(*m.texture, uv));
    fr.pos[i] = geo.positions[t[0]] * bary[i][0] + geo.positions[t[1]] * bary[i][1] + geo.positions[t[2]] * bary[i][2];
  }
  return fr;
}
}  // namespace detail
namespace {
double de(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return std::sqrt(double(a[0] - b[0]) * (a[0] - b[0]) + double(a[1] - b[1]) * (a[1] - b[1]) + double(a[2] - b[2]) * (a[2] - b[2]));
}
double chroma(const std::array<float, 3>& c) { return std::sqrt(double(c[1]) * c[1] + double(c[2]) * c[2]); }
// Masked box blur (separable, normalised by the mask), radius r px.
std::vector<double> blur(const std::vector<double>& v, const std::vector<uint8_t>& mask, int w, int h, int r) {
  std::vector<double> tmp(v.size(), 0), out(v.size(), 0), wt(v.size(), 0), wo(v.size(), 0);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      double s = 0, n = 0;
      for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k)
        if (mask[size_t(y) * w + k]) { s += v[size_t(y) * w + k]; n += 1; }
      tmp[size_t(y) * w + x] = s;
      wt[size_t(y) * w + x] = n;
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      double s = 0, n = 0;
      for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k) {
        s += tmp[size_t(k) * w + x];
        n += wt[size_t(k) * w + x];
      }
      out[size_t(y) * w + x] = n > 0 ? s / n : 0;
      wo[size_t(y) * w + x] = n;
    }
  return out;
}
double pearson_masked(const std::vector<double>& a, const std::vector<double>& b, const std::vector<uint8_t>& m) {
  double sa = 0, sb = 0, n = 0;
  for (size_t i = 0; i < a.size(); ++i)
    if (m[i]) { sa += a[i]; sb += b[i]; n += 1; }
  if (n < 400) return 0;
  sa /= n;
  sb /= n;
  double ab = 0, aa = 0, bb = 0;
  for (size_t i = 0; i < a.size(); ++i)
    if (m[i]) {
      ab += (a[i] - sa) * (b[i] - sb);
      aa += (a[i] - sa) * (a[i] - sa);
      bb += (b[i] - sb) * (b[i] - sb);
    }
  return aa > 0 && bb > 0 ? ab / std::sqrt(aa * bb) : 0;
}
// Connected same-label components (4-neighbour) of at least `min_px` pixels, per 1000 labelled pixels.
double components_per_1000(const std::vector<int>& label, int w, int h, int min_px) {
  std::vector<uint8_t> seen(label.size(), 0);
  size_t comps = 0, pixels = 0;
  std::vector<size_t> stack;
  for (size_t s = 0; s < label.size(); ++s) {
    if (label[s] < 0) continue;
    ++pixels;
    if (seen[s]) continue;
    size_t size = 0;
    stack.assign(1, s);
    seen[s] = 1;
    while (!stack.empty()) {
      const size_t i = stack.back();
      stack.pop_back();
      ++size;
      const int x = int(i % size_t(w)), y = int(i / size_t(w));
      const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
      for (int k = 0; k < 4; ++k) {
        if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
        const size_t j = size_t(ny[k]) * size_t(w) + size_t(nx[k]);
        if (!seen[j] && label[j] == label[s]) {
          seen[j] = 1;
          stack.push_back(j);
        }
      }
    }
    comps += size >= size_t(min_px);
  }
  return pixels ? 1000.0 * double(comps) / double(pixels) : 0;
}
// Patch support: is a small patch of the hidden side something the picture itself contains anywhere?
// A single-picture patch prior; TRELLIS faces, smudges and foreign streaks are patches the picture never
// shows. 7x7 Oklab patches (colour and structure), nearest neighbour among the picture's own patches.
constexpr int kPatchR = 3;
constexpr int kPatchDim = (2 * kPatchR + 1) * (2 * kPatchR + 1) * 3;
struct PatchSet {
  std::vector<float> v;
  std::vector<std::array<int, 2>> pos;
  size_t size() const { return pos.size(); }
  const float* at(size_t i) const { return v.data() + i * kPatchDim; }
};
void collect_patches(const std::vector<std::array<float, 3>>& lab, const std::vector<uint8_t>& mask, int w, int h, PatchSet& out) {
  for (int y = kPatchR; y < h - kPatchR; ++y)
    for (int x = kPatchR; x < w - kPatchR; ++x) {
      bool full = true;
      for (int dy = -kPatchR; dy <= kPatchR && full; ++dy)
        for (int dx = -kPatchR; dx <= kPatchR; ++dx)
          if (!mask[size_t(y + dy) * w + x + dx]) { full = false; break; }
      if (!full) continue;
      for (int dy = -kPatchR; dy <= kPatchR; ++dy)
        for (int dx = -kPatchR; dx <= kPatchR; ++dx)
          for (int k = 0; k < 3; ++k) out.v.push_back(lab[size_t(y + dy) * w + x + dx][size_t(k)]);
      out.pos.push_back({x, y});
    }
}
// At most n patches, evenly spaced over the set (deterministic).
PatchSet cap_patches(const PatchSet& s, size_t n) {
  if (s.size() <= n || n < 2) return s;
  PatchSet o;
  o.v.reserve(n * kPatchDim);
  for (size_t k = 0; k < n; ++k) {
    const size_t i = size_t(double(k) * double(s.size() - 1) / double(n - 1));
    o.v.insert(o.v.end(), s.at(i), s.at(i) + kPatchDim);
    o.pos.push_back(s.pos[i]);
  }
  return o;
}
float patch_d2(const float* a, const float* b, float best) {
  float d = 0;
  for (int i = 0; i < kPatchDim; i += 21) {  // early exit every 7 pixels
    for (int j = i; j < i + 21; ++j) { const float t = a[j] - b[j]; d += t * t; }
    if (d >= best) return d;
  }
  return d;
}
double percentile_of(std::vector<double> v, double q) {  // linear interpolation, as numpy
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const double r = q * double(v.size() - 1);
  const size_t lo = size_t(std::floor(r)), hi = std::min(v.size() - 1, lo + 1);
  return v[lo] + (v[hi] - v[lo]) * (r - double(lo));
}
}  // namespace

nlohmann::json hidden_side_checks(const TexturedMesh& m, const Source& src, const Registration& reg, const std::vector<ExtraView>* extra) {
  const Mesh geo = render_geometry(m);
  // Extra pictures (multi-angle import): what they show is seen, not inferred, and is left out of the
  // hidden-side measures; their patches join the picture's own patch library.
  std::vector<View> extra_views;
  if (extra)
    for (const auto& e : *extra) extra_views.push_back(View::build(geo, *e.source, e.reg.pose));
  auto seen_by_extra = [&](size_t face, vec3d p) {
    if (extra_views.empty()) return false;
    const auto& t = geo.tris[face];
    vec3d fn = cross(geo.positions[t[1]] - geo.positions[t[0]], geo.positions[t[2]] - geo.positions[t[0]]);
    fn = length(fn) > 0 ? normalize(fn) : vec3d{0, 0, 1};
    for (size_t k = 0; k < extra_views.size(); ++k)
      if (extra_conf(extra_views[k], (*extra)[k], p, fn, fn) >= 0.5f) return true;
    return false;
  };
  double back_seen = 0, back_all = 0;
  size_t back_seen_patches = 0;  // back patches a real extra picture shows: supported by definition
  // UV charts = faces connected through shared render vertices (a UV cut splits the vertex).
  std::vector<uint32_t> parent(geo.positions.size());
  std::iota(parent.begin(), parent.end(), 0u);
  auto root = [&](uint32_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  };
  for (const auto& t : geo.tris) {
    const uint32_t a = root(t[0]);
    for (int k = 1; k < 3; ++k) {
      const uint32_t b = root(t[size_t(k)]);
      if (a != b) parent[b] = a;
    }
  }
  vec3d lo = geo.positions.front(), hi = lo;
  for (auto p : geo.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const vec3d centre = (lo + hi) * 0.5;
  double radius = 1e-9;
  for (auto p : geo.positions) radius = std::max(radius, length(p - centre));
  // Picture palette (k-means) for fragmentation and off-palette colours.
  nlohmann::json pal_ev;
  const auto palette = picture_palette(src, 0, 8, pal_ev);
  constexpr int N = 256;
  size_t pairs = 0, edges = 0, bounds = 0, edge_bounds = 0, obj = 0, off_palette = 0;
  double chroma_sum = 0, lum_sum = 0, frag_sum = 0, janus = 0;
  nlohmann::json janus_detail;
  PatchSet back_patches;
  double back_object_pixels = 0;
  int back_views = 0;
  nlohmann::json views = nlohmann::json::array();
  for (double dyaw : {90.0, 135.0, 180.0, 225.0, 270.0}) {
    const Frame fr = render_ids(geo, m, reg.pose.yaw + dyaw, 10, centre, radius, N);
    const double jump = 2.5 * fr.pixel_world;
    std::vector<uint8_t> seen(size_t(N) * N, 0);
    if (!extra_views.empty())
      for (size_t i = 0; i < seen.size(); ++i)
        if (fr.face[i] >= 0) seen[i] = seen_by_extra(size_t(fr.face[i]), fr.pos[i]);
    std::vector<int> label(size_t(N) * N, -1);
    size_t vobj = 0, vpairs = 0, vedges = 0, vbounds = 0, veb = 0;
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) {
        const size_t i = size_t(y) * N + x;
        if (fr.face[i] < 0) continue;
        ++vobj;
        const auto& c = fr.lab[i];
        chroma_sum += chroma(c);
        lum_sum += c[0];
        const size_t pi = nearest_colour(palette, c);
        label[i] = int(pi);
        off_palette += de(c, palette[pi]) > 0.10;
        for (auto [dx, dy] : {std::pair{1, 0}, std::pair{0, 1}}) {
          if (x + dx >= N || y + dy >= N) continue;
          const size_t j = size_t(y + dy) * N + x + dx;
          if (fr.face[j] < 0 || std::abs(fr.depth[j] - fr.depth[i]) > jump) continue;  // silhouettes / overlaps
          ++vpairs;
          const bool edge = de(c, fr.lab[j]) > 0.05;
          const bool bound = root(geo.tris[size_t(fr.face[i])][0]) != root(geo.tris[size_t(fr.face[j])][0]);
          vedges += edge;
          vbounds += bound;
          veb += edge && bound;
        }
      }
    // Front copy at feature scale ("Janus"): the back, band-passed between ~1% and ~6% of the object's
    // size, against the picture seen straight through the object at the same surface points.
    if (dyaw == 180.0) {
      std::vector<uint8_t> mask(size_t(N) * N, 0);
      std::array<std::vector<double>, 3> tex, pic;
      for (auto& v : tex) v.assign(size_t(N) * N, 0);
      for (auto& v : pic) v.assign(size_t(N) * N, 0);
      int ymin = N, ymax = -1;
      for (size_t i = 0; i < mask.size(); ++i) {
        if (fr.face[i] < 0 || seen[i]) continue;
        const vec2d q = reg.pose.to_image(fr.pos[i]);
        const auto c = src.sample(q.x, q.y);
        if (c[3] < 0.5f) continue;
        const auto pl = oklab(c);
        mask[i] = 1;
        ymin = std::min(ymin, int(i / N));
        ymax = std::max(ymax, int(i / N));
        for (int k = 0; k < 3; ++k) {
          tex[size_t(k)][i] = fr.lab[i][size_t(k)];
          pic[size_t(k)][i] = pl[size_t(k)];
        }
      }
      const double size_px = std::max(8, ymax - ymin + 1);
      const int r1 = std::max(1, int(std::lround(0.01 * size_px))), r2 = std::max(r1 + 2, int(std::lround(0.06 * size_px)));
      double corr[3]{};
      for (int k = 0; k < 3; ++k) {
        auto band = [&](const std::vector<double>& v) {
          const auto a = blur(v, mask, N, N, r1), b = blur(v, mask, N, N, r2);
          std::vector<double> d(v.size());
          for (size_t i = 0; i < v.size(); ++i) d[i] = a[i] - b[i];
          return d;
        };
        corr[k] = pearson_masked(band(tex[size_t(k)]), band(pic[size_t(k)]), mask);
      }
      janus = 0.6 * corr[0] + 0.2 * corr[1] + 0.2 * corr[2];
      janus_detail = {{"L", corr[0]}, {"a", corr[1]}, {"b", corr[2]}, {"band_px", {r1, r2}}};
      // Milestone 4 / W2: the share of the back view this measure actually sees. With a real back picture only a
      // strip along depth edges stays inferred (hooded character: 14 %), too little to judge (docs/agent/W2-HOODED-BACK.md).
      size_t mask_px = 0;
      for (auto v : mask) mask_px += v;
      janus_detail["measured_pixels"] = mask_px;
      janus_detail["view_pixels"] = vobj;
      janus_detail["measured_fraction"] = vobj ? double(mask_px) / double(vobj) : 0.0;
    }
    if (dyaw >= 135.0 && dyaw <= 225.0) {  // back and back three-quarters
      std::vector<uint8_t> on_object(size_t(N) * N, 0);
      for (size_t i = 0; i < on_object.size(); ++i) {
        on_object[i] = fr.face[i] >= 0 && !seen[i];
        if (fr.face[i] >= 0) { back_all += 1; back_seen += seen[i]; }
      }
      collect_patches(fr.lab, on_object, N, N, back_patches);
      if (!extra_views.empty()) {
        std::vector<uint8_t> all(size_t(N) * N, 0);
        for (size_t i = 0; i < all.size(); ++i) all[i] = fr.face[i] >= 0;
        PatchSet every;
        collect_patches(fr.lab, all, N, N, every);
        for (const auto& q : every.pos) back_seen_patches += seen[size_t(q[1]) * N + size_t(q[0])];
      }
      back_object_pixels += double(vobj);
      ++back_views;
    }
    const double frag = components_per_1000(label, N, N, 3);
    frag_sum += frag;
    obj += vobj;
    // Chart lift on the straight back only: on thin objects the side views mix the picture-coloured rim
    // with the back along the very chart border that separates them, which is not patchwork.
    if (dyaw == 180.0) {
      pairs += vpairs;
      edges += vedges;
      bounds += vbounds;
      edge_bounds += veb;
    }
    views.push_back({{"yaw_from_picture", dyaw}, {"object_pixels", vobj}, {"fragments_per_1000_px", frag},
                     {"colour_edges", vedges}, {"chart_boundaries", vbounds}, {"edges_on_chart_boundaries", veb}});
  }
  // The picture at the same object size (nearest sampling), same palette and component rule.
  double pic_frag = 0, pic_chroma = 0, pic_lum = 0;
  {
    size_t area = 0;
    int x0 = src.width, y0 = src.height, x1 = -1, y1 = -1;
    for (int y = 0; y < src.height; ++y)
      for (int x = 0; x < src.width; ++x)
        if (src.alpha(x, y) >= 0.5f) {
          ++area;
          x0 = std::min(x0, x);
          y0 = std::min(y0, y);
          x1 = std::max(x1, x);
          y1 = std::max(y1, y);
        }
    const double scale = area ? std::sqrt(double(obj) / 5.0 / double(area)) : 1;
    const int w = std::max(1, int(std::ceil((x1 - x0 + 1) * scale))), h = std::max(1, int(std::ceil((y1 - y0 + 1) * scale)));
    std::vector<int> label(size_t(w) * h, -1);
    size_t n = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const int sx = std::min(x1, x0 + int((x + 0.5) / scale)), sy = std::min(y1, y0 + int((y + 0.5) / scale));
        const auto& p = src.px[size_t(sy) * src.width + sx];
        if (p[3] < 0.5f) continue;
        const auto c = oklab(p);
        label[size_t(y) * w + x] = int(nearest_colour(palette, c));
        pic_chroma += chroma(c);
        pic_lum += c[0];
        ++n;
      }
    pic_frag = components_per_1000(label, w, h, 3);
    pic_chroma /= std::max<size_t>(n, 1);
    pic_lum /= std::max<size_t>(n, 1);
  }
  // Patch support of the back against the picture, at the same object size (box-filtered picture).
  double unsupported = 0, support_tau = 0;
  size_t picture_patch_count = 0, back_patch_count = 0;
  {
    size_t area = 0;
    int x0 = src.width, y0 = src.height, x1 = -1, y1 = -1;
    for (int y = 0; y < src.height; ++y)
      for (int x = 0; x < src.width; ++x)
        if (src.alpha(x, y) >= 0.5f) {
          ++area;
          x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
        }
    if (area && back_views) {
     PatchSet pic_all;
     // Each picture (the front, then the extra angles) resampled to the object's size in the back views.
     std::vector<const Source*> library{&src};
     if (extra)
       for (const auto& e : *extra) library.push_back(e.source);
     for (const Source* ps : library) {
      const Source& src = *ps;
      size_t area = 0;
      int x0 = src.width, y0 = src.height, x1 = -1, y1 = -1;
      for (int y = 0; y < src.height; ++y)
        for (int x = 0; x < src.width; ++x)
          if (src.alpha(x, y) >= 0.5f) {
            ++area;
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
          }
      if (!area) continue;
      const double scale = std::sqrt(back_object_pixels / back_views / double(area));
      const int w = std::max(1, int((x1 - x0 + 1) * scale)), h = std::max(1, int((y1 - y0 + 1) * scale));
      std::vector<std::array<float, 3>> lab(size_t(w) * h, {0, 0, 0});
      std::vector<uint8_t> mask(size_t(w) * h, 0);
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const int sx0 = x0 + int(std::floor(x / scale)), sx1 = std::min(x1 + 1, std::max(sx0 + 1, x0 + int(std::floor((x + 1) / scale))));
          const int sy0 = y0 + int(std::floor(y / scale)), sy1 = std::min(y1 + 1, std::max(sy0 + 1, y0 + int(std::floor((y + 1) / scale))));
          double r = 0, g = 0, b = 0, al = 0, n = 0;
          for (int sy = sy0; sy < sy1; ++sy)
            for (int sx = sx0; sx < sx1; ++sx) {
              const auto& p = src.px[size_t(sy) * src.width + sx];
              // Averaged in sRGB-encoded values, as an image scaler does (keeps thin highlights and
              // outlines at the weight they have in the picture).
              r += srgb8_from_linear(p[0]) * p[3]; g += srgb8_from_linear(p[1]) * p[3]; b += srgb8_from_linear(p[2]) * p[3];
              al += p[3]; n += 1;
            }
          if (n == 0 || al / n < 0.5) continue;
          mask[size_t(y) * w + x] = 1;
          auto lin = [&](double e) { return linear_from_srgb8(uint8_t(std::clamp(std::lround(e / al), 0L, 255L))); };
          lab[size_t(y) * w + x] = oklab({lin(r), lin(g), lin(b), 1.0f});
        }
      collect_patches(lab, mask, w, h, pic_all);
     }
      const PatchSet pic = cap_patches(pic_all, 4000);
      const PatchSet back = cap_patches(back_patches, 6000);
      picture_patch_count = pic.size();
      back_patch_count = back.size();
      if (pic.size() >= 200 && back.size() >= 200) {
        // The picture's own spread: each patch against the picture's patches away from it (> 2r px).
        std::vector<double> self(pic.size());
        for (size_t i = 0; i < pic.size(); ++i) {
          float best = std::numeric_limits<float>::infinity();
          for (size_t j = 0; j < pic.size(); ++j) {
            if (std::max(std::abs(pic.pos[i][0] - pic.pos[j][0]), std::abs(pic.pos[i][1] - pic.pos[j][1])) <= 2 * kPatchR) continue;
            best = std::min(best, patch_d2(pic.at(i), pic.at(j), best));
          }
          self[i] = std::sqrt(double(best));
        }
        support_tau = percentile_of(self, 0.95);
        const float tau2 = float(support_tau * support_tau);
        size_t far = 0;
        for (size_t i = 0; i < back.size(); ++i) {
          float best = std::numeric_limits<float>::infinity();
          for (size_t j = 0; j < pic.size() && best > tau2; ++j) best = std::min(best, patch_d2(back.at(i), pic.at(j), best));
          far += best > tau2;
        }
        unsupported = double(far) / double(back.size());
      }
      // With extra pictures the measure covers the whole back: patches a picture shows count as supported.
      if (back_seen_patches > 0) {
        const double unseen_patches = double(back_patches.size());
        unsupported = unsupported * unseen_patches / std::max(1.0, unseen_patches + double(back_seen_patches));
      }
    }
  }
  const double edge_rate = pairs ? double(bounds) / double(pairs) : 0;
  const double chart_lift = edges && edge_rate > 0 ? (double(edge_bounds) / double(edges)) / edge_rate : 1;
  const double frag = frag_sum / 5;
  const double frag_ratio = frag / std::max(pic_frag, 1.0);
  const double chroma_ratio = pic_chroma > 0.01 ? (chroma_sum / std::max<size_t>(obj, 1)) / pic_chroma : 1;
  const double off = obj ? double(off_palette) / double(obj) : 0;
  return {{"views", views}, {"palette", pal_ev},
          {"chart_edge_lift", chart_lift}, {"chart_boundary_rate", edge_rate},
          {"colour_edge_rate", pairs ? double(edges) / double(pairs) : 0},
          {"fragments_per_1000_px", frag}, {"picture_fragments_per_1000_px", pic_frag}, {"fragmentation_ratio", frag_ratio},
          {"chroma_ratio", chroma_ratio}, {"luminance_mean", lum_sum / std::max<size_t>(obj, 1)}, {"picture_luminance_mean", pic_lum},
          {"off_palette_fraction", off}, {"unsupported_patch_fraction", unsupported},
          {"extra_pictures", extra_views.size()}, {"back_seen_by_extra_pictures_fraction", back_all > 0 ? back_seen / back_all : 0.0}, {"patch_support", {{"radius_px", kPatchR}, {"picture_patches", picture_patch_count}, {"back_patches", back_patch_count}, {"picture_p95_distance", support_tau}}}, {"front_copy_correlation", janus}, {"front_copy_channels", janus_detail},
          {"method", "unlit base colour at yaw +90/+135/+180/+225/+270 from the registered camera, elevation 10, 256 px; "
                     "unsupported patches = back/back-3/4 7x7 Oklab patches farther from every picture patch than 95% of the picture's own patches are from each other; "
                     "chart lift (straight back view) = P(chart boundary | colour edge dE>0.05) / P(chart boundary), depth-continuous neighbours only"}};
}
}  // namespace p3dasset::autotex
