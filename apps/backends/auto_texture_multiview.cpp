// Generated views of the hidden sides (MV-Adapter image + geometry -> multi-view, Apache-2.0) as extra
// observations for the bake. This file only does geometry: it renders the control images the adapter
// expects (its orthographic six-view rig, position and normal maps in its frame) from the picture's camera,
// and projects generated views back onto the surface with a z-buffer, like the picture itself.
// The diffusion model runs in a separate process (tools/multiview/mv_generate.py).
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include "auto_texture_internal.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace p3dasset::autotex {
using namespace pixel3d;
using namespace detail;
namespace {
// The adapter's frame: Z up, mesh +X front axis convention of its loader (shape_init_mesh_up +y, front +x),
// i.e. std = (x, -z, y) of a Y-up GLB. The mesh is first turned so the picture's camera sits on +Z.
struct Frame3 {
  double yaw_rad = 0;
  vec3d to_std(vec3d p) const {
    const double c = std::cos(-yaw_rad), s = std::sin(-yaw_rad);
    const vec3d r{p.x * c + p.z * s, p.y, -p.x * s + p.z * c};
    return {r.x, -r.z, r.y};
  }
  vec3d from_std(vec3d a) const {  // inverse of to_std for directions
    const vec3d r{a.x, a.z, -a.y};
    const double c = std::cos(yaw_rad), s = std::sin(yaw_rad);
    return {r.x * c + r.z * s, r.y, -r.x * s + r.z * c};
  }
};
// get_c2w() of mvadapter/utils/mesh_utils/camera.py, in the adapter's frame.
void adapter_axes(double azimuth_deg, double elevation_deg, vec3d& right, vec3d& up, vec3d& lookat) {
  const double e = elevation_deg * 3.14159265358979323846 / 180, a = azimuth_deg * 3.14159265358979323846 / 180;
  const vec3d pos{std::cos(e) * std::cos(a), std::cos(e) * std::sin(a), std::sin(e)};
  lookat = normalize(vec3d{0, 0, 0} - pos);
  right = normalize(cross(lookat, vec3d{0, 0, 1}));
  up = normalize(cross(right, lookat));
}
struct Raster {
  std::vector<float> depth;
  std::vector<int32_t> face;
  std::vector<std::array<float, 3>> bary;
};
Raster rasterize(const Mesh& m, const MultiViewSet::Camera& cam) {
  const int n = cam.n;
  Raster r;
  r.depth.assign(size_t(n) * n, std::numeric_limits<float>::infinity());
  r.face.assign(size_t(n) * n, -1);
  r.bary.assign(size_t(n) * n, {0, 0, 0});
  std::vector<vec2d> q(m.positions.size());
  std::vector<double> d(m.positions.size());
  for (size_t i = 0; i < q.size(); ++i) {
    q[i] = cam.to_image(m.positions[i]);
    d[i] = cam.depth(m.positions[i]);
  }
  for (size_t f = 0; f < m.tris.size(); ++f) {
    const auto& t = m.tris[f];
    const vec2d a = q[t[0]], b = q[t[1]], c = q[t[2]];
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area) < 1e-14) continue;
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
        if (z < r.depth[i]) {
          r.depth[i] = z;
          r.face[i] = int32_t(f);
          r.bary[i] = {float(w0), float(w1), float(w2)};
        }
      }
  }
  return r;
}
}  // namespace

std::vector<MultiViewSet::Camera> MultiViewSet::rig(const Mesh& m, const Registration& reg, int n) {
  // scripts/inference_ig2mv_sd.py: six orthographic views, azimuth (x - 90) for x in 0, 90, 180, 270, 180, 180,
  // elevation 0 except top (89.99) and bottom (-89.99), frustum +-0.55 after scaling max |coordinate| to 0.5.
  Frame3 fr{reg.pose.yaw * 3.14159265358979323846 / 180};
  double max_abs = 1e-12;
  for (const auto& p : m.positions) {
    const vec3d s = fr.to_std(p);
    max_abs = std::max({max_abs, std::abs(s.x), std::abs(s.y), std::abs(s.z)});
  }
  const double az[6] = {-90, 0, 90, 180, 90, 90}, el[6] = {0, 0, 0, 0, 89.99, -89.99};
  std::vector<Camera> out;
  for (int k = 0; k < 6; ++k) {
    vec3d right, up, lookat;
    adapter_axes(az[k], el[k], right, up, lookat);
    Camera c;
    c.n = n;
    c.scale = 0.5 / max_abs;
    c.right = fr.from_std(right);
    c.up = fr.from_std(up);
    c.forward = fr.from_std(lookat);
    c.azimuth = az[k];
    c.elevation = el[k];
    c.yaw = reg.pose.yaw;
    out.push_back(c);
  }
  return out;
}

nlohmann::json MultiViewSet::export_controls(const Mesh& m, const Registration& reg, const std::string& utf8_dir, int n) {
  namespace fs = std::filesystem;
  const fs::path dir = path_from_utf8(utf8_dir);
  fs::create_directories(dir);
  const auto cams = rig(m, reg, n);
  const auto normals = vertex_normals(m);
  Frame3 fr{reg.pose.yaw * 3.14159265358979323846 / 180};
  nlohmann::json views = nlohmann::json::array();
  for (size_t k = 0; k < cams.size(); ++k) {
    const auto& cam = cams[k];
    const Raster r = rasterize(m, cam);
    ImageRGBA pos(n, n, {128, 128, 128, 255}), nrm(n, n, {128, 128, 128, 255});
    size_t covered = 0;
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const size_t i = size_t(y) * n + x;
        if (r.face[i] < 0) continue;
        ++covered;
        const auto& t = m.tris[size_t(r.face[i])];
        const auto& b = r.bary[i];
        const vec3d p = m.positions[t[0]] * b[0] + m.positions[t[1]] * b[1] + m.positions[t[2]] * b[2];
        const vec3d nn = normalize(normals[t[0]] * b[0] + normals[t[1]] * b[1] + normals[t[2]] * b[2]);
        const vec3d ps = fr.to_std(p) * cam.scale, ns = fr.to_std(nn);
        auto enc = [](double v) { return uint8_t(std::clamp(std::lround(v * 255.0), 0L, 255L)); };
        pos.at(x, y) = {enc(ps.x + 0.5), enc(ps.y + 0.5), enc(ps.z + 0.5), 255};
        nrm.at(x, y) = {enc(ns.x / 2 + 0.5), enc(ns.y / 2 + 0.5), enc(ns.z / 2 + 0.5), 255};
      }
    const auto pos_bytes = encode_png(pos), nrm_bytes = encode_png(nrm);
    for (auto [name, bytes] : {std::pair{"pos_" + std::to_string(k) + ".png", &pos_bytes}, std::pair{"nrm_" + std::to_string(k) + ".png", &nrm_bytes}}) {
      std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes->data()), std::streamsize(bytes->size()));
      if (!out) throw std::runtime_error("Could not write the multi-view control images");
    }
    views.push_back({{"azimuth", cam.azimuth}, {"elevation", cam.elevation}, {"covered_fraction", double(covered) / double(size_t(n) * n)}});
  }
  nlohmann::json j{{"schema", 1}, {"rig", "MV-Adapter ig2mv six-view orthographic (+-0.55, max |coordinate| 0.5)"}, {"size", n},
                   {"yaw", reg.pose.yaw}, {"scale", cams.front().scale}, {"views", views}};
  std::ofstream(dir / "mv.json", std::ios::binary | std::ios::trunc) << j.dump(2);
  return j;
}

MultiViewSet MultiViewSet::load(const Mesh& m, const Registration& reg, const std::string& utf8_dir) {
  namespace fs = std::filesystem;
  const fs::path dir = path_from_utf8(utf8_dir);
  MultiViewSet set;
  set.diag = diagonal(m);
  for (int k = 0; k < 6; ++k) {
    const fs::path file = dir / ("view_" + std::to_string(k) + ".png");
    if (!fs::exists(file)) continue;
    auto image = load_png(file);
    if (!image) throw std::runtime_error("Could not read the generated view " + to_utf8(file));
    View v;
    v.cam = rig(m, reg, image->width()).at(size_t(k));
    if (image->height() != image->width()) throw std::runtime_error("Generated views must be square");
    const int n = v.cam.n;
    const Raster r = rasterize(m, v.cam);
    v.depth = r.depth;
    v.rgb.resize(size_t(n) * n);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const auto p = image->at(x, y);
        v.rgb[size_t(y) * n + x] = {linear_from_srgb8(p.r), linear_from_srgb8(p.g), linear_from_srgb8(p.b)};
      }
    // Distance (px) to the object's outline or a depth jump in this view: generated pixels there are unreliable.
    const float far = 64;
    v.edge.assign(v.depth.size(), far);
    const double pixel_world = 1.1 / (n * v.cam.scale);
    const double jump = 6 * pixel_world + 0.002 * set.diag;
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const size_t i = size_t(y) * n + x;
        bool bad = !std::isfinite(v.depth[i]);
        if (!bad)
          for (auto [dx, dy] : std::array<std::pair<int, int>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= n || ny >= n) { bad = true; break; }
            const float d = v.depth[size_t(ny) * n + nx];
            if (!std::isfinite(d) || std::abs(d - v.depth[i]) > jump * v.cam.scale) { bad = true; break; }
          }
        if (bad) v.edge[i] = 0;
      }
    // Two-pass chamfer.
    auto at = [&](int x, int y) -> float& { return v.edge[size_t(y) * n + x]; };
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        float& e = at(x, y);
        if (x) e = std::min(e, at(x - 1, y) + 1.f);
        if (y) e = std::min(e, at(x, y - 1) + 1.f);
      }
    for (int y = n - 1; y >= 0; --y)
      for (int x = n - 1; x >= 0; --x) {
        float& e = at(x, y);
        if (x + 1 < n) e = std::min(e, at(x + 1, y) + 1.f);
        if (y + 1 < n) e = std::min(e, at(x, y + 1) + 1.f);
      }
    v.pixel_world = pixel_world;
    v.index = k;
    set.views.push_back(std::move(v));
  }
  if (set.views.empty()) throw std::runtime_error("No generated views found in " + utf8_dir);
  set.evidence = {{"directory", utf8_dir}, {"views", set.views.size()}};
  return set;
}

bool MultiViewSet::sample(vec3d p, vec3d smooth_n, vec3d face_n, std::array<float, 3>& rgb, float& weight, float& best_conf) const {
  double acc[3]{}, wsum = 0, best = 0;
  for (const auto& v : views) {
    const vec3d to_cam = -v.cam.forward;
    const double facing = dot(face_n, to_cam);
    if (facing <= 0.05) continue;
    const vec2d q = v.cam.to_image(p);
    const int n = v.cam.n, ix = int(std::floor(q.x)), iy = int(std::floor(q.y));
    if (ix < 0 || iy < 0 || ix >= n || iy >= n) continue;
    const size_t i = size_t(iy) * n + ix;
    const double tan_t = std::sqrt(std::max(0.0, 1 - facing * facing)) / std::max(facing, 0.2);
    const double bias = (v.pixel_world * (1.0 + 1.5 * tan_t) + 0.0015 * diag) * v.cam.scale;
    if (!(v.cam.depth(p) <= double(v.depth[i]) + bias)) continue;
    const double conf = smoothstep(0.10, 0.55, std::min(dot(smooth_n, to_cam), facing + 0.2)) * smoothstep(0.5, 3.0, v.edge[i]);
    if (conf <= 0.01) continue;
    // Bilinear over the generated image.
    const double fx = q.x - 0.5, fy = q.y - 0.5;
    const int bx = int(std::floor(fx)), by = int(std::floor(fy));
    const double tx = fx - bx, ty = fy - by;
    double c[3]{};
    for (int dy = 0; dy < 2; ++dy)
      for (int dx = 0; dx < 2; ++dx) {
        const auto& s = v.rgb[size_t(std::clamp(by + dy, 0, n - 1)) * n + std::clamp(bx + dx, 0, n - 1)];
        const double w = (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty);
        for (int d = 0; d < 3; ++d) c[d] += s[size_t(d)] * w;
      }
    // Sharpest view dominates (weight ~ conf^4), so overlapping views do not ghost.
    const double w = conf * conf * conf * conf;
    for (int d = 0; d < 3; ++d) acc[d] += c[d] * w;
    wsum += w;
    best = std::max(best, conf);
  }
  if (wsum <= 1e-9) return false;
  for (int d = 0; d < 3; ++d) rgb[size_t(d)] = float(acc[d] / wsum);
  weight = float(wsum);
  best_conf = float(best);
  return true;
}
}  // namespace p3dasset::autotex
