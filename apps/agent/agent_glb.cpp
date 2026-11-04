#include "agent_glb.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "pixel3d/common/path_utf8.hpp"

namespace p3dagent {
namespace {

using nlohmann::json;
using Mat4 = std::array<double, 16>;  // column-major, as glTF

constexpr uint32_t kMagic = 0x46546C67, kJson = 0x4E4F534A, kBin = 0x004E4942;
constexpr uint64_t kMaxGlb = 2ull << 30;

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
  uint32_t v;
  std::memcpy(&v, b.data() + at, 4);
  return v;
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  b.insert(b.end(), p, p + 4);
}

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + rr] * b[c * 4 + k];
      r[c * 4 + rr] = s;
    }
  return r;
}
Mat4 local_matrix(const json& node) {
  if (node.contains("matrix") && node["matrix"].is_array() && node["matrix"].size() == 16) {
    Mat4 m;
    for (int i = 0; i < 16; ++i) m[i] = node["matrix"][i].get<double>();
    return m;
  }
  std::array<double, 3> t{0, 0, 0}, s{1, 1, 1};
  std::array<double, 4> q{0, 0, 0, 1};
  if (node.contains("translation")) for (int i = 0; i < 3; ++i) t[i] = node["translation"][i].get<double>();
  if (node.contains("scale")) for (int i = 0; i < 3; ++i) s[i] = node["scale"][i].get<double>();
  if (node.contains("rotation")) for (int i = 0; i < 4; ++i) q[i] = node["rotation"][i].get<double>();
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  const double r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
                       2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                       2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};
  Mat4 m = identity();
  for (int c = 0; c < 3; ++c)
    for (int rr = 0; rr < 3; ++rr) m[c * 4 + rr] = r[c * 3 + rr] * s[c];
  m[12] = t[0];
  m[13] = t[1];
  m[14] = t[2];
  return m;
}

}  // namespace

Glb parse_glb(const std::vector<uint8_t>& b) {
  if (b.size() < 20 || u32(b, 0) != kMagic) throw std::runtime_error("not a GLB file (bad magic or too short)");
  if (u32(b, 4) != 2) throw std::runtime_error("GLB version is not 2");
  const uint32_t length = u32(b, 8);
  if (length > b.size() || length < 20) throw std::runtime_error("GLB is truncated (header length " + std::to_string(length) + ")");
  Glb g;
  size_t at = 12;
  bool have_json = false;
  while (at + 8 <= length) {
    const uint32_t clen = u32(b, at), ctype = u32(b, at + 4);
    if (uint64_t(at) + 8 + clen > length) throw std::runtime_error("GLB chunk runs past the end of the file");
    const auto* p = b.data() + at + 8;
    if (ctype == kJson && !have_json) {
      g.json = json::parse(std::string(reinterpret_cast<const char*>(p), clen));
      have_json = true;
    } else if (ctype == kBin && g.bin.empty()) {
      g.bin.assign(p, p + clen);
    }
    at += 8 + clen;
  }
  if (!have_json) throw std::runtime_error("GLB has no JSON chunk");
  return g;
}

Glb read_glb(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + pixel3d::to_utf8(file));
  const auto size = fs::file_size(file);
  if (size > kMaxGlb) throw std::runtime_error("GLB larger than 2 GiB");
  std::vector<uint8_t> bytes(size);
  in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
  if (!in) throw std::runtime_error("cannot read " + pixel3d::to_utf8(file));
  return parse_glb(bytes);
}

std::vector<uint8_t> encode_glb(const Glb& g) {
  std::string text = g.json.dump();
  while (text.size() % 4) text.push_back(' ');
  std::vector<uint8_t> bin = g.bin;
  while (bin.size() % 4) bin.push_back(0);
  std::vector<uint8_t> out;
  const uint32_t total = uint32_t(12 + 8 + text.size() + (bin.empty() ? 0 : 8 + bin.size()));
  out.reserve(total);
  put32(out, kMagic);
  put32(out, 2);
  put32(out, total);
  put32(out, uint32_t(text.size()));
  put32(out, kJson);
  out.insert(out.end(), text.begin(), text.end());
  if (!bin.empty()) {
    put32(out, uint32_t(bin.size()));
    put32(out, kBin);
    out.insert(out.end(), bin.begin(), bin.end());
  }
  return out;
}

void write_new_file(const fs::path& file, const std::vector<uint8_t>& bytes) {
  if (fs::exists(file)) throw std::runtime_error("refusing to overwrite " + pixel3d::to_utf8(file));
  const auto tmp = fs::path(file).concat(L".partial");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create " + pixel3d::to_utf8(tmp));
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) throw std::runtime_error("write failed: " + pixel3d::to_utf8(tmp));
  }
  std::error_code ec;
  if (fs::exists(file)) {
    fs::remove(tmp, ec);
    throw std::runtime_error("refusing to overwrite " + pixel3d::to_utf8(file));
  }
  fs::rename(tmp, file, ec);
  if (ec) throw std::runtime_error("cannot finish " + pixel3d::to_utf8(file) + ": " + ec.message());
}

json GlbInfo::to_json() const {
  return {{"triangles", triangles}, {"meshes", meshes}, {"primitives", primitives}, {"materials", materials},
          {"textures", textures}, {"images", images}, {"nodes", nodes}, {"has_uv", has_uv}, {"has_normals", has_normals},
          {"textured", textured}, {"aabb_min", min}, {"aabb_max", max}, {"aabb_size", size()}};
}

GlbInfo inspect_glb(const Glb& g) {
  const json& j = g.json;
  GlbInfo info;
  auto count = [&](const char* k) { return j.contains(k) && j[k].is_array() ? int(j[k].size()) : 0; };
  info.meshes = count("meshes");
  info.materials = count("materials");
  info.textures = count("textures");
  info.images = count("images");
  info.nodes = count("nodes");
  for (const auto& m : j.value("materials", json::array())) {
    const auto pbr = m.value("pbrMetallicRoughness", json::object());
    if (pbr.contains("baseColorTexture")) info.textured = true;
  }
  const auto& accessors = j.value("accessors", json::array());
  // Triangles per mesh (counted once per mesh, instanced meshes count per instance below).
  std::vector<int64_t> mesh_tris(info.meshes, 0);
  std::vector<std::array<double, 6>> mesh_box(info.meshes, {1e300, 1e300, 1e300, -1e300, -1e300, -1e300});
  for (int mi = 0; mi < info.meshes; ++mi)
    for (const auto& p : j["meshes"][mi].value("primitives", json::array())) {
      ++info.primitives;
      const auto attrs = p.value("attributes", json::object());
      if (attrs.contains("TEXCOORD_0")) info.has_uv = true;
      if (attrs.contains("NORMAL")) info.has_normals = true;
      if (p.value("mode", 4) != 4 || !attrs.contains("POSITION")) continue;
      const auto& pos = accessors.at(attrs["POSITION"].get<size_t>());
      const int64_t n = p.contains("indices") ? accessors.at(p["indices"].get<size_t>()).value("count", 0) : pos.value("count", 0);
      mesh_tris[mi] += n / 3;
      if (pos.contains("min") && pos.contains("max"))
        for (int k = 0; k < 3; ++k) {
          mesh_box[mi][k] = std::min(mesh_box[mi][k], pos["min"][k].get<double>());
          mesh_box[mi][3 + k] = std::max(mesh_box[mi][3 + k], pos["max"][k].get<double>());
        }
    }
  std::array<double, 6> world{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
  const auto& nodes = j.value("nodes", json::array());
  std::vector<int> depth_guard(nodes.size(), 0);
  // Depth-first from the default scene's roots (or every scene when none is named).
  std::vector<std::pair<int, Mat4>> stack;
  const auto scenes = j.value("scenes", json::array());
  const int scene = j.value("scene", 0);
  if (scene >= 0 && scene < int(scenes.size()))
    for (const auto& n : scenes[scene].value("nodes", json::array())) stack.push_back({n.get<int>(), identity()});
  while (!stack.empty()) {
    auto [ni, parent] = stack.back();
    stack.pop_back();
    if (ni < 0 || ni >= int(nodes.size()) || ++depth_guard[ni] > 1) continue;  // cycles / reuse: visit once
    const auto& node = nodes[ni];
    const Mat4 m = mul(parent, local_matrix(node));
    if (node.contains("mesh")) {
      const int mi = node["mesh"].get<int>();
      if (mi >= 0 && mi < info.meshes) info.triangles += mesh_tris[mi];
      if (mi >= 0 && mi < info.meshes && mesh_box[mi][0] <= mesh_box[mi][3]) {
        for (int c = 0; c < 8; ++c) {
          const double p[3] = {(c & 1) ? mesh_box[mi][3] : mesh_box[mi][0], (c & 2) ? mesh_box[mi][4] : mesh_box[mi][1],
                               (c & 4) ? mesh_box[mi][5] : mesh_box[mi][2]};
          for (int k = 0; k < 3; ++k) {
            const double v = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
            world[k] = std::min(world[k], v);
            world[3 + k] = std::max(world[3 + k], v);
          }
        }
      }
    }
    for (const auto& c : node.value("children", json::array())) stack.push_back({c.get<int>(), m});
  }
  if (world[0] <= world[3])
    for (int k = 0; k < 3; ++k) {
      info.min[k] = world[k];
      info.max[k] = world[3 + k];
    }
  return info;
}

DeliveryTransform delivery_transform(const GlbInfo& info, ScaleAxis axis, double metres) {
  const auto s = info.size();
  DeliveryTransform t;
  t.source_extent = axis == ScaleAxis::Height ? s[1] : std::max({s[0], s[1], s[2]});
  if (!(t.source_extent > 0)) throw std::runtime_error("the model has no extent along the scale axis");
  t.scale = metres / t.source_extent;
  t.translation = {-t.scale * (info.min[0] + info.max[0]) / 2, -t.scale * info.min[1], -t.scale * (info.min[2] + info.max[2]) / 2};
  return t;
}

std::array<double, 3> rotate_y(const std::array<double, 3>& p, double deg) {
  const double r = deg * 3.14159265358979323846 / 180, c = std::cos(r), s = std::sin(r);
  return {c * p[0] + s * p[2], p[1], -s * p[0] + c * p[2]};
}

double front_rotation_deg(double registered_yaw_deg) {
  double r = std::fmod(-registered_yaw_deg, 360.0);
  if (r <= -180) r += 360;
  if (r > 180) r -= 360;
  return r;
}

DeliveryTransform delivery_transform(const Glb& g, ScaleAxis axis, double metres, double rotation_y_deg) {
  // World positions of every float POSITION accessor through the node hierarchy, rotated.
  const json& j = g.json;
  std::array<double, 3> lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  const auto& nodes = j.value("nodes", json::array());
  const auto& accessors = j.value("accessors", json::array());
  std::vector<std::pair<int, Mat4>> stack;
  const auto scenes = j.value("scenes", json::array());
  const int scene = j.value("scene", 0);
  if (scene >= 0 && scene < int(scenes.size()))
    for (const auto& n : scenes[scene].value("nodes", json::array())) stack.push_back({n.get<int>(), identity()});
  std::vector<int> seen(nodes.size(), 0);
  while (!stack.empty()) {
    auto [ni, parent] = stack.back();
    stack.pop_back();
    if (ni < 0 || ni >= int(nodes.size()) || ++seen[ni] > 1) continue;
    const auto& node = nodes[ni];
    const Mat4 m = mul(parent, local_matrix(node));
    if (node.contains("mesh"))
      for (const auto& p : j.at("meshes").at(node["mesh"].get<size_t>()).value("primitives", json::array())) {
        const auto attrs = p.value("attributes", json::object());
        if (!attrs.contains("POSITION")) continue;
        const auto& acc = accessors.at(attrs["POSITION"].get<size_t>());
        if (acc.value("componentType", 0) != 5126 || acc.value("type", std::string()) != "VEC3" || !acc.contains("bufferView"))
          throw std::runtime_error("POSITION is not float VEC3: cannot measure the rotated box");
        const auto& view = j.at("bufferViews").at(acc["bufferView"].get<size_t>());
        const size_t stride = view.value("byteStride", 12), base = size_t(view.value("byteOffset", 0)) + size_t(acc.value("byteOffset", 0));
        const size_t count = acc["count"].get<size_t>();
        if (stride == 0 || !span_ok(base, count, stride, 12, g.bin.size())) throw std::runtime_error("POSITION out of range");
        for (size_t i = 0; i < count; ++i) {
          float q[3];
          std::memcpy(q, g.bin.data() + base + i * stride, 12);
          std::array<double, 3> w;
          for (int k = 0; k < 3; ++k) w[size_t(k)] = m[k] * q[0] + m[4 + k] * q[1] + m[8 + k] * q[2] + m[12 + k];
          w = rotate_y(w, rotation_y_deg);
          for (int k = 0; k < 3; ++k) {
            lo[size_t(k)] = std::min(lo[size_t(k)], w[size_t(k)]);
            hi[size_t(k)] = std::max(hi[size_t(k)], w[size_t(k)]);
          }
        }
      }
    for (const auto& c : node.value("children", json::array())) stack.push_back({c.get<int>(), m});
  }
  if (!(lo[0] <= hi[0])) throw std::runtime_error("the model has no vertices");
  GlbInfo box;
  box.min = lo;
  box.max = hi;
  auto t = delivery_transform(box, axis, metres);
  t.rotation_y_deg = rotation_y_deg;
  return t;
}

Glb with_root_transform(Glb g, const DeliveryTransform& t, const std::string& root_name) {
  auto& j = g.json;
  if (!j.contains("nodes")) j["nodes"] = json::array();
  const int root = int(j["nodes"].size());
  if (!j.contains("scenes") || j["scenes"].empty()) throw std::runtime_error("GLB has no scene to deliver");
  const int scene = j.value("scene", 0);
  auto& sc = j["scenes"].at(scene);
  json node = {{"name", root_name}, {"scale", {t.scale, t.scale, t.scale}}, {"translation", t.translation},
               {"children", sc.value("nodes", json::array())}};
  if (t.rotation_y_deg != 0) {
    const double h = t.rotation_y_deg * 3.14159265358979323846 / 360;
    node["rotation"] = {0.0, std::sin(h), 0.0, std::cos(h)};
  }
  j["nodes"].push_back(node);
  sc["nodes"] = json::array({root});
  j["scene"] = scene;
  return g;
}

std::optional<Glb> with_baked_transform(Glb g, const DeliveryTransform& t) {
  auto& j = g.json;
  const auto nodes = j.value("nodes", json::array());
  const auto scenes = j.value("scenes", json::array());
  const int scene = j.value("scene", 0);
  if (nodes.size() != 1 || scene < 0 || scene >= int(scenes.size()) || scenes[scene].value("nodes", json::array()) != json::array({0}))
    return std::nullopt;
  const auto& node = nodes[0];
  for (const char* k : {"matrix", "translation", "rotation", "scale", "children", "skin"})
    if (node.contains(k)) return std::nullopt;
  if (!node.contains("mesh") || !node["mesh"].is_number_integer()) return std::nullopt;
  const int mi = node["mesh"].get<int>();
  if (mi < 0 || mi >= int(j.value("meshes", json::array()).size())) return std::nullopt;
  std::vector<int> positions, directions;  // directions: NORMAL (VEC3) and TANGENT (VEC4, xyz) turn with a rotation
  for (const auto& p : j["meshes"][mi].value("primitives", json::array())) {
    const auto attrs = p.value("attributes", json::object());
    if (!attrs.contains("POSITION") || p.contains("targets")) return std::nullopt;
    const int a = attrs["POSITION"].get<int>();
    if (std::find(positions.begin(), positions.end(), a) == positions.end()) positions.push_back(a);
    if (t.rotation_y_deg != 0)
      for (const char* k : {"NORMAL", "TANGENT"})
        if (attrs.contains(k) && std::find(directions.begin(), directions.end(), attrs[k].get<int>()) == directions.end())
          directions.push_back(attrs[k].get<int>());
  }
  if (positions.empty()) return std::nullopt;
  // 3re's textured GLBs keep the welded original topology in a second accessor named in asset.extras
  // (pixel3d_topology); the importer checks it against the rendered triangles, so it moves with them.
  const auto extras = j.value("asset", json::object()).value("extras", json::object());
  if (extras.contains("pixel3d_topology")) {
    const auto& topo = extras["pixel3d_topology"];
    if (!topo.is_object() || !topo.contains("position_accessor") || !topo["position_accessor"].is_number_integer()) return std::nullopt;
    const int a = topo["position_accessor"].get<int>();
    if (std::find(positions.begin(), positions.end(), a) == positions.end()) positions.push_back(a);
  }
  auto& accessors = j["accessors"];
  for (int a : positions) {
    if (a < 0 || a >= int(accessors.size())) return std::nullopt;
    const auto& acc = accessors[a];
    if (acc.value("componentType", 0) != 5126 || acc.value("type", std::string()) != "VEC3" || acc.value("normalized", false) ||
        acc.contains("sparse") || !acc.contains("bufferView"))
      return std::nullopt;
    const auto& view = j["bufferViews"].at(acc["bufferView"].get<size_t>());
    if (view.value("buffer", 0) != 0) return std::nullopt;
    // Another accessor reading the same bytes (e.g. a shared view) would be moved too: refuse.
    for (int other = 0; other < int(accessors.size()); ++other)
      if (other != a && accessors[other].value("bufferView", -1) == acc["bufferView"].get<int>()) return std::nullopt;
  }
  for (int a : positions) {
    auto& acc = accessors[a];
    const auto& view = j["bufferViews"][acc["bufferView"].get<size_t>()];
    const size_t stride = view.value("byteStride", 12), base = size_t(view.value("byteOffset", 0)) + size_t(acc.value("byteOffset", 0));
    const size_t count = acc["count"].get<size_t>();
    if (count == 0 || stride == 0 || !span_ok(base, count, stride, 12, g.bin.size())) return std::nullopt;
    std::array<double, 3> lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
    for (size_t i = 0; i < count; ++i) {
      float p[3];
      std::memcpy(p, g.bin.data() + base + i * stride, 12);
      const auto r = rotate_y({double(p[0]), double(p[1]), double(p[2])}, t.rotation_y_deg);
      for (int k = 0; k < 3; ++k) {
        p[k] = float(t.scale * r[size_t(k)] + t.translation[k]);
        lo[k] = std::min(lo[k], double(p[k]));
        hi[k] = std::max(hi[k], double(p[k]));
      }
      std::memcpy(g.bin.data() + base + i * stride, p, 12);
    }
    acc["min"] = {float(lo[0]), float(lo[1]), float(lo[2])};
    acc["max"] = {float(hi[0]), float(hi[1]), float(hi[2])};
  }
  for (int a : directions) {
    if (a < 0 || a >= int(accessors.size())) return std::nullopt;
    auto& acc = accessors[a];
    const auto type = acc.value("type", std::string());
    if (acc.value("componentType", 0) != 5126 || (type != "VEC3" && type != "VEC4") || acc.contains("sparse") || !acc.contains("bufferView"))
      return std::nullopt;
    const auto& view = j["bufferViews"][acc["bufferView"].get<size_t>()];
    const size_t width = type == "VEC3" ? 12 : 16;
    const size_t stride = view.value("byteStride", width), base = size_t(view.value("byteOffset", 0)) + size_t(acc.value("byteOffset", 0));
    const size_t count = acc["count"].get<size_t>();
    if (count == 0 || stride == 0 || !span_ok(base, count, stride, width, g.bin.size())) return std::nullopt;
    for (size_t i = 0; i < count; ++i) {
      float d[3];
      std::memcpy(d, g.bin.data() + base + i * stride, 12);
      const auto r = rotate_y({double(d[0]), double(d[1]), double(d[2])}, t.rotation_y_deg);
      for (int k = 0; k < 3; ++k) d[k] = float(r[size_t(k)]);
      std::memcpy(g.bin.data() + base + i * stride, d, 12);
    }
    acc.erase("min");
    acc.erase("max");
  }
  return g;
}

}  // namespace p3dagent
