// pixel3d-regional-fix (milestone 4, W4: "there is a problem here").
//
//   views:  pixel3d-regional-fix views textured.glb OUT_DIR
//           Renders the model from 8 yaws (base colour, 512 px) with the same framing the fix uses, so a mark made on
//           one of these pictures maps exactly to faces.
//   fix:    pixel3d-regional-fix fix textured.glb OUT_DIR --yaw Y --mark X Y R [--source front.png]
//           [--candidate other.glb]... [--note TEXT]
//           The mark (circle in pixels of the yaw's view) → the faces under it → their texels (UV mask, +2 texels).
//           Reports what painted the region (the worker's provenance map next to the GLB, when there is one) and
//           tries at most 3 candidates (other textures of the same mesh and UVs: texture candidates, the alternative)
//           by copying ONLY the region's texels. Each attempt is re-checked by the texture gate and compared in the
//           region with the source picture when the picture sees it; texels outside the mask are compared and must
//           be identical (measured, not assumed). Writes fix.json, before/after close-ups and attempt GLBs.
//           Nothing is chosen for the person: the result lists the attempts with their numbers and asks.
// Exit: 0 report written, 2 failure, 64 usage.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>

#include "auto_texture.hpp"
#include "auto_texture_internal.hpp"
#include "mesh_asset.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/image.hpp"

namespace fs = std::filesystem;
using namespace p3dasset;
using namespace p3dasset::autotex;
using namespace p3dasset::autotex::detail;
using nlohmann::json;
using pixel3d::ImageRGBA;
using pixel3d::Rgba8;
using pixel3d::vec2d;
using pixel3d::vec3d;

namespace {

constexpr int kView = 512;

struct Framing {
  vec3d centre;
  double radius = 1;
};
Framing framing(const pixel3d::Mesh& geo) {
  vec3d lo = geo.positions.front(), hi = lo;
  for (auto p : geo.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  Framing f{(lo + hi) * 0.5, 1e-9};
  for (auto p : geo.positions) f.radius = std::max(f.radius, pixel3d::length(p - f.centre));
  return f;
}

ImageRGBA to_image(const Frame& fr) {
  ImageRGBA img(fr.n, fr.n, {30, 30, 34, 255});
  for (size_t i = 0; i < fr.face.size(); ++i) {
    if (fr.face[i] < 0) continue;
    const auto c = linear_from_oklab(fr.lab[i]);
    img.at(int(i % size_t(fr.n)), int(i / size_t(fr.n))) = {srgb8_from_linear(c[0]), srgb8_from_linear(c[1]), srgb8_from_linear(c[2]), 255};
  }
  return img;
}

void save(const ImageRGBA& img, const fs::path& f) {
  const auto m = pixel3d::save_png(img, f);
  if (m.code != "ok") throw std::runtime_error("cannot write " + pixel3d::to_utf8(f));
}

ImageRGBA crop(const ImageRGBA& img, int cx, int cy, int half, int scale) {
  ImageRGBA out(2 * half * scale, 2 * half * scale, {30, 30, 34, 255});
  for (int y = 0; y < out.height(); ++y)
    for (int x = 0; x < out.width(); ++x) {
      const int sx = cx - half + x / scale, sy = cy - half + y / scale;
      if (img.in_bounds(sx, sy)) out.at(x, y) = img.at(sx, sy);
    }
  return out;
}

// Texels of the faces (their UV triangles, plus `grow` texels around: gutters and bilinear reach).
std::vector<uint8_t> uv_mask(const pixel3d::TexturedMesh& m, const std::vector<uint8_t>& faces, int w, int h, int grow) {
  std::vector<uint8_t> mask(size_t(w) * h, 0);
  for (size_t f = 0; f < faces.size(); ++f) {
    if (!faces[f]) continue;
    // raster_uv works in texel units (as the bake calls it).
    const vec2d sz{double(w), double(h)};
    auto px = [&](vec2d uv) { return vec2d{uv.x * sz.x, uv.y * sz.y}; };
    const vec2d a = px(m.uvs[m.indices[3 * f]]), b = px(m.uvs[m.indices[3 * f + 1]]), c = px(m.uvs[m.indices[3 * f + 2]]);
    auto mark = [&](int x, int y, double, double, double) { mask[size_t(y) * w + x] = 1; };
    raster_uv(a, b, c, w, h, 0, h, mark);
    raster_uv_border(a, b, c, w, h, 0, h, 1.0, mark);
  }
  // Texels inside other faces' UV triangles stay theirs: growing into them would change the surface around the region.
  std::vector<uint8_t> other(size_t(w) * h, 0);
  for (size_t f = 0; f < faces.size(); ++f) {
    if (faces[f]) continue;
    const vec2d sz{double(w), double(h)};
    auto px = [&](vec2d uv) { return vec2d{uv.x * sz.x, uv.y * sz.y}; };
    // Everything another face samples: its triangle, the 1-texel border, and the 2x2 bilinear footprint of those.
    auto take = [&](int x, int y, double, double, double) {
      for (int dy = 0; dy <= 1; ++dy)
        for (int dx = 0; dx <= 1; ++dx) other[size_t(std::min(h - 1, y + dy)) * w + size_t(std::min(w - 1, x + dx))] = 1;
    };
    const vec2d a = px(m.uvs[m.indices[3 * f]]), b = px(m.uvs[m.indices[3 * f + 1]]), c = px(m.uvs[m.indices[3 * f + 2]]);
    raster_uv(a, b, c, w, h, 0, h, take);
    raster_uv_border(a, b, c, w, h, 0, h, 1.0, take);
  }
  for (int g = 0; g < grow; ++g) {
    auto next = mask;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (!mask[size_t(y) * w + x])
          for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
            if (x + dx >= 0 && y + dy >= 0 && x + dx < w && y + dy < h && mask[size_t(y + dy) * w + x + dx]) { next[size_t(y) * w + x] = 1; break; }
    mask.swap(next);
  }
  for (size_t i = 0; i < mask.size(); ++i)
    if (other[i]) mask[i] = 0;
  return mask;
}

int run_views(const fs::path& glb, const fs::path& out) {
  const auto m = load_generated_appearance(glb);
  if (!m) throw std::runtime_error("the GLB has no base-colour texture");
  const auto geo = render_geometry(*m);
  const auto fr = framing(geo);
  fs::create_directories(out);
  json views = json::array();
  for (double yaw : {0.0, 45.0, 90.0, 135.0, 180.0, 225.0, 270.0, 315.0}) {
    const Frame f = render_ids(geo, *m, yaw, 10, fr.centre, fr.radius, kView);
    const fs::path png = out / ("view-yaw" + std::to_string(int(yaw)) + ".png");
    save(to_image(f), png);
    views.push_back({{"yaw", yaw}, {"elevation", 10}, {"png", pixel3d::to_utf8(png)}, {"size", kView}});
  }
  const json j = {{"schema", "3re.regional-views/1"}, {"glb", pixel3d::to_utf8(glb)}, {"views", views}};
  pixel3d::write_file_atomic(out / "views.json", j.dump(1));
  std::cout << "REGIONAL_VIEWS " << views.size() << std::endl;
  return 0;
}

int run_fix(const fs::path& glb, const fs::path& out, double yaw, double mx, double my, double mr, const fs::path& source,
            const std::vector<fs::path>& candidates, const std::string& note) {
  if (fs::exists(out / "fix.json")) throw std::runtime_error("the output folder already has a fix.json (nothing is overwritten)");
  fs::create_directories(out);
  const auto logical = load_geometry_glb(glb);
  const auto m = load_generated_appearance(glb);
  if (!m || !m->texture) throw std::runtime_error("the GLB has no base-colour texture");
  const auto geo = render_geometry(*m);
  const auto fr = framing(geo);
  const Frame before = render_ids(geo, *m, yaw, 10, fr.centre, fr.radius, kView);

  // 1. The mark → faces (every face with a pixel inside the circle) → texels.
  std::vector<uint8_t> faces(geo.tris.size(), 0);
  size_t marked_px = 0;
  for (int y = 0; y < kView; ++y)
    for (int x = 0; x < kView; ++x) {
      if (std::hypot(x + 0.5 - mx, y + 0.5 - my) > mr) continue;
      const int f = before.face[size_t(y) * kView + x];
      if (f >= 0) faces[size_t(f)] = 1, ++marked_px;
    }
  const size_t n_faces = size_t(std::count(faces.begin(), faces.end(), 1));
  if (n_faces == 0) throw std::runtime_error("the mark does not touch the model in this view");
  // Faces sharing a vertex position with the region (bilinear neighbours).
  std::vector<uint8_t> adjacent(geo.tris.size(), 0);
  {
    std::vector<std::array<double, 3>> keys;
    for (size_t f = 0; f < faces.size(); ++f)
      if (faces[f])
        for (auto v : geo.tris[f]) keys.push_back({geo.positions[v].x, geo.positions[v].y, geo.positions[v].z});
    std::sort(keys.begin(), keys.end());
    for (size_t f = 0; f < faces.size(); ++f)
      if (!faces[f])
        for (auto v : geo.tris[f])
          if (std::binary_search(keys.begin(), keys.end(), std::array<double, 3>{geo.positions[v].x, geo.positions[v].y, geo.positions[v].z})) adjacent[f] = 1;
  }
  const int W = m->texture->width(), H = m->texture->height();
  const auto mask = uv_mask(*m, faces, W, H, 2);
  const size_t mask_texels = size_t(std::count(mask.begin(), mask.end(), 1));

  json report = {{"schema", "3re.regional-fix/1"}, {"glb", pixel3d::to_utf8(glb)}, {"note", note},
                 {"mark", {{"yaw", yaw}, {"elevation", 10}, {"x", mx}, {"y", my}, {"radius", mr}, {"view_size", kView}}},
                 {"region", {{"faces", n_faces}, {"view_pixels", marked_px}, {"texels", mask_texels}, {"texture_size", {W, H}}}}};

  // 2. What painted the region: the worker's provenance map (same atlas) when it is there.
  const fs::path prov_png = fs::path(glb).concat(L".provenance.png");
  json prov = {{"available", false}};
  if (fs::exists(prov_png)) {
    if (auto p = pixel3d::load_png(prov_png); p && p->width() == W && p->height() == H) {
      static const std::pair<Rgba8, const char*> kinds[] = {
          {{40, 200, 90, 255}, "seen in a picture"}, {{230, 210, 60, 255}, "blended (picture edge)"}, {{240, 140, 40, 255}, "reconstructed"},
          {{220, 50, 50, 255}, "fallback colour"}, {{80, 140, 235, 255}, "TRELLIS native texture (not seen by any picture)"},
          {{190, 90, 230, 255}, "extra picture (multi-angle)"}};
      json counts = json::object();
      size_t total = 0;
      for (size_t i = 0; i < mask.size(); ++i) {
        if (!mask[i]) continue;
        const Rgba8 c = p->at(int(i % size_t(W)), int(i / size_t(W)));
        for (const auto& [col, name] : kinds)
          if (c == col) { counts[name] = counts.value(name, 0) + 1; ++total; }
      }
      json share = json::object();
      std::string top;
      double best = 0;
      for (auto& [k, v] : counts.items()) {
        const double s = double(v.get<int>()) / double(std::max<size_t>(1, total));
        share[k] = s;
        if (s > best) best = s, top = k;
      }
      prov = {{"available", true}, {"map", pixel3d::to_utf8(prov_png)}, {"share", share}, {"mostly", top}};
    }
  }
  report["painted_by"] = prov;

  // 3. Does the source picture see the region? (registration of the front picture on this mesh)
  std::optional<Source> src;
  std::optional<Registration> reg;
  if (!source.empty()) {
    auto img = pixel3d::load_png(source);
    // A picture that was given but cannot be read must not turn into "no picture sees this region".
    if (!img) throw std::runtime_error("cannot read the source picture " + pixel3d::to_utf8(source) + ": " + img.error().text);
    {
      src = Source::from(*img, false);
      reg = register_source(logical, *src, Deadline::in_seconds(120));
    }
  }
  auto picture_distance = [&](const pixel3d::TexturedMesh& tm) -> std::optional<double> {
    // Mean Oklab distance between the region's texels and the picture at the same surface points, where the
    // picture sees them with confidence >= 0.5 (registered camera, depth-tested). None = the picture does not see it.
    if (!src || !reg) return std::nullopt;
    const View v = View::build(logical, *src, reg->pose);
    double sum = 0;
    size_t n = 0;
    for (size_t f = 0; f < faces.size(); ++f) {
      if (!faces[f]) continue;
      const auto& t = geo.tris[f];
      const vec3d c = (geo.positions[t[0]] + geo.positions[t[1]] + geo.positions[t[2]]) * (1.0 / 3);
      vec3d fn = pixel3d::cross(geo.positions[t[1]] - geo.positions[t[0]], geo.positions[t[2]] - geo.positions[t[0]]);
      fn = pixel3d::length(fn) > 0 ? pixel3d::normalize(fn) : vec3d{0, 0, 1};
      const auto o = v.observe(*src, reg->pose, c, fn, fn, 0.12, 0.5);
      if (o.conf < 0.5f) continue;
      const vec2d uv = (tm.uvs[t[0]] + tm.uvs[t[1]] + tm.uvs[t[2]]) * (1.0 / 3);
      const auto a = oklab(sample_texture(*tm.texture, uv));
      const auto b = oklab({o.rgb[0], o.rgb[1], o.rgb[2], 1});
      sum += std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
      ++n;
    }
    if (n < 3) return std::nullopt;
    return sum / double(n);
  };
  const auto d0 = picture_distance(*m);
  report["source_sees_region"] = d0.has_value();
  if (d0) report["before_picture_distance"] = *d0;

  // Close-ups (the view, 2x around the mark).
  const int half = std::clamp(int(mr * 3), 32, 96), cx = int(mx), cy = int(my);
  const ImageRGBA view_before = to_image(before);
  save(crop(view_before, cx, cy, half, 4), out / "before-closeup.png");
  save(view_before, out / "before-view.png");
  const auto gate0 = src && reg ? evaluate(logical, *m, *src, *reg) : json();
  report["before_gate"] = gate0.is_object() ? json(gate0.value("verdict", std::string())) : json(nullptr);

  // 4. Attempts: the region's texels from each candidate texture (same geometry and UVs), at most 3.
  json attempts = json::array();
  int tried = 0;
  // Attempt sources, in order: the source picture re-projected onto the region with a lower confidence than the bake
  // used (the bake left such texels to the TRELLIS texture), then the other textures of the same mesh.
  std::vector<std::string> sources;
  if (src && reg) sources.push_back("reproject:picture");
  for (const auto& cf : candidates) sources.push_back(pixel3d::to_utf8(cf));
  for (const auto& source_name : sources) {
    if (tried >= 3) break;
    json a = {{"candidate", source_name}};
    try {
      std::shared_ptr<const ImageRGBA> region_from;
      if (source_name == "reproject:picture") {
        // Every region texel whose surface point the picture sees (depth-tested, facing the camera, inside its
        // outline) takes the picture's colour, whatever its edge confidence; the others keep their colour.
        const View v = View::build(logical, *src, reg->pose);
        auto img = std::make_shared<ImageRGBA>(*m->texture);
        size_t painted = 0;
        for (size_t f = 0; f < faces.size(); ++f) {
          if (!faces[f]) continue;
          const auto& t = geo.tris[f];
          vec3d fn = pixel3d::cross(geo.positions[t[1]] - geo.positions[t[0]], geo.positions[t[2]] - geo.positions[t[0]]);
          fn = pixel3d::length(fn) > 0 ? pixel3d::normalize(fn) : vec3d{0, 0, 1};
          const vec2d sz{double(W), double(H)};
          auto px = [&](vec2d uv) { return vec2d{uv.x * sz.x, uv.y * sz.y}; };
          raster_uv(px(m->uvs[m->indices[3 * f]]), px(m->uvs[m->indices[3 * f + 1]]), px(m->uvs[m->indices[3 * f + 2]]), W, H, 0, H,
                    [&](int x, int y, double b0, double b1, double b2) {
                      if (!mask[size_t(y) * W + x]) return;
                      const vec3d pos = geo.positions[t[0]] * b0 + geo.positions[t[1]] * b1 + geo.positions[t[2]] * b2;
                      const auto o = v.observe(*src, reg->pose, pos, fn, fn, 0.12, 0.5);
                      if (o.stage != 3 || o.conf <= 0.f || o.rgb[3] < 0.5f) return;
                      img->at(x, y) = {srgb8_from_linear(o.rgb[0]), srgb8_from_linear(o.rgb[1]), srgb8_from_linear(o.rgb[2]), 255};
                      ++painted;
                    });
        }
        if (painted == 0) throw std::runtime_error("the picture sees none of the region's texels");
        a["reprojected_texels"] = painted;
        region_from = img;
      } else {
        const auto cm = load_generated_appearance(pixel3d::path_from_utf8(source_name));
        if (!cm || !cm->texture) throw std::runtime_error("no texture");
        if (cm->texture->width() != W || cm->texture->height() != H || cm->uvs != m->uvs || cm->indices != m->indices)
          throw std::runtime_error("different UV layout: its texels do not map to the same surface");
        region_from = cm->texture;
      }
      ++tried;
      auto tex = std::make_shared<ImageRGBA>(*m->texture);
      for (size_t i = 0; i < mask.size(); ++i)
        if (mask[i]) tex->at(int(i % size_t(W)), int(i / size_t(W))) = region_from->at(int(i % size_t(W)), int(i / size_t(W)));
      pixel3d::TexturedMesh fixed = *m;
      fixed.texture = tex;
      // Outside the mask nothing may change: measured texel by texel.
      size_t changed_outside = 0, changed_inside = 0;
      for (size_t i = 0; i < mask.size(); ++i) {
        const bool diff = !(tex->at(int(i % size_t(W)), int(i / size_t(W))) == m->texture->at(int(i % size_t(W)), int(i / size_t(W))));
        (mask[i] ? changed_inside : changed_outside) += diff;
      }
      a["changed_texels_inside"] = changed_inside;
      a["changed_texels_outside_mask"] = changed_outside;  // 0 by construction (only mask texels are written); kept as a guard
      // Independent check, from the surface's side: every face outside the region, the texels it samples (its UV
      // triangle plus the 1-texel bilinear reach). A face reading a changed texel changed, whatever the mask was.
      size_t faces_changed = 0, faces_changed_edge = 0;
      for (size_t f = 0; f < faces.size(); ++f) {
        if (faces[f]) continue;
        const vec2d sz{double(W), double(H)};
        auto px = [&](vec2d uv) { return vec2d{uv.x * sz.x, uv.y * sz.y}; };
        bool changed = false;
        auto probe = [&](int x, int y, double, double, double) {
          if (changed) return;
          for (int dy = 0; dy <= 1 && !changed; ++dy)
            for (int dx = 0; dx <= 1 && !changed; ++dx) {
              const int xx = std::min(W - 1, x + dx), yy = std::min(H - 1, y + dy);
              changed = !(tex->at(xx, yy) == m->texture->at(xx, yy));
            }
        };
        const vec2d ua = px(m->uvs[m->indices[3 * f]]), ub = px(m->uvs[m->indices[3 * f + 1]]), uc = px(m->uvs[m->indices[3 * f + 2]]);
        raster_uv(ua, ub, uc, W, H, 0, H, probe);
        raster_uv_border(ua, ub, uc, W, H, 0, H, 1.0, probe);
        if (changed) (adjacent[f] ? faces_changed_edge : faces_changed)++;
      }
      a["faces_outside_changed"] = faces_changed;
      a["faces_on_region_edge_changed"] = faces_changed_edge;
      const Frame after = render_ids(geo, fixed, yaw, 10, fr.centre, fr.radius, kView);
      // Rendered pixels outside the marked faces — a second, image-space check. Faces sharing a vertex with the region
      // can read its edge texels through bilinear filtering ("edge"); every other face must be unchanged.
      size_t view_changed_outside = 0, view_changed_edge = 0;
      for (size_t i = 0; i < after.face.size(); ++i)
        if (after.face[i] >= 0 && !faces[size_t(after.face[i])] &&
            (after.lab[i][0] != before.lab[i][0] || after.lab[i][1] != before.lab[i][1] || after.lab[i][2] != before.lab[i][2]))
          (adjacent[size_t(after.face[i])] ? view_changed_edge : view_changed_outside)++;
      a["view_pixels_changed_outside_region"] = view_changed_outside;
      a["view_pixels_changed_on_region_edge"] = view_changed_edge;
      const std::string tag = "attempt-" + std::to_string(tried);
      save(crop(to_image(after), cx, cy, half, 4), out / (tag + "-closeup.png"));
      if (const auto d = picture_distance(fixed)) a["picture_distance"] = *d;
      if (src && reg) a["gate"] = evaluate(logical, fixed, *src, *reg).value("verdict", std::string());
      const auto bytes = write_glb(logical, fixed, {{"kind", "regional_fix"}, {"from", pixel3d::to_utf8(glb)}, {"region_from", source_name}});
      const fs::path g = out / (tag + ".glb");
      pixel3d::write_file_atomic(g, std::span<const uint8_t>(bytes));
      a["glb"] = pixel3d::to_utf8(g);
      a["closeup"] = pixel3d::to_utf8(out / (tag + "-closeup.png"));
      a["outside_unchanged"] = changed_outside == 0 && faces_changed == 0 && view_changed_outside == 0;
      a["status"] = "made";
    } catch (const std::exception& e) {
      a["status"] = "skipped";
      a["reason"] = e.what();
    }
    attempts.push_back(a);
  }
  report["attempts"] = attempts;
  // 5. What can be said, measured: an attempt is "closer to the picture" only when the picture sees the region.
  json best = nullptr;
  if (d0)
    for (const auto& a : attempts)
      if (a.value("status", "") == "made" && a.contains("picture_distance") && a["picture_distance"].get<double>() < *d0 - 0.005 &&
          a.value("outside_unchanged", false) && (!a.contains("gate") || a["gate"] != "FAIL") &&
          (best.is_null() || a["picture_distance"].get<double>() < best["picture_distance"].get<double>()))
        best = a;
  report["closest_to_picture"] = best;
  std::string why;
  if (tried == 0) why = "nothing to take the region from: no picture sees it and no other texture of this mesh has the same UV layout";
  else if (!d0) why = "no picture sees this region: whether an attempt is better cannot be measured; compare the close-ups";
  else if (best.is_null()) why = "no attempt is closer to the picture in this region than the current texture";
  report["limit"] = why;
  pixel3d::write_file_atomic(out / "fix.json", report.dump(1));
  std::cout << "REGIONAL_FIX faces " << n_faces << " texels " << mask_texels << " attempts " << tried << (best.is_null() ? " none closer" : " one closer")
            << std::endl;
  return 0;
}

}  // namespace

int main() {
  std::vector<std::wstring> args;
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  LocalFree(argv);
  auto usage = [] {
    std::cerr << "usage: pixel3d-regional-fix views textured.glb OUT_DIR\n"
                 "       pixel3d-regional-fix fix textured.glb OUT_DIR --yaw Y --mark X Y R [--source front.png] [--candidate other.glb]... [--note TEXT]"
              << std::endl;
    return 64;
  };
  if (args.size() < 3) return usage();
  try {
    if (args[0] == L"views") return run_views(args[1], args[2]);
    if (args[0] != L"fix") return usage();
    double yaw = 0, x = -1, y = -1, r = -1;
    fs::path source;
    std::vector<fs::path> cands;
    std::string note;
    for (size_t i = 3; i < args.size(); ++i) {
      auto next = [&]() -> std::wstring { if (i + 1 >= args.size()) throw std::runtime_error("missing value"); return args[++i]; };
      if (args[i] == L"--yaw") yaw = std::stod(next());
      else if (args[i] == L"--mark") { x = std::stod(next()); y = std::stod(next()); r = std::stod(next()); }
      else if (args[i] == L"--source") source = next();
      else if (args[i] == L"--candidate") cands.push_back(next());
      else if (args[i] == L"--note") note = pixel3d::to_utf8(fs::path(next()));
      else return usage();
    }
    if (r <= 0) return usage();
    return run_fix(args[1], args[2], yaw, x, y, r, source, cands, note);
  } catch (const std::exception& e) {
    std::cerr << "REGIONAL_FIX_ERROR " << e.what() << std::endl;
    return 2;
  }
}
