// Texture quality checks that apply to any candidate (auto texture, embedded/Generated PBR, legacy
// projection): UV coverage, stretch and overlap, seams, bleeding, front-to-back duplication,
// reprojection error against the picture, dark/abnormal colour. Measures only; never edits.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numeric>
#include <unordered_map>
#include "auto_texture_internal.hpp"

namespace p3dasset::autotex {
using namespace pixel3d;
using namespace detail;
namespace {
double de(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  return std::sqrt(double(a[0] - b[0]) * (a[0] - b[0]) + double(a[1] - b[1]) * (a[1] - b[1]) + double(a[2] - b[2]) * (a[2] - b[2]));
}
double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  const size_t i = std::min(v.size() - 1, size_t(q * double(v.size() - 1)));
  std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(i), v.end());
  return v[i];
}
double mean(const std::vector<double>& v) { return v.empty() ? 0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size()); }
double pearson(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() < 200) return 0;
  const double ma = mean(a), mb = mean(b);
  double sab = 0, saa = 0, sbb = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    sab += (a[i] - ma) * (b[i] - mb);
    saa += (a[i] - ma) * (a[i] - ma);
    sbb += (b[i] - mb) * (b[i] - mb);
  }
  return saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0;
}
uint32_t lcg(uint64_t& s) {
  s = s * 6364136223846793005ull + 1442695040888963407ull;
  return uint32_t(s >> 33);
}
uint64_t pos_key(vec3d p) {
  const float f[3] = {float(p.x), float(p.y), float(p.z)};
  uint32_t b[3];
  std::memcpy(b, f, 12);
  return (uint64_t(b[0]) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(b[1]) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(b[2]) * 0x165667B19E3779F9ull);
}
}  // namespace

nlohmann::json evaluate(const Mesh& logical, const TexturedMesh& m, const Source& src, const Registration& reg,
                        const std::vector<ExtraView>* extra) {
  const auto started = std::chrono::steady_clock::now();
  if (!m.texture || m.uvs.size() != m.positions.size() || m.indices.empty()) throw std::runtime_error("Candidate has no textured UV mesh");
  const auto& tex = *m.texture;
  const Mesh geo = render_geometry(m);
  const size_t nf = geo.tris.size();
  // UV: degenerate, out of range, stretch (Sander L2, normalised by the global 3D/UV area ratio).
  std::vector<double> area3(nf), areauv(nf);
  double total3 = 0, totaluv = 0, degenerate_area = 0;
  size_t degenerate = 0, out_of_range = 0;
  for (size_t f = 0; f < nf; ++f) {
    const auto& t = geo.tris[f];
    area3[f] = length(cross(geo.positions[t[1]] - geo.positions[t[0]], geo.positions[t[2]] - geo.positions[t[0]])) / 2;
    const vec2d a = m.uvs[t[0]], b = m.uvs[t[1]], c = m.uvs[t[2]];
    areauv[f] = std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    for (auto uv : {a, b, c}) out_of_range += !(uv.x >= -1e-6 && uv.y >= -1e-6 && uv.x <= 1 + 1e-6 && uv.y <= 1 + 1e-6);
    total3 += area3[f];
    if (areauv[f] * double(tex.width()) * tex.height() < 1e-6 && area3[f] > 0) {
      ++degenerate;
      degenerate_area += area3[f];
    } else totaluv += areauv[f];
  }
  const double norm = std::sqrt(std::max(totaluv, 1e-30) / std::max(total3, 1e-30));
  std::vector<std::pair<double, double>> stretch;  // (value, 3D area)
  for (size_t f = 0; f < nf; ++f) {
    if (areauv[f] * double(tex.width()) * tex.height() < 1e-6 || area3[f] <= 0) continue;
    const auto& t = geo.tris[f];
    const vec3d q1 = geo.positions[t[0]], q2 = geo.positions[t[1]], q3 = geo.positions[t[2]];
    const vec2d a = m.uvs[t[0]], b = m.uvs[t[1]], c = m.uvs[t[2]];
    const double A = ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    const vec3d Ss = (q1 * (b.y - c.y) + q2 * (c.y - a.y) + q3 * (a.y - b.y)) / (2 * A);
    const vec3d St = (q1 * (c.x - b.x) + q2 * (a.x - c.x) + q3 * (b.x - a.x)) / (2 * A);
    stretch.push_back({std::sqrt((dot(Ss, Ss) + dot(St, St)) / 2) * norm, area3[f]});
  }
  double stretch_mean = 0, stretched_area = 0, sw = 0;
  std::vector<double> sv;
  for (auto [v, a] : stretch) {
    stretch_mean += v * a;
    sw += a;
    if (v > 2.5) stretched_area += a;
    sv.push_back(v);
  }
  stretch_mean /= std::max(sw, 1e-30);
  // Overlap: texels claimed by more than one face (front and back sharing texels = duplication).
  const int G = 1024;
  std::vector<uint16_t> owners(size_t(G) * G, 0);
  size_t claimed = 0, shared = 0;
  for (size_t f = 0; f < nf; ++f) {
    const auto& t = geo.tris[f];
    raster_uv(m.uvs[t[0]] * G, m.uvs[t[1]] * G, m.uvs[t[2]] * G, G, G, 0, G, [&](int x, int y, double, double, double) {
      auto& o = owners[size_t(y) * G + x];
      if (o == 1) ++shared;
      if (o < 2) ++o;
    });
  }
  for (auto o : owners) claimed += o > 0;
  // Surface samples (area-uniform, deterministic).
  const View view = View::build(geo, src, reg.pose);
  // Extra pictures: their own reprojection error, and the surface they show is not judged as hidden.
  std::vector<View> extra_views;
  std::vector<std::vector<double>> extra_reproj;
  if (extra)
    for (const auto& e : *extra) extra_views.push_back(View::build(geo, *e.source, e.reg.pose));
  extra_reproj.resize(extra_views.size());
  auto harmonised = [&](size_t k, const std::array<float, 4>& c) {
    const auto& e = (*extra)[k];
    auto lab = oklab(c);
    lab[0] = float(std::clamp(e.gain[0] * lab[0] + e.offset[0], 0.0, 1.0));
    lab[1] = float(e.gain[1] * lab[1] + e.offset[1]);
    lab[2] = float(e.gain[2] * lab[2] + e.offset[2]);
    return lab;
  };
  std::vector<double> cdf(nf);
  std::partial_sum(area3.begin(), area3.end(), cdf.begin());
  std::vector<vec3d> smooth = m.normals.size() == m.positions.size() ? m.normals : vertex_normals(geo);
  const size_t N = 160000;
  uint64_t seed = 0x3e3e3e;
  std::vector<double> reproj, lum_visible, lum_hidden, dup_tex, dup_src, det_tex_v, det_src_v;
  size_t missing = 0, near_black = 0, near_black_visible = 0, source_black_visible = 0, bleed = 0, visible_n = 0;
  // Background colour of the cutout (transparent pixels): what bleeding would look like.
  std::array<double, 3> bg{};
  {
    double n = 0;
    for (int y = 0; y < src.height; y += 2)
      for (int x = 0; x < src.width; x += 2)
        if (src.alpha(x, y) < 0.1f) {
          const auto& p = src.px[size_t(y) * src.width + x];
          for (int d = 0; d < 3; ++d) bg[size_t(d)] += p[size_t(d)];
          n += 1;
        }
    for (auto& v : bg) v /= std::max(n, 1.0);
  }
  const auto bg_lab = oklab({float(bg[0]), float(bg[1]), float(bg[2]), 1});
  // Blurred source luminance for detail (high-frequency) comparisons.
  auto src_lum = [&](double x, double y) { return double(oklab(src.sample(x, y))[0]); };
  auto src_detail = [&](double x, double y) {
    const double r = 3;
    return src_lum(x, y) - 0.25 * (src_lum(x - r, y) + src_lum(x + r, y) + src_lum(x, y - r) + src_lum(x, y + r));
  };
  const double texel_u = 3.0 / tex.width(), texel_v = 3.0 / tex.height();
  // Texture detail at the same physical scale as the source detail kernel (3 source px).
  for (size_t s = 0; s < N; ++s) {
    const double r = (lcg(seed) / 4294967296.0) * cdf.back();
    const size_t f = size_t(std::lower_bound(cdf.begin(), cdf.end(), r) - cdf.begin());
    if (f >= nf) continue;
    double u = lcg(seed) / 4294967296.0, v = lcg(seed) / 4294967296.0;
    if (u + v > 1) { u = 1 - u; v = 1 - v; }
    const double w0 = 1 - u - v;
    const auto& t = geo.tris[f];
    const vec3d p = geo.positions[t[0]] * w0 + geo.positions[t[1]] * u + geo.positions[t[2]] * v;
    vec3d fn = cross(geo.positions[t[1]] - geo.positions[t[0]], geo.positions[t[2]] - geo.positions[t[0]]);
    fn = length(fn) > 0 ? normalize(fn) : vec3d{0, 0, 1};
    const vec3d n = normalize(smooth[t[0]] * w0 + smooth[t[1]] * u + smooth[t[2]] * v);
    const vec2d uv = m.uvs[t[0]] * w0 + m.uvs[t[1]] * u + m.uvs[t[2]] * v;
    const auto c = sample_texture(tex, uv);
    // Unwritten texels have alpha 0; glass textures (BLEND) carry opacity >= 0.12, which is not missing.
    if (c[3] < 0.05f) ++missing;
    const auto lab = oklab(c);
    if (lab[0] < 0.2f) ++near_black;
    const auto o = view.observe(src, reg.pose, p, n, fn, 0.12, 0.5);
    bool seen_by_extra = false;
    for (size_t k = 0; k < extra_views.size(); ++k) {
      const auto& e = (*extra)[k];
      const auto oe = extra_views[k].observe(*e.source, e.reg.pose, p, n, fn, 0.12, 0.5);
      if (extra_conf(extra_views[k], e, p, n, fn) >= 0.6f) {
        seen_by_extra = true;
        if (o.conf < 0.6f) extra_reproj[k].push_back(de(lab, harmonised(k, oe.rgb)));
      }
    }
    if (o.conf >= 0.6f) {
      ++visible_n;
      near_black_visible += lab[0] < 0.2f;
      const auto slab = oklab(o.rgb);
      source_black_visible += slab[0] < 0.2f;
      reproj.push_back(de(lab, slab));
      lum_visible.push_back(lab[0]);
      if (de(lab, bg_lab) < 0.05 && de(slab, bg_lab) > 0.15) ++bleed;
      // Pixel scale of the texture detail kernel ≈ source kernel: compare at matching footprint.
      const double px_world = 1.0 / reg.pose.scale, tex_world = std::sqrt(total3 / std::max(totaluv * tex.width() * tex.height(), 1e-30));
      const double k = std::clamp(3 * px_world / std::max(tex_world, 1e-12), 1.0, 12.0) / 3.0;
      const double tl = oklab(sample_texture(tex, uv))[0];
      const double tm = 0.25 * (oklab(sample_texture(tex, uv + vec2d{texel_u * k, 0}))[0] + oklab(sample_texture(tex, uv - vec2d{texel_u * k, 0}))[0] +
                                oklab(sample_texture(tex, uv + vec2d{0, texel_v * k}))[0] + oklab(sample_texture(tex, uv - vec2d{0, texel_v * k}))[0]);
      const vec2d q = reg.pose.to_image(p);
      det_tex_v.push_back(tl - tm);
      det_src_v.push_back(src_detail(q.x, q.y));
    } else if (o.facing < -0.3f && !seen_by_extra) {
      lum_hidden.push_back(lab[0]);
      // Through-projection: what the picture shows "in front" of this back-facing point.
      const vec2d q = reg.pose.to_image(p);
      if (q.x >= 4 && q.y >= 4 && q.x < src.width - 4 && q.y < src.height - 4 && src.sample(q.x, q.y)[3] >= 0.9f) {
        const double px_world = 1.0 / reg.pose.scale, tex_world = std::sqrt(total3 / std::max(totaluv * tex.width() * tex.height(), 1e-30));
        const double k = std::clamp(3 * px_world / std::max(tex_world, 1e-12), 1.0, 12.0) / 3.0;
        const double tm = 0.25 * (oklab(sample_texture(tex, uv + vec2d{texel_u * k, 0}))[0] + oklab(sample_texture(tex, uv - vec2d{texel_u * k, 0}))[0] +
                                  oklab(sample_texture(tex, uv + vec2d{0, texel_v * k}))[0] + oklab(sample_texture(tex, uv - vec2d{0, texel_v * k}))[0]);
        dup_tex.push_back(lab[0] - tm);
        dup_src.push_back(src_detail(q.x, q.y));
      }
    }
  }
  // Seams: the same mesh edge sampled from both sides of a UV cut.
  std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, int>>> edges;  // key -> (face, edge)
  edges.reserve(nf * 2);
  std::vector<uint64_t> vkey(geo.positions.size());
  for (size_t i = 0; i < vkey.size(); ++i) vkey[i] = pos_key(geo.positions[i]);
  for (size_t f = 0; f < nf; ++f)
    for (int e = 0; e < 3; ++e) {
      const uint64_t a = vkey[geo.tris[f][size_t(e)]], b = vkey[geo.tris[f][size_t((e + 1) % 3)]];
      edges[std::min(a, b) * 0x9E3779B97F4A7C15ull ^ std::max(a, b)].push_back({uint32_t(f), e});
    }
  std::vector<double> seam;
  size_t seam_edges = 0;
  for (const auto& [key, list] : edges) {
    if (list.size() != 2) continue;
    const auto [f0, e0] = list[0];
    const auto [f1, e1] = list[1];
    const uint32_t a0 = geo.tris[f0][size_t(e0)], b0 = geo.tris[f0][size_t((e0 + 1) % 3)];
    uint32_t a1 = geo.tris[f1][size_t(e1)], b1 = geo.tris[f1][size_t((e1 + 1) % 3)];
    if (vkey[a1] != vkey[a0]) std::swap(a1, b1);
    if (m.uvs[a0] == m.uvs[a1] && m.uvs[b0] == m.uvs[b1]) continue;
    ++seam_edges;
    if (seam_edges % 2) continue;  // half of the seam edges are plenty
    for (double t : {0.25, 0.5, 0.75}) {
      const auto c0 = oklab(sample_texture(tex, m.uvs[a0] * (1 - t) + m.uvs[b0] * t));
      const auto c1 = oklab(sample_texture(tex, m.uvs[a1] * (1 - t) + m.uvs[b1] * t));
      seam.push_back(de(c0, c1));
    }
  }
  // Source statistics for relative darkness.
  size_t src_n = 0, src_black = 0;
  for (int y = 0; y < src.height; y += 2)
    for (int x = 0; x < src.width; x += 2)
      if (src.alpha(x, y) >= 0.5f) {
        ++src_n;
        src_black += oklab(src.px[size_t(y) * src.width + x])[0] < 0.2f;
      }
  const double reproj_mean = mean(reproj), reproj_p90 = percentile(reproj, 0.9);
  const double lv = mean(lum_visible), lh = mean(lum_hidden);
  const double dark_ratio = lum_hidden.size() > 200 && lv > 1e-3 ? lh / lv : 1.0;
  // Darkness is judged where the picture shows the surface (a black cloak may cover more surface than
  // it shows); the reconstruction is judged by the hidden/visible luminance ratio.
  const double black_frac = double(near_black) / N, src_black_frac = src_n ? double(src_black) / src_n : 0;
  const double black_visible = visible_n ? double(near_black_visible) / visible_n : 0;
  const double source_black_visible_frac = visible_n ? double(source_black_visible) / visible_n : 0;
  const double dup_r = pearson(dup_tex, dup_src), detail_r = pearson(det_tex_v, det_src_v);
  const double overlap = claimed ? double(shared) / claimed : 0;
  const double seam_mean = mean(seam), seam_p95 = percentile(seam, 0.95);
  const double bleed_frac = visible_n ? double(bleed) / visible_n : 0;
  const double missing_frac = double(missing) / N + degenerate_area / std::max(total3, 1e-30);
  const double stretched_frac = stretched_area / std::max(total3, 1e-30);
  nlohmann::json issues = nlohmann::json::array();
  std::string verdict = "PASS";
  auto fail = [&](const std::string& s) { issues.push_back("FAIL: " + s); verdict = "FAIL"; };
  auto review = [&](const std::string& s) { issues.push_back("REVIEW: " + s); if (verdict == "PASS") verdict = "REVIEW"; };
  if (missing_frac > 0.02) fail("missing UV/texture coverage on " + std::to_string(int(missing_frac * 100)) + "% of the surface");
  if (overlap > 0.05) fail("UV overlap: " + std::to_string(int(overlap * 100)) + "% of texels are shared by several faces (duplicated texture)");
  else if (overlap > 0.005) review("small UV overlap");
  if (dup_r > 0.6) fail("the picture's front detail is repeated on back surfaces");
  else if (dup_r > 0.35) review("back surfaces partly repeat front detail");
  if (reproj_mean > 0.12) fail("colours differ strongly from the picture where it is visible");
  else if (reproj_mean > 0.07) review("visible colours drift from the picture");
  if (dark_ratio < 0.35) fail("hidden surfaces are abnormally dark");
  else if (dark_ratio < 0.6) review("hidden surfaces are darker than the visible ones");
  // Same surface points, texture vs picture: catches dark/broken materials, not dark objects.
  if (black_visible > source_black_visible_frac + 0.25) fail("texture is much darker than the picture");
  else if (black_visible > source_black_visible_frac + 0.1) review("texture darker than the picture");
  if (stretched_frac > 0.25) fail("texture stretched on " + std::to_string(int(stretched_frac * 100)) + "% of the surface");
  else if (stretched_frac > 0.05) review("texture stretched on part of the surface");
  if (seam_mean > 0.04 || seam_p95 > 0.15) review("visible UV seams");
  if (bleed_frac > 0.02) review("background colour bleeds into the texture");
  nlohmann::json extra_ev = nlohmann::json::array();
  for (size_t k = 0; k < extra_reproj.size(); ++k) {
    const double mk = mean(extra_reproj[k]);
    extra_ev.push_back({{"name", (*extra)[k].name}, {"label", (*extra)[k].label}, {"samples", extra_reproj[k].size()},
                        {"delta_e_mean", mk}, {"delta_e_p90", percentile(extra_reproj[k], 0.9)}});
    if (extra_reproj[k].size() >= 200) {
      if (mk > 0.12) fail("colours differ strongly from the " + (*extra)[k].label + " picture where it is the only picture that shows the surface");
      else if (mk > 0.07) review("colours drift from the " + (*extra)[k].label + " picture");
    }
  }
  const nlohmann::json hidden_side = hidden_side_checks(m, src, reg, extra);
  // Hidden-side gate (spec 2026-10-20-hybrid-texture-design §6). Thresholds were set on the six
  // regression assets (docs/reports/hybrid-texture): REVIEW-level, because without a real back view
  // no image measure is proof; the bake's own provenance decides FAIL (worker, overall verdict).
  // Judged only where the back is mostly inferred: once a real back picture paints most of it, the rest is a strip
  // along depth edges whose correlation swings across the threshold between meshes of the same shape, and real
  // backs resemble their fronts anyway (0.59-0.68 picture to picture; docs/agent/W2-HOODED-BACK.md).
  const double fc_measured = hidden_side.value("front_copy_channels", nlohmann::json::object()).value("measured_fraction", 1.0);
  if (fc_measured >= 0.5 && hidden_side.value("front_copy_correlation", 0.0) >= 0.45)
    review("the back repeats the picture's front features at the same places (possible face or details copied to the back)");
  if (hidden_side.value("fragmentation_ratio", 1.0) < 0.40 && hidden_side.value("colour_edge_rate", 1.0) < 0.10)
    review("hidden sides are large flat colour patches with little of the picture's detail");
  // chart_edge_lift is reported but not judged: TRELLIS albedo carries crease shading and planar charts
  // split at creases, so on the regression set it did not separate good backs from patchwork ones.
  if (hidden_side.value("chroma_ratio", 1.0) < 0.6) review("hidden sides are much greyer than the picture");
  // Patch support (2026-10-21): small patches of the back that appear nowhere in the picture — a face
  // TRELLIS painted on a cloak, dark smudges on clear glass, foreign streaks. On the regression set the
  // flagged pixels sit on exactly those defects; good backs measure 0.00-0.06, the bad ones 0.11-0.26.
  if (const double u = hidden_side.value("unsupported_patch_fraction", 0.0); u >= 0.08)
    review("about " + std::to_string(int(std::lround(u * 100))) +
           "% of the back shows patterns or colours that appear nowhere in the picture (for example a painted face, smudges or streaks)");
  auto clamp01 = [](double x) { return std::clamp(x, 0.0, 1.0); };
  const double score = 0.30 * clamp01(1 - reproj_mean / 0.15) + 0.15 * clamp01(detail_r) + 0.15 * (1 - clamp01(dup_r)) +
                       0.10 * clamp01(1 - seam_mean / 0.08) + 0.10 * clamp01(dark_ratio / 0.8) + 0.10 * clamp01(1 - stretched_frac * 4) +
                       0.10 * (1 - clamp01(overlap * 5));
  return {{"verdict", verdict}, {"issues", issues}, {"score", score},
          {"uv", {{"faces", nf}, {"degenerate_faces", degenerate}, {"out_of_range_vertices", out_of_range}, {"overlap_fraction", overlap},
                  {"stretch_mean", stretch_mean}, {"stretch_p95", percentile(sv, 0.95)}, {"stretched_area_fraction", stretched_frac}}},
          {"coverage_missing_fraction", missing_frac},
          {"seams", {{"edges", seam_edges}, {"delta_e_mean", seam_mean}, {"delta_e_p95", seam_p95}}},
          {"reprojection", {{"samples", reproj.size()}, {"delta_e_mean", reproj_mean}, {"delta_e_p90", reproj_p90}, {"detail_correlation", detail_r}}},
          {"reprojection_extra_pictures", extra_ev},
          {"bleeding_fraction", bleed_frac},
          {"front_to_back_duplication", {{"samples", dup_tex.size()}, {"detail_correlation", dup_r}}},
          {"luminance", {{"visible_mean", lv}, {"hidden_mean", lh}, {"hidden_to_visible", dark_ratio},
                         {"near_black_fraction", black_frac}, {"visible_near_black_fraction", black_visible}, {"source_near_black_at_visible", source_black_visible_frac}, {"source_near_black_fraction", src_black_frac}}},
          {"hidden_side", hidden_side},
          {"texture_size", {tex.width(), tex.height()}}, {"faces_match_geometry", nf == logical.tris.size()},
          {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()},
          {"colour_space", "Oklab; delta_e 0.02 = just noticeable, 0.1 = large"}};
}
}  // namespace p3dasset::autotex
