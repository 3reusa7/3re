// TRELLIS native texture as the hybrid's base layer: the engine's sparse PBR volume (TRELLIS_DUMP_POST)
// sampled on the final surface, and the native -> picture colour calibration
// (docs/superpowers/specs/2026-10-20-hybrid-texture-design.md).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include "auto_texture_internal.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace p3dasset::autotex {
using namespace pixel3d;
namespace {
uint64_t voxel_key(int x, int y, int z) {  // same packing as the engine's VoxSampler
  return (uint64_t(uint32_t(x)) << 40) | (uint64_t(uint32_t(y)) << 20) | uint32_t(z);
}
float linear_from_unit_srgb(float v) {
  const double x = std::clamp(double(v), 0.0, 1.0);
  return float(x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4));
}
}  // namespace

NativeVolume NativeVolume::from_voxels(int res, const std::vector<std::array<int, 3>>& coords, const std::vector<std::array<float, 3>>& srgb) {
  if (res <= 0 || res > 2048 || coords.size() != srgb.size() || coords.empty()) throw std::runtime_error("Native texture volume is empty or inconsistent");
  NativeVolume v;
  v.res = res;
  v.index.reserve(coords.size() * 2);
  v.linear.reserve(coords.size());
  for (size_t i = 0; i < coords.size(); ++i) {
    const auto& c = coords[i];
    if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[0] >= res || c[1] >= res || c[2] >= res) throw std::runtime_error("Native texture voxel outside its grid");
    v.index[voxel_key(c[0], c[1], c[2])] = uint32_t(i);
    v.linear.push_back({linear_from_unit_srgb(srgb[i][0]), linear_from_unit_srgb(srgb[i][1]), linear_from_unit_srgb(srgb[i][2])});
  }
  double mean[3]{};
  for (const auto& c : v.linear)
    for (int k = 0; k < 3; ++k) mean[k] += c[size_t(k)];
  v.evidence = {{"resolution", res}, {"voxels", coords.size()},
                {"mean_linear_rgb", {mean[0] / double(coords.size()), mean[1] / double(coords.size()), mean[2] / double(coords.size())}}};
  return v;
}

void NativeVolume::smooth(double sigma) {
  if (sigma <= 0 || linear.empty()) return;
  std::vector<std::array<int, 3>> coord(linear.size());
  for (const auto& [k, i] : index) coord[i] = {int((k >> 40) & 0xFFFFF), int((k >> 20) & 0xFFFFF), int(k & 0xFFFFF)};
  std::vector<std::array<float, 3>> out(linear.size());
  const double inv = 1 / (2 * sigma * sigma);
  double before = 0, after = 0;
  size_t pairs = 0;
  detail::parallel_rows(int(linear.size()), [&](int a, int b) {
    for (int i = a; i < b; ++i) {
      const auto& c = coord[size_t(i)];
      double acc[3]{}, ws = 0;
      for (int dz = -2; dz <= 2; ++dz)
        for (int dy = -2; dy <= 2; ++dy)
          for (int dx = -2; dx <= 2; ++dx) {
            const auto it = index.find(voxel_key(c[0] + dx, c[1] + dy, c[2] + dz));
            if (it == index.end()) continue;
            const double w = std::exp(-(dx * dx + dy * dy + dz * dz) * inv);
            for (int k = 0; k < 3; ++k) acc[k] += w * linear[it->second][size_t(k)];
            ws += w;
          }
      for (int k = 0; k < 3; ++k) out[size_t(i)][size_t(k)] = float(acc[k] / ws);
    }
  });
  // Evidence: mean luminance step between 6-neighbours, before and after.
  for (size_t i = 0; i < linear.size(); i += 7) {
    const auto& c = coord[i];
    const auto it = index.find(voxel_key(c[0] + 1, c[1], c[2]));
    if (it == index.end()) continue;
    auto lum = [](const std::array<float, 3>& v) { return (v[0] + v[1] + v[2]) / 3.0; };
    before += std::abs(lum(linear[i]) - lum(linear[it->second]));
    after += std::abs(lum(out[i]) - lum(out[it->second]));
    ++pairs;
  }
  linear.swap(out);
  evidence["smoothing"] = {{"sigma_voxels", sigma}, {"radius_voxels", 2}, {"neighbour_step_before", before / double(std::max<size_t>(pairs, 1))},
                           {"neighbour_step_after", after / double(std::max<size_t>(pairs, 1))}};
}

// Layout (engine, trellis_cli.cpp): i32 V,F,Mv,res; f32 verts[V*3]; i32 faces[F*3]; i32 coords[Mv*3];
// f32 pbr6[Mv*6] = base_color.rgb (the bytes the engine would write to its sRGB texture), metallic, roughness, alpha.
NativeVolume NativeVolume::load_post(const std::string& utf8_path, uint64_t max_bytes) {
  const auto path = path_from_utf8(utf8_path);
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(path, ec);
  if (ec || size < 16 || size > max_bytes) throw std::runtime_error("Native texture dump missing or outside its size limit");
  std::ifstream in(path, std::ios::binary);
  int32_t h[4]{};
  in.read(reinterpret_cast<char*>(h), sizeof h);
  const auto [V, F, M, res] = h;
  if (!in || V < 0 || F < 0 || M <= 0 || res <= 0 || res > 2048) throw std::runtime_error("Native texture dump has no PBR volume");
  const uint64_t expected = 16 + uint64_t(V) * 12 + uint64_t(F) * 12 + uint64_t(M) * 12 + uint64_t(M) * 24;
  if (expected != size) throw std::runtime_error("Native texture dump is truncated or not a TRELLIS POST dump");
  in.seekg(std::streamoff(16 + uint64_t(V) * 12 + uint64_t(F) * 12));
  std::vector<std::array<int, 3>> coords(static_cast<size_t>(M));
  in.read(reinterpret_cast<char*>(coords.data()), std::streamsize(size_t(M) * 12));
  std::vector<std::array<float, 6>> pbr(static_cast<size_t>(M));
  in.read(reinterpret_cast<char*>(pbr.data()), std::streamsize(size_t(M) * 24));
  if (!in) throw std::runtime_error("Could not read the native texture dump");
  std::vector<std::array<float, 3>> srgb(static_cast<size_t>(M));
  double metallic = 0, roughness = 0;
  for (size_t i = 0; i < pbr.size(); ++i) {
    for (int k = 0; k < 6; ++k)
      if (!std::isfinite(pbr[i][size_t(k)])) throw std::runtime_error("Native texture dump contains non-finite values");
    srgb[i] = {pbr[i][0], pbr[i][1], pbr[i][2]};
    metallic += pbr[i][3];
    roughness += pbr[i][4];
  }
  auto v = from_voxels(res, coords, srgb);
  {
    std::vector<float> a(pbr.size());
    for (size_t i = 0; i < pbr.size(); ++i) a[i] = std::clamp(pbr[i][5], 0.f, 1.f);
    std::vector<float> sorted = a;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float median = sorted[sorted.size() / 2];
    v.translucent = median < 0.8f;
    v.evidence["median_alpha"] = median;
    v.evidence["translucent"] = v.translucent;
    // Row order of from_voxels: the coordinate order of the dump.
    v.alpha.assign(v.linear.size(), 1.f);
    for (size_t i = 0; i < coords.size(); ++i) {
      const auto it = v.index.find(voxel_key(coords[i][0], coords[i][1], coords[i][2]));
      if (it != v.index.end()) v.alpha[it->second] = a[i];
    }
  }
  v.evidence["mesh_vertices"] = V;
  v.evidence["mesh_triangles"] = F;
  v.evidence["mean_metallic"] = metallic / double(M);
  v.evidence["mean_roughness"] = roughness / double(M);
  v.smooth(1.0);
  return v;
}

bool NativeVolume::sample(vec3d g, std::array<float, 3>& out, bool* direct) const {
  if (direct) *direct = false;
  const double p[3] = {g.x, -g.z, g.y};  // GLB (x, z, -y) back to TRELLIS (x, y, z)
  double gf[3];
  int b[3];
  for (int a = 0; a < 3; ++a) {
    gf[a] = (p[a] + 0.5) * res - 0.5;
    if (!std::isfinite(gf[a]) || std::abs(gf[a]) > 1e6) return false;
    b[a] = int(std::floor(gf[a]));
  }
  auto find = [&](int x, int y, int z) -> const std::array<float, 3>* {
    const auto it = index.find(voxel_key(x, y, z));
    return it == index.end() ? nullptr : &linear[it->second];
  };
  double acc[3]{}, wsum = 0;
  for (int dz = 0; dz < 2; ++dz)
    for (int dy = 0; dy < 2; ++dy)
      for (int dx = 0; dx < 2; ++dx) {
        const double w = (dx ? gf[0] - b[0] : 1 - (gf[0] - b[0])) * (dy ? gf[1] - b[1] : 1 - (gf[1] - b[1])) * (dz ? gf[2] - b[2] : 1 - (gf[2] - b[2]));
        if (w <= 0) continue;
        if (const auto* c = find(b[0] + dx, b[1] + dy, b[2] + dz)) {
          for (int k = 0; k < 3; ++k) acc[k] += w * (*c)[size_t(k)];
          wsum += w;
        }
      }
  if (wsum > 1e-6) {
    for (int k = 0; k < 3; ++k) out[size_t(k)] = float(acc[k] / wsum);
    if (direct) *direct = true;
    return true;
  }
  const double fr[3] = {gf[0] - b[0], gf[1] - b[1], gf[2] - b[2]};
  for (int r = 1; r <= 3; ++r) {
    double racc[3]{};
    int hits = 0;
    for (int dz = -r; dz <= r + 1; ++dz)
      for (int dy = -r; dy <= r + 1; ++dy)
        for (int dx = -r; dx <= r + 1; ++dx) {
          const double d = std::max({std::abs(dx - fr[0]), std::abs(dy - fr[1]), std::abs(dz - fr[2])});
          if (d > r + 0.5 || (r > 1 && d <= r - 0.5)) continue;
          if (const auto* c = find(b[0] + dx, b[1] + dy, b[2] + dz)) {
            for (int k = 0; k < 3; ++k) racc[k] += (*c)[size_t(k)];
            ++hits;
          }
        }
    if (hits) {
      for (int k = 0; k < 3; ++k) out[size_t(k)] = float(racc[k] / hits);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------------------------------------
// Colour calibration.
namespace {
using Lab = std::array<float, 3>;
double weighted_quantile(std::vector<std::pair<float, double>>& v, double q) {  // v sorted by value
  double total = 0;
  for (const auto& e : v) total += e.second;
  double run = 0;
  for (const auto& e : v) {
    run += e.second;
    if (run >= q * total) return e.first;
  }
  return v.empty() ? 0 : v.back().first;
}
struct Moments { double mean[3]{}, sd[3]{}; };
Moments moments(const std::vector<Lab>& x, const std::vector<double>& w, const std::vector<size_t>& rows) {
  Moments m;
  double ws = 0;
  for (auto i : rows) {
    for (int k = 0; k < 3; ++k) m.mean[k] += x[i][size_t(k)] * w[i];
    ws += w[i];
  }
  for (double& v : m.mean) v /= std::max(ws, 1e-30);
  for (auto i : rows)
    for (int k = 0; k < 3; ++k) m.sd[k] += (x[i][size_t(k)] - m.mean[k]) * (x[i][size_t(k)] - m.mean[k]) * w[i];
  for (double& v : m.sd) v = std::sqrt(v / std::max(ws, 1e-30));
  return m;
}
double chroma(const Lab& c) { return std::sqrt(double(c[1]) * c[1] + double(c[2]) * c[2]); }

// Fit one method on `rows`. 0 = Reinhard mean/std per channel, 1 = L quantile map + a/b mean/std,
// 2 = method 1 + kernel-smoothed residual LUT over native colour.
ColourMap fit_method(int method, const std::vector<Lab>& n, const std::vector<Lab>& p, const std::vector<double>& w, const std::vector<size_t>& rows) {
  ColourMap m;
  const Moments mn = moments(n, w, rows), mp = moments(p, w, rows);
  for (int k = 0; k < 3; ++k) {
    m.gain[size_t(k)] = mn.sd[k] > 1e-6 ? std::clamp(mp.sd[k] / mn.sd[k], 0.25, 8.0) : 1.0;
    m.offset[size_t(k)] = mp.mean[k] - m.gain[size_t(k)] * mn.mean[k];
  }
  m.method = "reinhard";
  if (method == 0) return m;
  std::vector<std::pair<float, double>> ln, lp;
  for (auto i : rows) {
    ln.push_back({n[i][0], w[i]});
    lp.push_back({p[i][0], w[i]});
  }
  std::sort(ln.begin(), ln.end());
  std::sort(lp.begin(), lp.end());
  for (int q = 0; q <= 40; ++q) {
    const double t = 0.01 + 0.98 * q / 40.0;
    const float a = float(weighted_quantile(ln, t)), b = float(weighted_quantile(lp, t));
    if (!m.l_from.empty() && a <= m.l_from.back() + 1e-5f) continue;  // keep it strictly monotone
    m.l_from.push_back(a);
    m.l_to.push_back(std::max(b, m.l_to.empty() ? b : m.l_to.back()));
  }
  m.method = "l_quantile";
  if (m.l_from.size() < 2) {
    m.l_from.clear();
    m.l_to.clear();
    m.method = "reinhard";
  }
  if (method == 1) return m;
  // Residual LUT: what the picture shows for each native colour, beyond the global map; shrunk towards 0
  // where few correspondences support it, so a misaligned pair cannot repaint the back.
  constexpr int N = 16;
  m.lut_n = N;
  m.lut_lo = {1e9f, 1e9f, 1e9f};
  m.lut_hi = {-1e9f, -1e9f, -1e9f};
  for (auto i : rows)
    for (int k = 0; k < 3; ++k) {
      m.lut_lo[size_t(k)] = std::min(m.lut_lo[size_t(k)], n[i][size_t(k)]);
      m.lut_hi[size_t(k)] = std::max(m.lut_hi[size_t(k)], n[i][size_t(k)]);
    }
  for (int k = 0; k < 3; ++k) m.lut_hi[size_t(k)] = std::max(m.lut_hi[size_t(k)], m.lut_lo[size_t(k)] + 1e-3f);
  ColourMap base = m;
  base.lut_n = 0;
  std::vector<Lab> resid(rows.size());
  for (size_t r = 0; r < rows.size(); ++r) {
    const Lab b = base.apply(n[rows[r]]);
    for (int k = 0; k < 3; ++k) resid[r][size_t(k)] = p[rows[r]][size_t(k)] - b[size_t(k)];
  }
  // Gaussian kernel in native Oklab; weights normalised to a mean of 1 so `prior` reads as "6 average pairs".
  const double sigma = 0.035, inv = 1 / (2 * sigma * sigma), prior = 6.0;
  double wmean = 0;
  for (auto i : rows) wmean += w[i];
  wmean = std::max(wmean / double(rows.size()), 1e-30);
  m.lut.assign(size_t(N) * N * N, {0, 0, 0});
  detail::parallel_rows(N, [&](int z0, int z1) {
    for (int z = z0; z < z1; ++z)
      for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
          const double q[3] = {m.lut_lo[0] + (m.lut_hi[0] - m.lut_lo[0]) * x / (N - 1.0), m.lut_lo[1] + (m.lut_hi[1] - m.lut_lo[1]) * y / (N - 1.0),
                               m.lut_lo[2] + (m.lut_hi[2] - m.lut_lo[2]) * z / (N - 1.0)};
          double acc[3]{}, ws = 0;
          for (size_t r = 0; r < rows.size(); ++r) {
            const auto& c = n[rows[r]];
            const double d2 = (c[0] - q[0]) * (c[0] - q[0]) + (c[1] - q[1]) * (c[1] - q[1]) + (c[2] - q[2]) * (c[2] - q[2]);
            if (d2 > 9 * sigma * sigma) continue;
            const double k = std::exp(-d2 * inv) * w[rows[r]] / wmean;
            for (int j = 0; j < 3; ++j) acc[j] += k * resid[r][size_t(j)];
            ws += k;
          }
          auto& o = m.lut[(size_t(z) * N + y) * N + x];
          for (int j = 0; j < 3; ++j) o[size_t(j)] = float(acc[j] / (ws + prior));
        }
  });
  m.method = "l_quantile_lut";
  return m;
}
}  // namespace

std::array<float, 3> ColourMap::apply(const std::array<float, 3>& c) const {
  Lab o;
  for (int k = 0; k < 3; ++k) o[size_t(k)] = float(gain[size_t(k)] * c[size_t(k)] + offset[size_t(k)]);
  if (l_from.size() >= 2) {
    const float x = c[0];
    const size_t n = l_from.size();
    if (x <= l_from.front()) {
      const float s = (l_to[1] - l_to[0]) / std::max(l_from[1] - l_from[0], 1e-5f);
      o[0] = l_to.front() + std::clamp(s, 0.25f, 8.f) * (x - l_from.front());
    } else if (x >= l_from.back()) {
      const float s = (l_to[n - 1] - l_to[n - 2]) / std::max(l_from[n - 1] - l_from[n - 2], 1e-5f);
      o[0] = l_to.back() + std::clamp(s, 0.25f, 8.f) * (x - l_from.back());
    } else {
      const size_t i = size_t(std::upper_bound(l_from.begin(), l_from.end(), x) - l_from.begin());
      const float t = (x - l_from[i - 1]) / std::max(l_from[i] - l_from[i - 1], 1e-6f);
      o[0] = l_to[i - 1] + t * (l_to[i] - l_to[i - 1]);
    }
  }
  if (lut_n > 1) {
    float f[3];
    int b[3];
    for (int k = 0; k < 3; ++k) {
      const float g = std::clamp((c[size_t(k)] - lut_lo[size_t(k)]) / (lut_hi[size_t(k)] - lut_lo[size_t(k)]), 0.f, 1.f) * float(lut_n - 1);
      b[k] = std::min(int(g), lut_n - 2);
      f[k] = g - float(b[k]);
    }
    for (int dz = 0; dz < 2; ++dz)
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const float w = (dx ? f[0] : 1 - f[0]) * (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]);
          const auto& r = lut[(size_t(b[2] + dz) * lut_n + b[1] + dy) * lut_n + b[0] + dx];
          for (int k = 0; k < 3; ++k) o[size_t(k)] += w * r[size_t(k)];
        }
  }
  o[0] = std::clamp(o[0], 0.f, 1.f);
  return o;
}

ColourMap fit_colour_map(const std::vector<Lab>& n, const std::vector<Lab>& p, const std::vector<double>& w, nlohmann::json& ev) {
  if (n.size() != p.size() || n.size() != w.size()) throw std::runtime_error("Colour calibration inputs differ in size");
  std::vector<size_t> all(n.size()), even, odd;
  std::iota(all.begin(), all.end(), size_t(0));
  // Interleaved split: neighbouring surface points land in both halves, so the held-out score measures
  // the colour map, not where the pairs happen to be.
  for (size_t i = 0; i < n.size(); ++i) (i % 2 ? odd : even).push_back(i);
  ev = nlohmann::json::object();
  ev["pairs"] = n.size();
  if (n.size() < 64) {
    ev["method"] = "identity";
    ev["reason"] = "too few surface points seen squarely in the picture";
    return ColourMap{};
  }
  const Moments mp = moments(p, w, odd);
  double cp = 0, ws = 0;
  for (auto i : odd) {
    cp += chroma(p[i]) * w[i];
    ws += w[i];
  }
  cp /= std::max(ws, 1e-30);
  auto score = [&](const ColourMap& m) {
    double de = 0, wsum = 0, cm = 0;
    std::vector<Lab> mapped;
    for (auto i : odd) {
      const Lab q = m.apply(n[i]);
      mapped.push_back(q);
      de += std::sqrt(double(q[0] - p[i][0]) * (q[0] - p[i][0]) + double(q[1] - p[i][1]) * (q[1] - p[i][1]) + double(q[2] - p[i][2]) * (q[2] - p[i][2])) * w[i];
      cm += chroma(q) * w[i];
      wsum += w[i];
    }
    std::vector<size_t> idx(mapped.size());
    std::iota(idx.begin(), idx.end(), size_t(0));
    std::vector<double> mw;
    for (auto i : odd) mw.push_back(w[i]);
    const Moments mm = moments(mapped, mw, idx);
    de /= std::max(wsum, 1e-30);
    cm /= std::max(wsum, 1e-30);
    // Held-out ΔE alone rewards washing colours out to the average; contrast and chroma must survive too.
    const double distribution = std::abs(mm.sd[0] - mp.sd[0]) + std::abs(cm - cp);
    return std::array<double, 3>{de + distribution, de, distribution};
  };
  nlohmann::json tried = nlohmann::json::array();
  int best = 0;
  double best_score = 1e30;
  for (int method = 0; method < 3; ++method) {
    const ColourMap m = fit_method(method, n, p, w, even);
    const auto s = score(m);
    tried.push_back({{"method", m.method}, {"score", s[0]}, {"heldout_delta_e", s[1]}, {"distribution_error", s[2]}});
    if (s[0] < best_score) { best_score = s[0]; best = method; }
  }
  double uncalibrated = 0;
  {
    ColourMap id;
    const auto s = score(id);
    uncalibrated = s[1];
    ev["uncalibrated_heldout_delta_e"] = s[1];
  }
  // When no colour map beats the uncalibrated colours on the held-out points, the pairs do not agree (the
  // picture's viewpoint is ambiguous or the surface is mostly unseen): fitting a/b to them amplifies noise
  // into foreign tints (2.3 end-to-end: a black motorbike turned green, chroma gains 1.5-1.7). The lightness
  // quantile map needs no pairing (it matches distributions), so only TRELLIS's darkness is corrected.
  const bool pairs_disagree = tried[size_t(best)]["heldout_delta_e"].get<double>() >= uncalibrated;
  ColourMap chosen = fit_method(pairs_disagree ? 1 : best, n, p, w, all);
  if (pairs_disagree) {
    chosen.gain[1] = chosen.gain[2] = 1;
    chosen.offset[1] = chosen.offset[2] = 0;
    chosen.method = "l_quantile_lightness_only";
    ev["why"] = "no colour map beat the uncalibrated colours on held-out points: lightness only";
  }
  ev["tried"] = tried;
  ev["method"] = chosen.method;
  ev["gain"] = chosen.gain;
  ev["offset"] = chosen.offset;
  const Moments mn = moments(n, w, all), mpa = moments(p, w, all);
  ev["native_mean_lab"] = mn.mean;
  ev["picture_mean_lab"] = mpa.mean;
  return chosen;
}
namespace detail {
size_t nearest_colour(const std::vector<std::array<float, 3>>& pal, const std::array<float, 3>& c) {
  size_t best = 0;
  double bd = 1e30;
  for (size_t i = 0; i < pal.size(); ++i) {
    const double d = double(pal[i][0] - c[0]) * (pal[i][0] - c[0]) + double(pal[i][1] - c[1]) * (pal[i][1] - c[1]) + double(pal[i][2] - c[2]) * (pal[i][2] - c[2]);
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}
std::vector<std::array<float, 3>> picture_palette(const Source& s, size_t max_exact, int fallback_k, nlohmann::json& ev) {
  std::unordered_map<uint32_t, size_t> count;
  size_t total = 0;
  for (const auto& p : s.px) {
    if (p[3] < 0.5f) continue;
    const uint32_t key = (uint32_t(srgb8_from_linear(p[0])) << 16) | (uint32_t(srgb8_from_linear(p[1])) << 8) | srgb8_from_linear(p[2]);
    ++count[key];
    ++total;
  }
  if (!total) throw std::runtime_error("The picture has no opaque pixels");
  std::vector<std::pair<size_t, uint32_t>> order;
  for (const auto& [k, n] : count) order.push_back({n, k});
  std::sort(order.rbegin(), order.rend());
  auto lab_of = [](uint32_t k) {
    return oklab({linear_from_srgb8(uint8_t(k >> 16)), linear_from_srgb8(uint8_t(k >> 8)), linear_from_srgb8(uint8_t(k)), 1.f});
  };
  // Exact palette when a handful of colours cover (almost) every pixel: true pixel art.
  size_t covered = 0, used = 0;
  while (used < order.size() && used < max_exact && covered < size_t(0.995 * double(total))) covered += order[used++].first;
  std::vector<std::array<float, 3>> pal;
  if (covered >= size_t(0.995 * double(total))) {
    for (size_t i = 0; i < used; ++i) pal.push_back(lab_of(order[i].second));
    ev = {{"method", "exact"}, {"colours", pal.size()}, {"unique_colours", order.size()}};
    return pal;
  }
  // Otherwise weighted k-means over the distinct colours (deterministic farthest-point seeding).
  std::vector<std::array<float, 3>> lab;
  std::vector<double> w;
  for (const auto& [n, k] : order) {
    lab.push_back(lab_of(k));
    w.push_back(double(n));
  }
  const int k = std::min<int>(fallback_k, int(lab.size()));
  pal.push_back(lab[0]);
  std::vector<double> near(lab.size(), 1e30);
  while (int(pal.size()) < k) {
    size_t far = 0;
    double best = -1;
    for (size_t i = 0; i < lab.size(); ++i) {
      const auto& c = pal.back();
      near[i] = std::min(near[i], double(lab[i][0] - c[0]) * (lab[i][0] - c[0]) + double(lab[i][1] - c[1]) * (lab[i][1] - c[1]) + double(lab[i][2] - c[2]) * (lab[i][2] - c[2]));
      if (near[i] * std::sqrt(w[i]) > best) { best = near[i] * std::sqrt(w[i]); far = i; }
    }
    pal.push_back(lab[far]);
  }
  for (int it = 0; it < 12; ++it) {
    std::vector<std::array<double, 4>> acc(pal.size(), {0, 0, 0, 0});
    for (size_t i = 0; i < lab.size(); ++i) {
      auto& a = acc[nearest_colour(pal, lab[i])];
      for (int d = 0; d < 3; ++d) a[size_t(d)] += lab[i][size_t(d)] * w[i];
      a[3] += w[i];
    }
    for (size_t j = 0; j < pal.size(); ++j)
      if (acc[j][3] > 0) pal[j] = {float(acc[j][0] / acc[j][3]), float(acc[j][1] / acc[j][3]), float(acc[j][2] / acc[j][3])};
  }
  ev = {{"method", "kmeans"}, {"colours", pal.size()}, {"unique_colours", order.size()}};
  return pal;
}
}  // namespace detail
float NativeVolume::sample_alpha(vec3d g) const {
  if (alpha.empty()) return 1.f;
  const double p[3] = {g.x, -g.z, g.y};
  double gf[3];
  int b[3];
  for (int a = 0; a < 3; ++a) {
    gf[a] = (p[a] + 0.5) * res - 0.5;
    if (!std::isfinite(gf[a]) || std::abs(gf[a]) > 1e6) return 1.f;
    b[a] = int(std::floor(gf[a]));
  }
  double acc = 0, wsum = 0;
  for (int dz = 0; dz < 2; ++dz)
    for (int dy = 0; dy < 2; ++dy)
      for (int dx = 0; dx < 2; ++dx) {
        const auto it = index.find(voxel_key(b[0] + dx, b[1] + dy, b[2] + dz));
        if (it == index.end()) continue;
        const double w = (dx ? gf[0] - b[0] : 1 - (gf[0] - b[0])) * (dy ? gf[1] - b[1] : 1 - (gf[1] - b[1])) * (dz ? gf[2] - b[2] : 1 - (gf[2] - b[2]));
        acc += alpha[it->second] * w;
        wsum += w;
      }
  if (wsum > 1e-6) return float(acc / wsum);
  for (int r = 1; r <= 2; ++r) {
    double a = 0;
    int n = 0;
    for (int dz = -r; dz <= r; ++dz)
      for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
          const auto it = index.find(voxel_key(b[0] + dx, b[1] + dy, b[2] + dz));
          if (it != index.end()) { a += alpha[it->second]; ++n; }
        }
    if (n) return float(a / n);
  }
  return 1.f;
}
}  // namespace p3dasset::autotex
