// pixel3d-texture-inspect: developer tool (not packaged) for judging textures up close.
//
//   pixel3d-texture-inspect render <model.glb> <out.png> <yaw> <elevation> <size> [--centre X Y Z --radius R]
//                                  [--untextured] [--unlit]
//   pixel3d-texture-inspect density <model.glb> <picture.png> --centre X Y Z --radius R [--crisp]
//
// render: lit close-up (camera framed on the sphere centre/radius, whole model by default); --untextured
// renders the bare geometry in grey (is the shape or the texture the limit?).
// density: registers the picture like the worker and prints, for the surface inside the cylinder (centre
// X Y, radius R, along GLB Z) that the
// picture sees: picture pixels per world unit, atlas texels per world unit, texels per picture pixel, and
// how many picture pixels cover that surface (JSON on stdout).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "auto_texture_internal.hpp"
#include "backends/mesh_asset.hpp"
#include "pixel3d/common/path_utf8.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using namespace pixel3d;
using namespace p3dasset;
using namespace p3dasset::autotex;

namespace {
TexturedMesh grey(const Mesh& m) {
  TexturedMesh t;
  t.positions = m.positions;
  t.normals = detail::vertex_normals(m);
  for (const auto& tri : m.tris) t.indices.insert(t.indices.end(), tri.begin(), tri.end());
  t.uvs.assign(t.positions.size(), {0.5, 0.5});
  auto img = std::make_shared<ImageRGBA>(1, 1, Rgba8{200, 200, 205, 255});
  t.texture = img;
  return t;
}
ImageRGBA closeup(const TexturedMesh& m, double yaw, double el, int size, vec3d centre, double radius, bool lit) {
  const Pose pose = Pose::make(yaw, el);
  const int ss = 2;
  Camera c;
  c.forward = pose.forward;
  c.right = pose.right;
  c.up = pose.up;
  c.eye = centre - c.forward * radius * 4;
  c.width = c.height = size * ss;
  c.scale = c.width * 0.48 / radius;
  c.offset = {c.width / 2.0, c.height / 2.0};
  RenderOptions o;
  o.lit = lit;
  o.smooth_texture = true;
  o.background = {46, 48, 54, 255};
  o.light_dir = normalize(-pose.forward + pose.up * 0.9 - pose.right * 0.35);
  const auto big = render(m, c, o);
  ImageRGBA out(size, size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      int r = 0, g = 0, b = 0;
      for (int dy = 0; dy < ss; ++dy)
        for (int dx = 0; dx < ss; ++dx) {
          const auto p = big.color.at(x * ss + dx, y * ss + dy);
          r += p.r; g += p.g; b += p.b;
        }
      out.at(x, y) = {uint8_t(r / (ss * ss)), uint8_t(g / (ss * ss)), uint8_t(b / (ss * ss)), 255};
    }
  return out;
}
}  // namespace

int run(const std::vector<std::string>& a) {
  if (a.size() < 3) throw std::runtime_error("usage: see the file header");
  vec3d centre{};
  double radius = 0;
  bool untextured = false, lit = true, crisp = false;
  std::vector<std::string> pos;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] == "--centre" && i + 3 < a.size()) { centre = {std::stod(a[i + 1]), std::stod(a[i + 2]), std::stod(a[i + 3])}; i += 3; }
    else if (a[i] == "--radius" && i + 1 < a.size()) radius = std::stod(a[++i]);
    else if (a[i] == "--untextured") untextured = true;
    else if (a[i] == "--unlit") lit = false;
    else if (a[i] == "--crisp") crisp = true;
    else pos.push_back(a[i]);
  }
  const Mesh geo = load_geometry_glb(path_from_utf8(pos.at(1)));
  if (radius <= 0) {
    vec3d lo = geo.positions.front(), hi = lo;
    for (auto p : geo.positions) {
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    centre = (lo + hi) * 0.5;
    radius = length(hi - lo) / 2;
  }
  if (pos[0] == "render") {
    std::shared_ptr<const TexturedMesh> m;
    if (!untextured) m = load_generated_appearance(path_from_utf8(pos.at(1)));
    if (!m) m = std::make_shared<TexturedMesh>(grey(geo));
    const auto img = closeup(*m, std::stod(pos.at(3)), std::stod(pos.at(4)), std::stoi(pos.at(5)), centre, radius, lit);
    const auto bytes = encode_png(img);
    std::ofstream(path_from_utf8(pos.at(2)), std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return 0;
  }
  if (pos[0] == "density") {
    auto image = load_png(path_from_utf8(pos.at(2)));
    if (!image) throw std::runtime_error("cannot read picture");
    const Source src = Source::from(*image, crisp);
    const Registration reg = register_source(geo, src, Deadline::in_seconds(120));
    const auto view = detail::View::build(geo, src, reg.pose);
    const auto normals = detail::vertex_normals(geo);
    auto m = load_generated_appearance(path_from_utf8(pos.at(1)));
    double area_in = 0, area_seen = 0, uv_seen = 0, picture_px = 0, facing_sum = 0;
    double edge_sum = 0;
    size_t faces_in = 0;
    for (size_t f = 0; f < geo.tris.size(); ++f) {
      const auto& t = geo.tris[f];
      const vec3d c = (geo.positions[t[0]] + geo.positions[t[1]] + geo.positions[t[2]]) * (1.0 / 3);
      if (std::hypot(c.x - centre.x, c.y - centre.y) > radius) continue;  // cylinder along GLB Z (front picture view)
      const vec3d cr = cross(geo.positions[t[1]] - geo.positions[t[0]], geo.positions[t[2]] - geo.positions[t[0]]);
      const double A = length(cr) / 2;
      if (A <= 0) continue;
      area_in += A;
      ++faces_in;
      edge_sum += (length(geo.positions[t[1]] - geo.positions[t[0]]) + length(geo.positions[t[2]] - geo.positions[t[1]]) + length(geo.positions[t[0]] - geo.positions[t[2]])) / 3;
      const vec3d fn = cr / (2 * A);
      const auto o = view.observe(src, reg.pose, c, normals[t[0]], fn, 0.12, 0.5);
      if (o.conf < 0.5f) continue;
      area_seen += A;
      facing_sum += A * o.facing;
      picture_px += A * o.facing * reg.pose.scale * reg.pose.scale;  // projected area in picture pixels
      if (m && m->indices.size() == geo.tris.size() * 3 && m->texture) {
        const vec2d ua = m->uvs[m->indices[3 * f]], ub = m->uvs[m->indices[3 * f + 1]], uc = m->uvs[m->indices[3 * f + 2]];
        uv_seen += std::abs((ub.x - ua.x) * (uc.y - ua.y) - (ub.y - ua.y) * (uc.x - ua.x)) / 2 * m->texture->width() * m->texture->height();
      }
    }
    const double texel_per_unit = area_seen > 0 ? std::sqrt(uv_seen / area_seen) : 0;
    nlohmann::json j{{"region", {{"centre", {centre.x, centre.y, centre.z}}, {"radius", radius}}},
                     {"registration", {{"yaw", reg.pose.yaw}, {"elevation", reg.pose.elevation}, {"iou", reg.iou}}},
                     {"surface_area", area_in}, {"triangles", faces_in}, {"mean_edge_length", faces_in ? edge_sum / double(faces_in) : 0}, {"seen_fraction", area_in > 0 ? area_seen / area_in : 0},
                     {"mean_facing_seen", area_seen > 0 ? facing_sum / area_seen : 0},
                     {"picture_pixels_per_unit", reg.pose.scale}, {"picture_pixels_covering", picture_px},
                     {"atlas_texels_on_seen_surface", uv_seen}, {"atlas_texels_per_unit", texel_per_unit},
                     {"texels_per_picture_pixel_linear", reg.pose.scale > 0 ? texel_per_unit / reg.pose.scale : 0},
                     {"texture_size", m && m->texture ? m->texture->width() : 0}};
    std::cout << j.dump(2) << std::endl;
    return 0;
  }
  throw std::runtime_error("unknown command " + pos[0]);
}

int main() {
  std::vector<std::string> args;
#ifdef _WIN32
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) args.push_back(to_utf8(fs::path(argv[i])));
  LocalFree(argv);
#endif
  try {
    return run(args);
  } catch (const std::exception& e) {
    std::cerr << "ERROR " << e.what() << std::endl;
    return 2;
  }
}
