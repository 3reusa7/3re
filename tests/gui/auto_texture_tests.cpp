// 3re Auto Texture engine: synthetic object with known colours, photographed from a known oblique view;
// hybrid texture (spec 2026-10-20-hybrid-texture-design): a darkened TRELLIS-like voxel volume of the
// whole object under the picture, and the hidden-side gate; texture v2 (material lock, glass opacity,
// outline packing, generated views).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include "auto_texture.hpp"
#include "auto_texture_internal.hpp"
#include "backends/mesh_asset.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/render/camera.hpp"

using namespace p3dasset;
using namespace p3dasset::autotex;
using pixel3d::vec2d;
using pixel3d::vec3d;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  } else {
    std::printf("ok: %s\n", what);
  }
}
// Face colours by outward direction: +X red, -X green, +Y white, -Y grey, +Z blue, -Z yellow.
const pixel3d::Rgba8 kColours[6] = {{220, 30, 30, 255}, {30, 200, 40, 255}, {240, 240, 240, 255}, {90, 90, 90, 255}, {30, 60, 220, 255}, {230, 210, 30, 255}};

struct Scene {
  pixel3d::Mesh logical;          // welded, closed boxes
  pixel3d::TexturedMesh painted;  // per-face palette colours (for making the picture)
  std::vector<int> face_dir;      // logical face -> direction index
};
void add_box(Scene& s, vec3d lo, vec3d hi, int n, std::map<std::array<int64_t, 3>, uint32_t>& weld) {
  auto vertex = [&](vec3d p) {
    const std::array<int64_t, 3> k{std::llround(p.x * 1e6), std::llround(p.y * 1e6), std::llround(p.z * 1e6)};
    auto [it, inserted] = weld.try_emplace(k, uint32_t(s.logical.positions.size()));
    if (inserted) s.logical.positions.push_back(p);
    return it->second;
  };
  for (int dir = 0; dir < 6; ++dir) {
    const int axis = dir / 2;
    const bool positive = dir % 2 == 0;
    const int ua = (axis + 1) % 3, va = (axis + 2) % 3;
    auto point = [&](double u, double v) {
      double c[3];
      const double l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
      c[axis] = positive ? h[axis] : l[axis];
      c[ua] = l[ua] + (h[ua] - l[ua]) * u;
      c[va] = l[va] + (h[va] - l[va]) * v;
      return vec3d{c[0], c[1], c[2]};
    };
    for (int j = 0; j < n; ++j)
      for (int i = 0; i < n; ++i) {
        const double u0 = double(i) / n, u1 = double(i + 1) / n, v0 = double(j) / n, v1 = double(j + 1) / n;
        const vec3d q[4] = {point(u0, v0), point(u1, v0), point(u1, v1), point(u0, v1)};
        // (ua, va, axis) is right-handed, so u x v points along +axis: CCW from outside on positive faces.
        const int order[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& o : order) {
          std::array<uint32_t, 3> t{vertex(q[o[0]]), vertex(q[o[1]]), vertex(q[o[2]])};
          if (!positive) std::swap(t[1], t[2]);
          s.logical.tris.push_back(t);
          s.face_dir.push_back(dir);
        }
      }
  }
}
// Paints every logical face with palette entry colour_of(face) (uv at that palette pixel's centre).
void paint(Scene& s, const std::vector<pixel3d::Rgba8>& palette_colours, const std::function<int(size_t)>& colour_of) {
  auto palette = std::make_shared<pixel3d::ImageRGBA>(int(palette_colours.size()), 1);
  for (size_t i = 0; i < palette_colours.size(); ++i) palette->at(int(i), 0) = palette_colours[i];
  s.painted = {};
  for (size_t f = 0; f < s.logical.tris.size(); ++f)
    for (auto v : s.logical.tris[f]) {
      s.painted.indices.push_back(uint32_t(s.painted.positions.size()));
      s.painted.positions.push_back(s.logical.positions[v]);
      s.painted.normals.push_back({0, 0, 1});
      s.painted.uvs.push_back({(colour_of(f) + 0.5) / double(palette_colours.size()), 0.5});
    }
  s.painted.texture = palette;
}
Scene make_scene(double scale = 1) {
  Scene s;
  std::map<std::array<int64_t, 3>, uint32_t> weld;
  add_box(s, vec3d{-0.5, -0.5, -0.2} * scale, vec3d{0.5, 0.5, 0.2} * scale, 24, weld);
  add_box(s, vec3d{0.25, 0.5, -0.2} * scale, vec3d{0.5, 1.0, 0.0} * scale, 12, weld);   // tower, top right back
  add_box(s, vec3d{-0.5, -0.5, 0.2} * scale, vec3d{-0.15, -0.2, 0.6} * scale, 12, weld);  // step, bottom left front
  paint(s, std::vector<pixel3d::Rgba8>(kColours, kColours + 6), [&](size_t f) { return s.face_dir[f]; });
  return s;
}
pixel3d::ImageRGBA photograph(const Scene& s, double yaw, double el, bool transparent, double scale = 220, vec3d centre = {0, 0.1, 0}) {
  const Pose p = Pose::make(yaw, el);
  pixel3d::Camera c;
  c.forward = p.forward;
  c.right = p.right;
  c.up = p.up;
  c.eye = centre - c.forward * 3;
  c.width = c.height = 480;
  c.scale = scale;
  c.offset = {240, 240};
  pixel3d::RenderOptions o;
  o.background = transparent ? pixel3d::Rgba8{0, 0, 0, 0} : pixel3d::Rgba8{20, 20, 20, 255};
  return pixel3d::render(s.painted, c, o).color;
}
double colour_distance(const std::array<float, 4>& a, pixel3d::Rgba8 b) {
  const auto la = oklab(a), lb = oklab({linear_from_srgb8(b.r), linear_from_srgb8(b.g), linear_from_srgb8(b.b), 1});
  return std::sqrt(double(la[0] - lb[0]) * (la[0] - lb[0]) + double(la[1] - lb[1]) * (la[1] - lb[1]) + double(la[2] - lb[2]) * (la[2] - lb[2]));
}
std::array<float, 4> texel(const pixel3d::TexturedMesh& m, size_t face) {  // colour at the face centre
  const vec2d uv = (m.uvs[m.indices[3 * face]] + m.uvs[m.indices[3 * face + 1]] + m.uvs[m.indices[3 * face + 2]]) * (1.0 / 3);
  const auto p = m.texture->at(std::clamp(int(uv.x * m.texture->width()), 0, m.texture->width() - 1),
                               std::clamp(int(uv.y * m.texture->height()), 0, m.texture->height() - 1));
  return {linear_from_srgb8(p.r), linear_from_srgb8(p.g), linear_from_srgb8(p.b), 1};
}
}  // namespace

int main() {
  const Scene scene = make_scene();
  const auto picture = photograph(scene, 60, 25, true);
  const Source src = Source::from(picture, false);
  // One deadline for every section below (they grew with each milestone): 120 s was crossed under memory pressure
  // on the 16 GB laptop (2026-10-31, camera registration in a late section). The workers' own budgets are unchanged.
  const auto deadline = Deadline::in_seconds(300);

  // 1. Registration recovers the oblique viewpoint (the object is not the TRELLIS front).
  const auto reg = register_source(scene.logical, src, deadline);
  std::printf("registered yaw %.2f elevation %.2f iou %.3f\n", reg.pose.yaw, reg.pose.elevation, reg.iou);
  check(std::abs(reg.pose.yaw - 60) < 4 && std::abs(reg.pose.elevation - 25) < 4, "registration finds the picture's viewpoint");
  check(reg.iou > 0.9 && reg.silhouette_usable, "silhouette fits");

  // 2. Atlas + bake: seen faces carry the picture's colours, hidden faces never get them copied.
  const auto atlas = build_atlas(scene.logical, 1024, 0, deadline);
  check(atlas.mesh.indices.size() == scene.logical.tris.size() * 3, "atlas keeps every face");
  const auto b = bake(scene.logical, atlas, src, reg, {}, deadline);
  double seen_error = 0, seen_n = 0;
  size_t hidden_copies = 0, hidden_n = 0;
  for (size_t f = 0; f < scene.logical.tris.size(); ++f) {
    const int dir = scene.face_dir[f];
    const auto c = texel(b.mesh, f);
    if (dir == 0 || dir == 2 || dir == 4) {  // +X, +Y, +Z face the camera at yaw 60 / elevation 25
      seen_error += colour_distance(c, kColours[dir]);
      seen_n += 1;
    } else {
      ++hidden_n;
      // A hidden face showing its own true colour would mean information the picture does not contain.
      if (colour_distance(c, kColours[dir]) < 0.05) ++hidden_copies;
    }
  }
  std::printf("seen mean dE %.4f, hidden faces with an unseen colour %zu/%zu\n", seen_error / seen_n, hidden_copies, hidden_n);
  check(seen_error / seen_n < 0.06, "visible faces carry the picture's colours");
  check(hidden_copies == 0, "hidden faces are reconstructed from seen colours only");

  // 3. Quality checks: the auto texture passes, a front-stamped (XY) projection fails on overlap.
  const auto q = evaluate(scene.logical, b.mesh, src, reg);
  std::printf("auto verdict %s score %.3f unsupported %.3f\n", q["verdict"].get<std::string>().c_str(), q["score"].get<double>(), q["hidden_side"].value("unsupported_patch_fraction", 0.0));
  for (const auto& i : q["issues"]) std::printf("  %s\n", i.get<std::string>().c_str());
  check(q["verdict"] != "FAIL" && q["uv"]["overlap_fraction"].get<double>() < 0.005 && q["coverage_missing_fraction"].get<double>() < 0.01,
        "auto texture passes the UV and coverage checks");
  check(q["seams"]["delta_e_mean"].get<double>() < 0.04, "no visible seams");
  pixel3d::TexturedMesh legacy;
  legacy.positions = scene.logical.positions;
  legacy.normals.assign(legacy.positions.size(), {0, 0, 1});
  for (auto p : legacy.positions) legacy.uvs.push_back({std::clamp((p.x + 0.5), 0.0, 1.0), std::clamp((1.0 - p.y) / 1.5, 0.0, 1.0)});
  for (const auto& t : scene.logical.tris) legacy.indices.insert(legacy.indices.end(), t.begin(), t.end());
  legacy.texture = b.mesh.texture;
  const auto lq = evaluate(scene.logical, legacy, src, reg);
  check(lq["verdict"] == "FAIL" && lq["uv"]["overlap_fraction"].get<double>() > 0.3, "front-stamped projection is detected as duplicated");

  // 4. GLB: geometry identical after re-import, material readable, provenance kept.
  const auto dir = std::filesystem::temp_directory_path() / "threere-auto-texture-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto glb = dir / "textured.glb";
  {
    const auto bytes = write_glb(scene.logical, b.mesh, {{"kind", "auto_texture"}});
    std::ofstream(glb, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  }
  check(geometrically_identical(load_geometry_glb(glb), scene.logical), "GLB re-import: geometry identical");
  const auto info = material_info(glb);
  check(info["kind"] == "auto_texture" && load_generated_appearance(glb) != nullptr, "GLB re-import: material and provenance");

  // 5. Budgets and degenerate pictures.
  bool budget = false;
  try { build_atlas(scene.logical, 1024, 0, Deadline::in_seconds(-1)); } catch (const BudgetExceeded&) { budget = true; }
  check(budget, "time budget stops a stage");
  const auto opaque = Source::from(photograph(scene, 60, 25, false), false);
  const auto r2 = register_source(scene.logical, opaque, deadline);
  check(!r2.silhouette_usable && r2.pose.yaw == 0, "a picture without transparency falls back to the front prior and says so");
  // 6. Hybrid texture: a TRELLIS-like volume of the whole object (every side, true colours but 65% darker,
  //    as the engine's albedo comes out) under the picture. Scaled into TRELLIS's unit grid.
  {
    const Scene small = make_scene(0.4);
    const auto pic = photograph(small, 60, 25, true, 550, {0, 0.04, 0});
    const Source s6 = Source::from(pic, false);
    const auto r6 = register_source(small.logical, s6, deadline);
    constexpr int res = 256;
    std::map<std::array<int, 3>, std::array<float, 3>> vox;
    auto voxel_of = [&](vec3d g) {  // GLB (x, y, z) is TRELLIS (x, -z, y)
      const double t[3] = {g.x, -g.z, g.y};
      return std::array<int, 3>{int(std::floor((t[0] + 0.5) * res)), int(std::floor((t[1] + 0.5) * res)), int(std::floor((t[2] + 0.5) * res))};
    };
    for (size_t f = 0; f < small.logical.tris.size(); ++f) {
      const auto& t = small.logical.tris[f];
      const auto c = kColours[small.face_dir[f]];
      const std::array<float, 3> dark{srgb8_from_linear(linear_from_srgb8(c.r) * 0.35f) / 255.f, srgb8_from_linear(linear_from_srgb8(c.g) * 0.35f) / 255.f,
                                      srgb8_from_linear(linear_from_srgb8(c.b) * 0.35f) / 255.f};
      for (int i = 0; i <= 6; ++i)
        for (int j = 0; i + j <= 6; ++j) {
          const double u = i / 6.0, v = j / 6.0;
          vox[voxel_of(small.logical.positions[t[0]] * (1 - u - v) + small.logical.positions[t[1]] * u + small.logical.positions[t[2]] * v)] = dark;
        }
    }
    std::vector<std::array<int, 3>> coords;
    std::vector<std::array<float, 3>> colours;
    for (const auto& [k, c] : vox) { coords.push_back(k); colours.push_back(c); }
    const NativeVolume volume = NativeVolume::from_voxels(res, coords, colours);
    const auto a6 = build_atlas(small.logical, 1024, 0, deadline);
    BakeOptions opt;
    opt.native = &volume;
    const auto h = bake(small.logical, a6, s6, r6, opt, deadline);
    check(h.evidence["native"]["used"] == true && h.evidence["native_fraction"].get<double>() > 0.4, "hybrid: the TRELLIS volume colours the hidden sides");
    // The picture is a lit render, so calibration also learns its lighting: a hidden face must show its own
    // colour (nearest of the six), never a neighbour's, and stay close to it.
    double seen = 0, seen_n = 0, hidden_e = 0;
    size_t hidden_own = 0, hidden_all = 0;
    for (size_t f = 0; f < small.logical.tris.size(); ++f) {
      const int d = small.face_dir[f];
      const auto c = texel(h.mesh, f);
      const double e = colour_distance(c, kColours[d]);
      if (d == 0 || d == 2 || d == 4) {
        seen += e;
        seen_n += 1;
        continue;
      }
      ++hidden_all;
      hidden_e += e;
      int nearest = 0;
      for (int k = 1; k < 6; ++k)
        if (colour_distance(c, kColours[k]) < colour_distance(c, kColours[nearest])) nearest = k;
      hidden_own += nearest == d;
    }
    std::printf("hybrid: seen dE %.4f, hidden faces showing their own colour %zu/%zu (mean dE %.3f), calibration %s\n", seen / seen_n, hidden_own,
                hidden_all, hidden_e / double(hidden_all), h.evidence["native"]["calibration"]["method"].get<std::string>().c_str());
    check(seen / seen_n < 0.06, "hybrid: visible faces still carry the picture's colours");
    check(hidden_own >= hidden_all * 95 / 100 && hidden_e / double(hidden_all) < 0.15,
          "hybrid: hidden sides show their own (unseen) colours, calibrated from the dark volume");
    // A volume from another frame (shifted 40 voxels) must not be forced onto this geometry.
    std::vector<std::array<int, 3>> shifted = coords;
    for (auto& c : shifted) c[0] = std::min(res - 1, c[0] + 40);
    const NativeVolume wrong = NativeVolume::from_voxels(res, shifted, colours);
    opt.native = &wrong;
    const auto w = bake(small.logical, a6, s6, r6, opt, deadline);
    check(w.evidence["native"]["used"] == false && w.evidence["native_fraction"].get<double>() == 0.0, "hybrid: a volume that does not line up is rejected");
    // The engine's POST dump layout: header, mesh, coords, pbr6; a truncated file is refused.
    const auto pdir = std::filesystem::temp_directory_path() / "threere-hybrid-post-test";
    std::filesystem::remove_all(pdir);
    std::filesystem::create_directories(pdir);
    {
      std::ofstream out(pdir / "native.post", std::ios::binary);
      const int32_t head[4]{0, 0, int32_t(coords.size()), res};
      out.write(reinterpret_cast<const char*>(head), 16);
      for (const auto& c : coords) out.write(reinterpret_cast<const char*>(c.data()), 12);
      for (const auto& c : colours) {
        const float row[6]{c[0], c[1], c[2], 0.f, 1.f, 1.f};
        out.write(reinterpret_cast<const char*>(row), 24);
      }
    }
    const auto loaded = NativeVolume::load_post(pixel3d::to_utf8(pdir / "native.post"));
    check(loaded.res == res && loaded.linear.size() == coords.size() && loaded.evidence.contains("smoothing"), "POST dump loads (and is low-passed)");
    std::filesystem::resize_file(pdir / "native.post", std::filesystem::file_size(pdir / "native.post") - 7);
    bool refused = false;
    try { NativeVolume::load_post(pixel3d::to_utf8(pdir / "native.post")); } catch (const std::exception&) { refused = true; }
    check(refused, "truncated POST dump is refused");
    std::filesystem::remove_all(pdir);
  }

  // 7. Hidden-side gate: a thin slab whose back repeats the front's disc ("Janus") vs a plain back.
  {
    Scene slab;
    std::map<std::array<int64_t, 3>, uint32_t> weld;
    add_box(slab, {-0.4, -0.5, -0.02}, {0.4, 0.5, 0.02}, 32, weld);
    auto in_disc = [&](size_t f) {
      const auto& t = slab.logical.tris[f];
      const vec3d c = (slab.logical.positions[t[0]] + slab.logical.positions[t[1]] + slab.logical.positions[t[2]]) * (1.0 / 3);
      return std::hypot(c.x - 0.1, c.y - 0.15) < 0.14;
    };
    const std::vector<pixel3d::Rgba8> pal{{70, 60, 160, 255}, {235, 190, 150, 255}};
    paint(slab, pal, [&](size_t f) { return (slab.face_dir[f] == 4 || slab.face_dir[f] == 5) && in_disc(f) ? 1 : 0; });
    const pixel3d::TexturedMesh janus = slab.painted;
    const auto spic = photograph(slab, 0, 0, true, 380, {0, 0, 0});
    const Source s7 = Source::from(spic, false);
    const auto r7 = register_source(slab.logical, s7, deadline);
    paint(slab, pal, [&](size_t f) { return slab.face_dir[f] == 4 && in_disc(f) ? 1 : 0; });
    const pixel3d::TexturedMesh plain = slab.painted;
    const auto qj = evaluate(slab.logical, janus, s7, r7), qp = evaluate(slab.logical, plain, s7, r7);
    const double cj = qj["hidden_side"]["front_copy_correlation"], cp = qp["hidden_side"]["front_copy_correlation"];
    std::printf("front copy: janus %.3f, plain back %.3f\n", cj, cp);
    check(cj >= 0.45 && qj["verdict"] != "PASS", "gate: the front's feature repeated on the back is flagged");
    check(cp < 0.45, "gate: a plain back is not flagged as a front copy");
    check(qp["hidden_side"].contains("chart_edge_lift") && qp["hidden_side"].contains("fragmentation_ratio") && qp["hidden_side"].contains("chroma_ratio"),
          "gate: hidden-side measures are reported");
    check(qj["hidden_side"]["front_copy_channels"].value("measured_fraction", 0.0) >= 0.5, "gate: without a back picture the front-copy measure sees most of the back");

    // 8. Patch support: the same slab with a back of blotches in colours the picture never shows
    // (smudges / a painted pattern) against the plain back.
    const std::vector<pixel3d::Rgba8> pal8{{70, 60, 160, 255}, {235, 190, 150, 255}, {20, 150, 60, 255}, {15, 15, 15, 255}};
    paint(slab, pal8, [&](size_t f) {
      if (slab.face_dir[f] == 4) return in_disc(f) ? 1 : 0;
      if (slab.face_dir[f] != 5) return 0;
      const auto& t = slab.logical.tris[f];
      const vec3d c = (slab.logical.positions[t[0]] + slab.logical.positions[t[1]] + slab.logical.positions[t[2]]) * (1.0 / 3);
      const int bx = int(std::floor((c.x + 0.4) / 0.1)), by = int(std::floor((c.y + 0.5) / 0.1));
      return (bx + by) % 3 == 0 ? 2 : (bx * 7 + by * 3) % 5 == 0 ? 3 : 0;
    });
    const auto qb = evaluate(slab.logical, slab.painted, s7, r7);
    const double ub = qb["hidden_side"]["unsupported_patch_fraction"], up = qp["hidden_side"]["unsupported_patch_fraction"];
    std::printf("unsupported patches: blotched back %.3f, plain back %.3f\n", ub, up);
    check(ub >= 0.08 && qb["verdict"] != "PASS", "gate: a back with patterns the picture never shows is flagged");
    check(up < 0.08, "gate: a plain back in the picture's own colour is supported by the picture");
  }
  // 9. Texture v2 (2.3): material lock, glass opacity, outline packing, generated views.
  {
    const Scene small = make_scene(0.4);
    const auto pic = photograph(small, 60, 25, true, 550, {0, 0.04, 0});
    const Source s9 = Source::from(pic, false);
    const auto r9 = register_source(small.logical, s9, deadline);
    constexpr int res = 256;
    auto voxel_of = [&](vec3d g) {
      const double t[3] = {g.x, -g.z, g.y};
      return std::array<int, 3>{int(std::floor((t[0] + 0.5) * res)), int(std::floor((t[1] + 0.5) * res)), int(std::floor((t[2] + 0.5) * res))};
    };
    // The main box's back (-Z, yellow, never seen) carries a red blotch in its middle: a colour of another
    // part (+X), not connected to it — the "orange streaks in black hair" case.
    auto in_blotch = [&](vec3d c) { return std::abs(c.z + 0.08) < 1e-6 && c.x * c.x + c.y * c.y < 0.06 * 0.06; };
    std::map<std::array<int, 3>, std::array<float, 3>> vox;
    for (size_t f = 0; f < small.logical.tris.size(); ++f) {
      const auto& t = small.logical.tris[f];
      const vec3d centre = (small.logical.positions[t[0]] + small.logical.positions[t[1]] + small.logical.positions[t[2]]) * (1.0 / 3);
      const auto c = in_blotch(centre) ? kColours[0] : kColours[small.face_dir[f]];
      const std::array<float, 3> dark{srgb8_from_linear(linear_from_srgb8(c.r) * 0.35f) / 255.f, srgb8_from_linear(linear_from_srgb8(c.g) * 0.35f) / 255.f,
                                      srgb8_from_linear(linear_from_srgb8(c.b) * 0.35f) / 255.f};
      for (int i = 0; i <= 6; ++i)
        for (int j = 0; i + j <= 6; ++j) {
          const double u = i / 6.0, v = j / 6.0;
          vox[voxel_of(small.logical.positions[t[0]] * (1 - u - v) + small.logical.positions[t[1]] * u + small.logical.positions[t[2]] * v)] = dark;
        }
    }
    std::vector<std::array<int, 3>> coords;
    std::vector<std::array<float, 3>> colours;
    for (const auto& [k, c] : vox) { coords.push_back(k); colours.push_back(c); }
    NativeVolume volume = NativeVolume::from_voxels(res, coords, colours);
    const auto a9 = build_atlas(small.logical, 1024, 0, deadline);
    check(a9.metrics.contains("packer") && a9.metrics["utilization"].get<double>() > 0.2, "atlas: packer and utilization are reported");
    auto blotch_faces = [&](const pixel3d::TexturedMesh& m, size_t& red, size_t& yellow) {
      red = yellow = 0;
      for (size_t f = 0; f < small.logical.tris.size(); ++f) {
        const auto& t = small.logical.tris[f];
        const vec3d centre = (small.logical.positions[t[0]] + small.logical.positions[t[1]] + small.logical.positions[t[2]]) * (1.0 / 3);
        if (!in_blotch(centre)) continue;
        const auto c = texel(m, f);
        (colour_distance(c, kColours[0]) < colour_distance(c, kColours[5]) ? red : yellow) += 1;
      }
    };
    BakeOptions off;
    off.native = &volume;
    off.material_lock = false;
    const auto unlocked = bake(small.logical, a9, s9, r9, off, deadline);
    BakeOptions on = off;
    on.material_lock = true;
    const auto locked = bake(small.logical, a9, s9, r9, on, deadline);
    size_t red0, yellow0, red1, yellow1;
    blotch_faces(unlocked.mesh, red0, yellow0);
    blotch_faces(locked.mesh, red1, yellow1);
    std::printf("material lock: blotch faces red/yellow without %zu/%zu, with %zu/%zu; %s\n", red0, yellow0, red1, yellow1,
                locked.evidence["native"]["material_lock"].dump().c_str());
    check(red0 > yellow0, "material lock: the test blotch is there without the lock");
    check(yellow1 > 3 * red1, "material lock: a blotch of another part's colour takes the surrounding colour");
    // The unseen faces themselves (-X green, -Y grey, -Z yellow: colours the picture never shows) stay TRELLIS's:
    // as many show their own colour as without the lock.
    auto own_colour = [&](const pixel3d::TexturedMesh& m, size_t& all) {
      size_t own = 0;
      all = 0;
      for (size_t f = 0; f < small.logical.tris.size(); ++f) {
        const int d = small.face_dir[f];
        if (d == 0 || d == 2 || d == 4) continue;
        const auto& t = small.logical.tris[f];
        if (in_blotch((small.logical.positions[t[0]] + small.logical.positions[t[1]] + small.logical.positions[t[2]]) * (1.0 / 3))) continue;
        const auto c = texel(m, f);
        int nearest = 0;
        for (int k = 1; k < 6; ++k)
          if (colour_distance(c, kColours[k]) < colour_distance(c, kColours[nearest])) nearest = k;
        own += nearest == d;
        ++all;
      }
      return own;
    };
    size_t all = 0;
    const size_t own = own_colour(locked.mesh, all), own_unlocked = own_colour(unlocked.mesh, all);
    std::printf("material lock: unseen faces keeping their own colour %zu/%zu (without the lock %zu)\n", own, all, own_unlocked);
    check(own + all / 50 >= own_unlocked && own >= all * 9 / 10, "material lock: large regions in colours the picture never shows are kept as TRELLIS painted them");

    // Glass: a translucent volume gives a BLEND material; the preview loader accepts it, coverage is not missing.
    volume.alpha.assign(volume.linear.size(), 0.4f);
    volume.translucent = true;
    const auto glass = bake(small.logical, a9, s9, r9, on, deadline);
    check(glass.evidence.value("translucent", false) && glass.mesh.texture->at(glass.mesh.texture->width() / 2, glass.mesh.texture->height() / 2).a <= 255,
          "glass: the bake carries TRELLIS's opacity");
    const auto gq = evaluate(small.logical, glass.mesh, s9, r9);
    check(gq["coverage_missing_fraction"].get<double>() < 0.01, "glass: opacity is not mistaken for missing texture");
    const auto gdir = std::filesystem::temp_directory_path() / "threere-glass-test";
    std::filesystem::remove_all(gdir);
    std::filesystem::create_directories(gdir);
    {
      const auto bytes = write_glb(small.logical, glass.mesh, {{"kind", "auto_texture"}});
      std::ofstream(gdir / "glass.glb", std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    const auto ginfo = material_info(gdir / "glass.glb");
    check(ginfo["alpha_mode"] == "BLEND" && load_generated_appearance(gdir / "glass.glb") != nullptr, "glass: BLEND GLB re-imports");
    std::filesystem::remove_all(gdir);

    // Generated views: controls in the adapter's conventions, and views rendered through the same rig put every
    // hidden face's own colour back on it (no native volume: the views alone).
    const auto mdir = std::filesystem::temp_directory_path() / "threere-multiview-test";
    std::filesystem::remove_all(mdir);
    const auto ctl = MultiViewSet::export_controls(small.logical, r9, pixel3d::to_utf8(mdir), 256);
    check(ctl["views"].size() == 6 && std::filesystem::exists(mdir / "pos_0.png") && std::filesystem::exists(mdir / "nrm_5.png"), "multi-view: six control image pairs");
    {  // Front control: normals facing the camera are (0, -1, 0) in the adapter's frame -> purple (128, 0, 128).
      const auto nrm = pixel3d::load_png(mdir / "nrm_0.png");
      long purple = 0, object = 0;
      for (int y = 0; y < nrm->height(); ++y)
        for (int x = 0; x < nrm->width(); ++x) {
          const auto p = nrm->at(x, y);
          if (p.r == 128 && p.g == 128 && p.b == 128) continue;
          ++object;
          purple += p.g < 60 && p.r > 70 && p.b > 70;
        }
      check(object > 0 && purple > object / 4, "multi-view: front normals use the adapter's frame");
    }
    const auto rig = MultiViewSet::rig(small.logical, r9, 256);
    for (size_t k = 0; k < rig.size(); ++k) {
      pixel3d::Camera c;
      c.forward = rig[k].forward;
      c.right = rig[k].right;
      c.up = rig[k].up;
      c.eye = vec3d{0, 0, 0} - c.forward * 3;
      c.width = c.height = 256;
      c.scale = rig[k].scale * 256 / 1.1;
      c.offset = {128, 128};
      pixel3d::RenderOptions o;
      o.background = {128, 128, 128, 255};
      const auto view = pixel3d::render(small.painted, c, o).color;
      const auto bytes = pixel3d::encode_png(view);
      std::ofstream(mdir / ("view_" + std::to_string(k) + ".png"), std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    const auto views = MultiViewSet::load(small.logical, r9, pixel3d::to_utf8(mdir));
    BakeOptions mv;
    mv.multiview = &views;
    const auto withviews = bake(small.logical, a9, s9, r9, mv, deadline);
    // Where the views own the texel (provenance: generated view), the face shows its own colour. Strips at the
    // views' silhouettes are left to the other layers on purpose (generated pixels there are unreliable).
    size_t vown = 0, vall = 0;
    const auto& prov = withviews.provenance;
    for (size_t f = 0; f < small.logical.tris.size(); ++f) {
      const int d = small.face_dir[f];
      if (d == 0 || d == 2 || d == 4) continue;
      const auto& m = withviews.mesh;
      const vec2d uv = (m.uvs[m.indices[3 * f]] + m.uvs[m.indices[3 * f + 1]] + m.uvs[m.indices[3 * f + 2]]) * (1.0 / 3);
      const auto p = prov.at(std::clamp(int(uv.x * prov.width()), 0, prov.width() - 1), std::clamp(int(uv.y * prov.height()), 0, prov.height() - 1));
      if (!(p.r == 190 && p.g == 90 && p.b == 230)) continue;
      const auto c = texel(m, f);
      int nearest = 0;
      for (int k = 1; k < 6; ++k)
        if (colour_distance(c, kColours[k]) < colour_distance(c, kColours[nearest])) nearest = k;
      vown += nearest == d;
      ++vall;
    }
    std::printf("multi-view: hidden faces owned by the views showing their own colour %zu/%zu, generated-view share %.2f\n", vown, vall,
                withviews.evidence["multiview_fraction"].get<double>());
    check(vall > 2000 && vown >= vall * 97 / 100 && withviews.evidence["multiview_fraction"].get<double>() > 0.3, "multi-view: generated views land on the right faces");
    std::filesystem::remove_all(mdir);
  }
  // 12. Multi-angle pictures (2.3): the object photographed from four angles, every side a different colour.
  //     Each side is seen by two pictures (three-quarter views), the back picture is under-exposed: the
  //     hidden sides of the front get their exact colours, the overlaps blend without seams, a wrong angle
  //     label is caught and "auto" finds the angle.
  {
    const Scene ms = make_scene(0.8);
    const auto mdl = Deadline::in_seconds(300);
    auto shoot = [&](double yaw, double exposure) {
      auto img = photograph(ms, yaw, 15, true, 200);
      for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
          auto& q = img.at(x, y);
          q.r = srgb8_from_linear(linear_from_srgb8(q.r) * float(exposure));
          q.g = srgb8_from_linear(linear_from_srgb8(q.g) * float(exposure));
          q.b = srgb8_from_linear(linear_from_srgb8(q.b) * float(exposure));
        }
      return Source::from(img, false);
    };
    const Source mfront = shoot(45, 1.0), mleft = shoot(135, 1.0), mback = shoot(225, 0.75), mright = shoot(315, 1.0);
    const auto mreg = register_source(ms.logical, mfront, mdl);
    check(std::abs(mreg.pose.yaw - 45) < 4, "multi-angle: front picture registered");
    std::vector<ExtraInput> in(3);
    in[0] = {"left.png", "left", 0, false, &mleft};
    in[1] = {"back.png", "back", 0, false, &mback};
    in[2] = {"right.png", "auto", 0, false, &mright};
    nlohmann::json vr;
    const auto views = register_extra_views(ms.logical, mfront, mreg, in, mdl, vr);
    for (const auto& v : vr)
      std::printf("  view %s (%s): %s fit %.3f yaw %.1f %s\n", v.value("name", std::string()).c_str(), v.value("label", std::string()).c_str(),
                  v.value("status", std::string()).c_str(), v.value("fit", 0.0), v.value("yaw_from_front", 0.0), v.value("reason", std::string()).c_str());
    check(views.size() == 3, "multi-angle: all three extra pictures accepted");
    bool auto_ok = false;
    for (const auto& v : views)
      if (v.name == "right.png") auto_ok = std::abs(std::fmod(v.reg.pose.yaw - 315 + 540, 360) - 180) < 5;
    check(auto_ok, "multi-angle: 'auto' finds the right picture's angle");
    const auto ma = build_atlas(ms.logical, 1024, 0, mdl);
    BakeOptions mo;
    mo.extra_views = &views;
    const auto mb = bake(ms.logical, ma, mfront, mreg, mo, mdl);
    const auto single = bake(ms.logical, ma, mfront, mreg, {}, mdl);
    // Main box faces only (the first 24x24x2x6 triangles) that a picture shows (faces under the step are
    // inside the object), per side: share of faces within dE 0.04 of the true colour, and the 95th percentile
    // difference (a seam or an exposure step shows up there).
    const size_t main_faces = 24 * 24 * 2 * 6;
    const auto pics = detail::Pictures::make(ms.logical, mfront, mreg, &views);
    std::vector<uint8_t> visible(main_faces, 0);
    for (size_t f = 0; f < main_faces; ++f) {
      const auto& t = ms.logical.tris[f];
      const vec3d c = (ms.logical.positions[t[0]] + ms.logical.positions[t[1]] + ms.logical.positions[t[2]]) * (1.0 / 3);
      const vec3d n = pixel3d::normalize(pixel3d::cross(ms.logical.positions[t[1]] - ms.logical.positions[t[0]], ms.logical.positions[t[2]] - ms.logical.positions[t[0]]));
      visible[f] = pics.observe(c, n, n, 0.12, 0.5, nullptr, false).o.conf >= 0.5f;
    }
    double worst_share = 1, worst_p95 = 0, single_back_share = 0;
    for (int d : {0, 1, 4, 5}) {  // +X, -X, +Z, -Z
      std::vector<double> e;
      size_t good = 0, good_single = 0;
      for (size_t f = 0; f < main_faces; ++f) {
        if (ms.face_dir[f] != d || !visible[f]) continue;
        const double de = colour_distance(texel(mb.mesh, f), kColours[d]);
        e.push_back(de);
        good += de < 0.04;
        good_single += colour_distance(texel(single.mesh, f), kColours[d]) < 0.04;
      }
      std::sort(e.begin(), e.end());
      const double share = double(good) / double(e.size()), p95 = e[e.size() * 95 / 100];
      std::printf("  side %d: exact %.3f p95 dE %.4f (front picture alone: exact %.3f)\n", d, share, p95, double(good_single) / double(e.size()));
      worst_share = std::min(worst_share, share);
      worst_p95 = std::max(worst_p95, p95);
      if (d == 5) single_back_share = double(good_single) / double(e.size());
    }
    check(worst_share >= 0.97, "multi-angle: every side carries its exact colour (the back from the under-exposed back picture)");
    check(worst_p95 < 0.04, "multi-angle: no seams or exposure steps inside a side");
    check(single_back_share < 0.05, "multi-angle: without the extra pictures the back colour is unknown (control)");
    const auto& own = mb.evidence["picture_ownership"];
    std::printf("  ownership %s\n", own.dump().c_str());
    check(own.value("not_seen_by_any_picture", 1.0) < 0.35 && own.value("back", 0.0) > 0.1, "multi-angle: ownership reported per picture");
    const auto mq = evaluate(ms.logical, mb.mesh, mfront, mreg, &views);
    std::printf("  verdict %s, back seen by extra pictures %.2f\n", mq["verdict"].get<std::string>().c_str(), mq["hidden_side"].value("back_seen_by_extra_pictures_fraction", 0.0));
    for (const auto& i : mq["issues"]) std::printf("    %s\n", i.get<std::string>().c_str());
    check(mq["hidden_side"].value("back_seen_by_extra_pictures_fraction", 0.0) > 0.8, "multi-angle: a back shown by a real picture counts as seen");
    // W2 (milestone 4): once a real back picture paints the back, the front-copy measure is left with a strip and is
    // not judged (the hooded character's REVIEW/PASS split); front-only, it sees most of the back and is judged.
    {
      const double with_back = mq["hidden_side"]["front_copy_channels"].value("measured_fraction", 1.0);
      bool flagged = false;
      for (const auto& i : mq["issues"]) flagged |= i.get<std::string>().find("repeats the picture's front") != std::string::npos;
      const auto sq = evaluate(ms.logical, single.mesh, mfront, mreg);
      const double front_only = sq["hidden_side"]["front_copy_channels"].value("measured_fraction", 0.0);
      std::printf("  front-copy measured fraction: with the back picture %.3f, front only %.3f\n", with_back, front_only);
      check(with_back < 0.5 && !flagged, "gate: a back painted from a real back picture is not judged as a front copy");
      check(front_only >= 0.5, "gate: front only, the front-copy measure sees most of the back");
    }
    check(mq["seams"]["delta_e_mean"].get<double>() < 0.04 && mq["verdict"] != "FAIL", "multi-angle: quality checks pass");
    // Wrong labels: the left picture labelled as the right one (mirror view), and as the back.
    for (const char* wrong : {"right", "back"}) {
      std::vector<ExtraInput> bad{{"left.png", wrong, 0, false, &mleft}};
      nlohmann::json br;
      const auto bv = register_extra_views(ms.logical, mfront, mreg, bad, mdl, br);
      std::printf("  left picture labelled %s: %s (%s)\n", wrong, br[0].value("status", std::string()).c_str(), br[0].value("reason", std::string()).c_str());
      check(bv.empty() && br[0].value("status", std::string()) == "rejected" && !br[0].value("reason", std::string()).empty(),
            "multi-angle: a picture with a wrong angle label is rejected with the reason");
    }
  }
  // 10. Calibration guard: picture/model pairs that do not correspond (an ambiguous viewpoint) must not tint
  //     the texture; only lightness is mapped (distributions need no pairing).
  {
    std::vector<std::array<float, 3>> nat, pic;
    std::vector<double> w;
    uint32_t s = 12345;
    auto rnd = [&] { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0; };
    for (int i = 0; i < 2000; ++i) nat.push_back({float(0.15 + 0.25 * rnd()), float(0.12 * (rnd() - 0.5)), float(0.12 * (rnd() - 0.5))});
    for (int i = 0; i < 2000; ++i) {  // the picture's colours at unrelated surface points
      const auto& c = nat[size_t((i * 7919 + 13) % 2000)];
      pic.push_back(c);
      w.push_back(1);
    }
    nlohmann::json ev;
    const auto m = fit_colour_map(nat, pic, w, ev);
    std::printf("calibration guard: %s (held-out uncalibrated %.3f)\n", ev["method"].get<std::string>().c_str(), ev["uncalibrated_heldout_delta_e"].get<double>());
    check(ev["method"] == "l_quantile_lightness_only" && m.gain[1] == 1 && m.gain[2] == 1 && m.offset[1] == 0 && m.offset[2] == 0,
          "calibration: pairs that do not correspond change lightness only (no tint)");
    // Pairs that do correspond (a darker, cooler TRELLIS) are still fully calibrated.
    std::vector<std::array<float, 3>> dark;
    for (const auto& c : nat) dark.push_back({c[0] * 0.7f, c[1] * 0.8f - 0.01f, c[2] * 0.8f + 0.01f});
    nlohmann::json ev2;
    fit_colour_map(dark, nat, w, ev2);
    check(ev2["method"] != "l_quantile_lightness_only", "calibration: corresponding pairs are calibrated in colour too");
  }
  // 11. Per-material colour match (agent M2, B4): TRELLIS paints the back half of the red material duller and
  //     browner than its seen front (the anime-style character's orange). The material lock keeps it red; matching moves that hidden
  //     part's mean colour onto the front's mean of the same material, and leaves colours the picture never shows.
  {
    const Scene small = make_scene(0.4);
    const auto pic = photograph(small, 60, 25, true, 550, {0, 0.04, 0});
    const Source s11 = Source::from(pic, false);
    const auto r11 = register_source(small.logical, s11, deadline);
    constexpr int res = 256;
    auto voxel_of = [&](vec3d g) {
      const double t[3] = {g.x, -g.z, g.y};
      return std::array<int, 3>{int(std::floor((t[0] + 0.5) * res)), int(std::floor((t[1] + 0.5) * res)), int(std::floor((t[2] + 0.5) * res))};
    };
    // Back of the main box (-Z, never seen): the half next to the red +X side is the red material, painted duller.
    auto in_dull = [&](vec3d c) { return std::abs(c.z + 0.08) < 1e-6 && c.x > 0.04 && c.y < 0.2; };
    const pixel3d::Rgba8 dull{170, 62, 50, 255};
    std::map<std::array<int, 3>, std::array<float, 3>> vox;
    for (size_t f = 0; f < small.logical.tris.size(); ++f) {
      const auto& t = small.logical.tris[f];
      const vec3d centre = (small.logical.positions[t[0]] + small.logical.positions[t[1]] + small.logical.positions[t[2]]) * (1.0 / 3);
      const auto c = in_dull(centre) ? dull : kColours[small.face_dir[f]];
      const std::array<float, 3> dark{srgb8_from_linear(linear_from_srgb8(c.r) * 0.35f) / 255.f, srgb8_from_linear(linear_from_srgb8(c.g) * 0.35f) / 255.f,
                                      srgb8_from_linear(linear_from_srgb8(c.b) * 0.35f) / 255.f};
      for (int i = 0; i <= 6; ++i)
        for (int j = 0; i + j <= 6; ++j) {
          const double u = i / 6.0, v = j / 6.0;
          vox[voxel_of(small.logical.positions[t[0]] * (1 - u - v) + small.logical.positions[t[1]] * u + small.logical.positions[t[2]] * v)] = dark;
        }
    }
    std::vector<std::array<int, 3>> coords;
    std::vector<std::array<float, 3>> colours;
    for (const auto& [k, c] : vox) { coords.push_back(k); colours.push_back(c); }
    const NativeVolume volume = NativeVolume::from_voxels(res, coords, colours);
    const auto a11 = build_atlas(small.logical, 1024, 0, deadline);
    BakeOptions off;
    off.native = &volume;
    off.material_match = false;
    BakeOptions on = off;
    on.material_match = true;
    const auto plain = bake(small.logical, a11, s11, r11, off, deadline);
    const auto matched = bake(small.logical, a11, s11, r11, on, deadline);
    // Seen red (+X) as baked vs the dull back region; and the yellow rest of the back (a colour never seen).
    auto mean_lab = [&](const pixel3d::TexturedMesh& m, auto&& pick) {
      std::array<double, 3> acc{0, 0, 0};
      double n = 0;
      for (size_t f = 0; f < small.logical.tris.size(); ++f) {
        const auto& t = small.logical.tris[f];
        const vec3d centre = (small.logical.positions[t[0]] + small.logical.positions[t[1]] + small.logical.positions[t[2]]) * (1.0 / 3);
        if (!pick(f, centre)) continue;
        const auto l = oklab(texel(m, f));
        for (int d = 0; d < 3; ++d) acc[size_t(d)] += l[size_t(d)];
        n += 1;
      }
      for (auto& v : acc) v /= std::max(n, 1.0);
      return acc;
    };
    auto de = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
      return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    auto front_red = [&](size_t f, vec3d) { return small.face_dir[f] == 0; };
    auto back_dull = [&](size_t, vec3d c) { return in_dull(c); };
    auto back_rest = [&](size_t f, vec3d c) { return small.face_dir[f] == 5 && std::abs(c.z + 0.08) < 1e-6 && c.x < -0.04; };
    const double before = de(mean_lab(plain.mesh, front_red), mean_lab(plain.mesh, back_dull));
    const double after = de(mean_lab(matched.mesh, front_red), mean_lab(matched.mesh, back_dull));
    const double rest = de(mean_lab(plain.mesh, back_rest), mean_lab(matched.mesh, back_rest));
    std::printf("material match: red front vs dull back dE %.4f -> %.4f; unseen yellow moved %.4f; %s\n", before, after, rest,
                matched.evidence["native"].value("material_match", nlohmann::json()).dump().c_str());
    check(before > 0.04, "material match: the test's back is visibly duller than its front without matching");
    check(after < 0.5 * before && after < 0.03, "material match: the hidden part of a material takes its front's colour");
    check(rest < 0.01, "material match: colours the picture never shows are left as TRELLIS painted them");
    const auto mm = matched.evidence["native"].value("material_match", nlohmann::json());
    check(mm.is_object() && mm.contains("materials") && mm["materials"].is_array() && !mm["materials"].empty() &&
              mm["materials"][0].contains("delta_e_before") && mm["materials"][0].contains("delta_e_after"),
          "material match: per-material front/back colour difference is reported before and after");
  }
  std::filesystem::remove_all(dir);
  std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
  return failures ? 1 : 0;
}
