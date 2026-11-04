#include "agent_fidelity.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>
#include <unordered_map>

#include "pixel3d/image/image.hpp"
#include "tri_bvh.h"

namespace p3dagent {
namespace {

using nlohmann::json;
using Mat4 = std::array<double, 16>;  // column-major (glTF)
constexpr double kPi = 3.14159265358979323846;

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int c = 0; c < 4; ++c)
    for (int r0 = 0; r0 < 4; ++r0) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + r0] * b[c * 4 + k];
      r[c * 4 + r0] = s;
    }
  return r;
}
Mat4 local_matrix(const json& n) {
  if (n.contains("matrix") && n["matrix"].is_array() && n["matrix"].size() == 16) {
    Mat4 m;
    for (int i = 0; i < 16; ++i) m[size_t(i)] = n["matrix"][size_t(i)].get<double>();
    return m;
  }
  Mat4 t = identity(), r = identity(), s = identity();
  if (n.contains("translation"))
    for (int i = 0; i < 3; ++i) t[12 + i] = n["translation"].at(size_t(i)).get<double>();
  if (n.contains("rotation")) {
    const double x = n["rotation"].at(0), y = n["rotation"].at(1), z = n["rotation"].at(2), w = n["rotation"].at(3);
    r = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0, 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0, 0, 0, 0, 1};
  }
  if (n.contains("scale"))
    for (int i = 0; i < 3; ++i) s[size_t(i * 5)] = n["scale"].at(size_t(i)).get<double>();
  return mul(t, mul(r, s));
}

struct View {
  const uint8_t* data = nullptr;
  size_t count = 0, stride = 0;
  int comp = 0, ncomp = 0;
  bool normalized = false;
};
View accessor(const Glb& g, int index) {
  const auto& a = g.json.at("accessors").at(size_t(index));
  if (!a.contains("bufferView")) throw std::runtime_error("sparse or empty accessor");
  const auto& bv = g.json.at("bufferViews").at(a["bufferView"].get<size_t>());
  if (bv.value("buffer", 0) != 0) throw std::runtime_error("accessor outside the GLB binary chunk");
  static const std::map<std::string, int> kN{{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}};
  View v;
  v.comp = a.at("componentType");
  v.ncomp = kN.at(a.at("type").get<std::string>());
  v.count = a.at("count");
  v.normalized = a.value("normalized", false);
  const int csize = v.comp == 5126 || v.comp == 5125 ? 4 : v.comp == 5123 || v.comp == 5122 ? 2 : 1;
  v.stride = bv.value("byteStride", size_t(csize * v.ncomp));
  const size_t bv_off = bv.value("byteOffset", size_t(0)), bv_len = bv.at("byteLength").get<size_t>();
  size_t off = 0, bv_end = 0;
  if (__builtin_add_overflow(bv_off, a.value("byteOffset", size_t(0)), &off) || __builtin_add_overflow(bv_off, bv_len, &bv_end) ||
      v.stride == 0 || !span_ok(off, v.count, v.stride, size_t(csize * v.ncomp), std::min(g.bin.size(), bv_end)))
    throw std::runtime_error("accessor out of range");
  v.data = g.bin.data() + off;
  return v;
}
double comp_at(const View& v, size_t i, int k) {
  const uint8_t* p = v.data + i * v.stride;
  switch (v.comp) {
    case 5126: { float f; std::memcpy(&f, p + 4 * k, 4); return f; }
    case 5125: { uint32_t u; std::memcpy(&u, p + 4 * k, 4); return u; }
    case 5123: { uint16_t u; std::memcpy(&u, p + 2 * k, 2); return v.normalized ? u / 65535.0 : u; }
    case 5121: return v.normalized ? p[k] / 255.0 : p[k];
    case 5122: { int16_t s; std::memcpy(&s, p + 2 * k, 2); return v.normalized ? std::max(s / 32767.0, -1.0) : s; }
    case 5120: return v.normalized ? std::max(int8_t(p[k]) / 127.0, -1.0) : int8_t(p[k]);
  }
  throw std::runtime_error("unsupported accessor component type");
}

std::array<float, 3> oklab_srgb8(const uint8_t* c) {
  auto lin = [](uint8_t v) {
    const double x = v / 255.0;
    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
  };
  const double r = lin(c[0]), g = lin(c[1]), b = lin(c[2]);
  const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  return {float(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s), float(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
          float(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)};
}

struct Box {
  std::array<double, 3> lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  double diag() const {
    double s = 0;
    for (int k = 0; k < 3; ++k) s += (hi[k] - lo[k]) * (hi[k] - lo[k]);
    return std::sqrt(s);
  }
};
Box box(const FMesh& m) {
  Box b;
  for (const auto& p : m.pos)
    for (int k = 0; k < 3; ++k) {
      b.lo[k] = std::min(b.lo[k], double(p[k]));
      b.hi[k] = std::max(b.hi[k], double(p[k]));
    }
  return b;
}

// One view: per pixel the front-most triangle and its barycentrics.
struct Frame {
  int n = 0;
  std::vector<int32_t> face;
  std::vector<std::array<float, 3>> bary;
  std::array<double, 3> fwd{}, right{}, up{};
};
Frame raster(const FMesh& m, double yaw_deg, double el_deg, int n, std::array<double, 3> c, double radius) {
  const double yaw = yaw_deg * kPi / 180, el = el_deg * kPi / 180;
  const std::array<double, 3> to_cam{std::sin(yaw) * std::cos(el), std::sin(el), std::cos(yaw) * std::cos(el)};
  Frame f;
  f.n = n;
  f.fwd = {-to_cam[0], -to_cam[1], -to_cam[2]};
  // right = fwd x up(0,1,0); up = right x fwd
  f.right = {f.fwd[1] * 0 - f.fwd[2] * 1, f.fwd[2] * 0 - f.fwd[0] * 0, f.fwd[0] * 1 - f.fwd[1] * 0};
  double rl = std::sqrt(f.right[0] * f.right[0] + f.right[1] * f.right[1] + f.right[2] * f.right[2]);
  if (rl < 1e-9) {  // looking straight down/up: right = +X
    f.right = {1, 0, 0};
    rl = 1;
  }
  for (auto& v : f.right) v /= rl;
  f.up = {f.right[1] * f.fwd[2] - f.right[2] * f.fwd[1], f.right[2] * f.fwd[0] - f.right[0] * f.fwd[2], f.right[0] * f.fwd[1] - f.right[1] * f.fwd[0]};
  const double s = n * 0.44 / std::max(radius, 1e-12);
  std::vector<std::array<double, 3>> q(m.pos.size());
  for (size_t i = 0; i < m.pos.size(); ++i) {
    const double d[3] = {m.pos[i][0] - c[0], m.pos[i][1] - c[1], m.pos[i][2] - c[2]};
    q[i] = {n / 2.0 + s * (d[0] * f.right[0] + d[1] * f.right[1] + d[2] * f.right[2]),
            n / 2.0 - s * (d[0] * f.up[0] + d[1] * f.up[1] + d[2] * f.up[2]), d[0] * f.fwd[0] + d[1] * f.fwd[1] + d[2] * f.fwd[2]};
  }
  f.face.assign(size_t(n) * n, -1);
  f.bary.assign(size_t(n) * n, {0, 0, 0});
  std::vector<double> depth(size_t(n) * n, std::numeric_limits<double>::infinity());
  for (size_t t = 0; t < m.tri.size(); ++t) {
    const auto& a = q[m.tri[t][0]];
    const auto& b = q[m.tri[t][1]];
    const auto& cc = q[m.tri[t][2]];
    const double area = (b[0] - a[0]) * (cc[1] - a[1]) - (b[1] - a[1]) * (cc[0] - a[0]);
    if (std::abs(area) < 1e-12) continue;
    const int x0 = std::max(0, int(std::floor(std::min({a[0], b[0], cc[0]})))), x1 = std::min(n - 1, int(std::ceil(std::max({a[0], b[0], cc[0]}))));
    const int y0 = std::max(0, int(std::floor(std::min({a[1], b[1], cc[1]})))), y1 = std::min(n - 1, int(std::ceil(std::max({a[1], b[1], cc[1]}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((b[0] - px) * (cc[1] - py) - (cc[0] - px) * (b[1] - py)) / area;
        const double w1 = ((cc[0] - px) * (a[1] - py) - (a[0] - px) * (cc[1] - py)) / area;
        const double w2 = 1 - w0 - w1;
        if (w0 < -1e-9 || w1 < -1e-9 || w2 < -1e-9) continue;
        const double z = w0 * a[2] + w1 * b[2] + w2 * cc[2];
        const size_t i = size_t(y) * n + x;
        if (z < depth[i]) {
          depth[i] = z;
          f.face[i] = int32_t(t);
          f.bary[i] = {float(w0), float(w1), float(w2)};
        }
      }
  }
  return f;
}

std::array<double, 3> face_normal(const FMesh& m, size_t t) {
  const auto& a = m.pos[m.tri[t][0]];
  const auto& b = m.pos[m.tri[t][1]];
  const auto& c = m.pos[m.tri[t][2]];
  const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  std::array<double, 3> n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
  const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (l > 0)
    for (auto& x : n) x /= l;
  return n;
}

// Base colour at a pixel (sRGB 8 bit, bilinear); false = no texture on that triangle.
bool sample(const FMesh& m, int32_t t, const std::array<float, 3>& w, uint8_t out[4]) {
  if (m.uv.empty() || t < 0 || m.tri_image[size_t(t)] < 0) return false;
  const auto& img = m.images[size_t(m.tri_image[size_t(t)])];
  const auto& tr = m.tri[size_t(t)];
  double u = 0, v = 0;
  for (int k = 0; k < 3; ++k) {
    u += w[size_t(k)] * m.uv[tr[size_t(k)]][0];
    v += w[size_t(k)] * m.uv[tr[size_t(k)]][1];
  }
  u -= std::floor(u);
  v -= std::floor(v);
  const double x = u * img.w - 0.5, y = v * img.h - 0.5;
  const int x0 = std::clamp(int(std::floor(x)), 0, img.w - 1), y0 = std::clamp(int(std::floor(y)), 0, img.h - 1);
  const int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
  const double fx = std::clamp(x - std::floor(x), 0.0, 1.0), fy = std::clamp(y - std::floor(y), 0.0, 1.0);
  for (int k = 0; k < 4; ++k) {
    auto at = [&](int xx, int yy) { return double(img.rgba[(size_t(yy) * img.w + xx) * 4 + k]); };
    const double val = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
    out[k] = uint8_t(std::clamp(std::lround(val), 0L, 255L));
  }
  return true;
}

std::vector<std::array<float, 3>> surface_samples(const FMesh& m, int count, uint32_t seed) {
  std::vector<double> cum(m.tri.size(), 0);
  double total = 0;
  for (size_t t = 0; t < m.tri.size(); ++t) {
    const auto& a = m.pos[m.tri[t][0]];
    const auto& b = m.pos[m.tri[t][1]];
    const auto& c = m.pos[m.tri[t][2]];
    const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const double x = u[1] * v[2] - u[2] * v[1], y = u[2] * v[0] - u[0] * v[2], z = u[0] * v[1] - u[1] * v[0];
    total += 0.5 * std::sqrt(x * x + y * y + z * z);
    cum[t] = total;
  }
  std::vector<std::array<float, 3>> out;
  if (!(total > 0)) return out;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> U(0, 1);
  out.reserve(size_t(count));
  for (int i = 0; i < count; ++i) {
    const size_t t = size_t(std::lower_bound(cum.begin(), cum.end(), U(rng) * total) - cum.begin());
    const auto& tr = m.tri[std::min(t, m.tri.size() - 1)];
    double r1 = std::sqrt(U(rng)), r2 = U(rng);
    const double w0 = 1 - r1, w1 = r1 * (1 - r2), w2 = r1 * r2;
    std::array<float, 3> p;
    for (int k = 0; k < 3; ++k) p[size_t(k)] = float(w0 * m.pos[tr[0]][size_t(k)] + w1 * m.pos[tr[1]][size_t(k)] + w2 * m.pos[tr[2]][size_t(k)]);
    out.push_back(p);
  }
  return out;
}

std::vector<double> distances(const std::vector<std::array<float, 3>>& pts, const FMesh& to) {
  std::vector<float> v;
  v.reserve(to.pos.size() * 3);
  for (const auto& p : to.pos) v.insert(v.end(), p.begin(), p.end());
  std::vector<int32_t> f;
  f.reserve(to.tri.size() * 3);
  for (const auto& t : to.tri) f.insert(f.end(), {int32_t(t[0]), int32_t(t[1]), int32_t(t[2])});
  const auto bvh = trellis::TriBvh::build(v.data(), int64_t(to.pos.size()), f.data(), int64_t(to.tri.size()));
  std::vector<double> d(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) d[i] = std::sqrt(double(bvh.closest(pts[i].data()).dist2));
  return d;
}

}  // namespace

bool FMesh::textured() const { return !uv.empty() && std::any_of(tri_image.begin(), tri_image.end(), [](int32_t i) { return i >= 0; }); }

FMesh fmesh_from_glb(const Glb& g) {
  FMesh m;
  const auto& j = g.json;
  std::map<int, int> image_slot;  // glTF image index -> m.images
  auto image_of_material = [&](const json& prim) -> int32_t {
    if (!prim.contains("material") || !j.contains("materials")) return -1;
    const auto& mat = j["materials"].at(prim["material"].get<size_t>());
    const auto pbr = mat.value("pbrMetallicRoughness", json::object());
    if (!pbr.contains("baseColorTexture")) return -1;
    const auto& tex = j.at("textures").at(pbr["baseColorTexture"]["index"].get<size_t>());
    int src = tex.value("source", -1);
    if (tex.contains("extensions"))
      for (const auto& [name, e] : tex["extensions"].items())
        if (e.is_object() && e.contains("source")) src = e["source"];
    if (src < 0) return -1;
    if (auto it = image_slot.find(src); it != image_slot.end()) return it->second;
    const auto& im = j.at("images").at(size_t(src));
    if (!im.contains("bufferView")) throw std::runtime_error("external image URIs are not supported");
    const auto& bv = j.at("bufferViews").at(im["bufferView"].get<size_t>());
    const size_t off = bv.value("byteOffset", size_t(0)), len = bv.at("byteLength");
    if (bv.value("buffer", 0) != 0 || !span_ok(off, 1, 0, len, g.bin.size())) throw std::runtime_error("image out of range");
    auto decoded = pixel3d::decode_image(std::span<const uint8_t>(g.bin.data() + off, len), im.value("mimeType", std::string()));
    if (!decoded) throw std::runtime_error("cannot decode an embedded image");
    FMesh::Image out;
    out.w = decoded->width();
    out.h = decoded->height();
    out.rgba.resize(size_t(out.w) * out.h * 4);
    for (int y = 0; y < out.h; ++y)
      for (int x = 0; x < out.w; ++x) {
        const auto p = decoded->at(x, y);
        uint8_t* o = &out.rgba[(size_t(y) * out.w + x) * 4];
        o[0] = p.r; o[1] = p.g; o[2] = p.b; o[3] = p.a;
      }
    m.images.push_back(std::move(out));
    image_slot[src] = int(m.images.size() - 1);
    return int32_t(m.images.size() - 1);
  };
  bool any_uv = false;
  // Each node once, at most 64 levels deep: a cyclic or absurdly deep hierarchy in a foreign file never recurses forever.
  std::vector<uint8_t> visited(j.value("nodes", json::array()).size(), 0);
  std::function<void(int, const Mat4&, int)> visit = [&](int ni, const Mat4& parent, int depth) {
    if (ni < 0 || size_t(ni) >= visited.size() || visited[size_t(ni)] || depth > 64) throw std::runtime_error("invalid or cyclic node hierarchy");
    visited[size_t(ni)] = 1;
    const auto& node = j.at("nodes").at(size_t(ni));
    const Mat4 world = mul(parent, local_matrix(node));
    if (node.contains("mesh"))
      for (const auto& prim : j.at("meshes").at(node["mesh"].get<size_t>()).at("primitives")) {
        if (prim.value("mode", 4) != 4) continue;
        const auto pv = accessor(g, prim.at("attributes").at("POSITION"));
        if (pv.ncomp != 3) throw std::runtime_error("POSITION must be VEC3");
        if (m.pos.size() + pv.count > (size_t(1) << 31)) throw std::runtime_error("too many vertices");
        const uint32_t base = uint32_t(m.pos.size());
        const int32_t prim_id = m.vert_prim.empty() ? 0 : m.vert_prim.back() + 1;
        // glTF: a mirrored node (negative determinant) draws its triangles with the winding reversed.
        const double det = world[0] * (world[5] * world[10] - world[9] * world[6]) - world[4] * (world[1] * world[10] - world[9] * world[2]) +
                           world[8] * (world[1] * world[6] - world[5] * world[2]);
        const bool mirrored = det < 0;
        for (size_t i = 0; i < pv.count; ++i) {
          const double x = comp_at(pv, i, 0), y = comp_at(pv, i, 1), z = comp_at(pv, i, 2);
          m.pos.push_back({float(world[0] * x + world[4] * y + world[8] * z + world[12]), float(world[1] * x + world[5] * y + world[9] * z + world[13]),
                           float(world[2] * x + world[6] * y + world[10] * z + world[14])});
          m.vert_prim.push_back(prim_id);
        }
        const bool has_uv = prim["attributes"].contains("TEXCOORD_0");
        if (has_uv && !any_uv) {
          m.uv.assign(base, {0, 0});
          any_uv = true;
        }
        if (any_uv) {
          if (has_uv) {
            const auto uv = accessor(g, prim["attributes"]["TEXCOORD_0"]);
            if (uv.count != pv.count || uv.ncomp != 2) throw std::runtime_error("TEXCOORD_0 must be VEC2 with POSITION's count");
            for (size_t i = 0; i < uv.count; ++i) m.uv.push_back({float(comp_at(uv, i, 0)), float(comp_at(uv, i, 1))});
          } else m.uv.resize(m.pos.size(), {0, 0});
        }
        const int32_t img = has_uv ? image_of_material(prim) : -1;
        if (prim.contains("indices")) {
          const auto iv = accessor(g, prim["indices"]);
          for (size_t i = 0; i + 2 < iv.count; i += 3) {
            std::array<uint32_t, 3> t;
            for (int k = 0; k < 3; ++k) {
              const double idx = comp_at(iv, i + k, 0);
              if (idx >= double(pv.count)) throw std::runtime_error("index out of range");
              t[size_t(k)] = base + uint32_t(idx);
            }
            if (mirrored) std::swap(t[1], t[2]);
            m.tri.push_back(t);
            m.tri_image.push_back(img);
          }
        } else
          for (uint32_t i = 0; i + 2 < pv.count; i += 3) {
            m.tri.push_back(mirrored ? std::array<uint32_t, 3>{base + i, base + i + 2, base + i + 1} : std::array<uint32_t, 3>{base + i, base + i + 1, base + i + 2});
            m.tri_image.push_back(img);
          }
      }
    if (node.contains("children"))
      for (const auto& c : node["children"]) visit(c.get<int>(), world, depth + 1);
  };
  const int scene = j.value("scene", 0);
  if (j.contains("scenes") && j["scenes"].size() > size_t(scene))
    for (const auto& n : j["scenes"][size_t(scene)].value("nodes", json::array())) visit(n.get<int>(), identity(), 0);
  if (m.tri.empty()) throw std::runtime_error("the GLB has no triangles");
  if (any_uv) m.uv.resize(m.pos.size(), {0, 0});
  return m;
}

json Topology::to_json() const {
  return {{"triangles", triangles}, {"boundary_edges", boundary_edges}, {"boundary_loops", boundary_loops}, {"overfull_edges", overfull_edges},
          {"inconsistent_winding_edges", inconsistent_winding_edges}, {"degenerate_triangles", degenerate_triangles}};
}

Topology topology(const FMesh& m) {
  // Textured meshes duplicate vertices along UV seams: topology is read on positions (exact) within each primitive,
  // so a seam is not a hole and two separate parts that touch are not a non-manifold edge. Untextured meshes are
  // read on their indices as stored (touching sheets stay separate).
  std::vector<uint32_t> id(m.pos.size());
  if (!m.uv.empty()) {
    std::map<std::pair<int32_t, std::array<float, 3>>, uint32_t> weld;
    for (size_t i = 0; i < m.pos.size(); ++i)
      id[i] = weld.emplace(std::make_pair(i < m.vert_prim.size() ? m.vert_prim[i] : 0, m.pos[i]), uint32_t(weld.size())).first->second;
  } else std::iota(id.begin(), id.end(), 0u);
  Topology t;
  t.triangles = int64_t(m.tri.size());
  const double d = box(m).diag();
  std::unordered_map<uint64_t, std::array<int, 2>> edges;  // key min/max -> {count, count of min->max direction}
  for (size_t f = 0; f < m.tri.size(); ++f) {
    const uint32_t v[3] = {id[m.tri[f][0]], id[m.tri[f][1]], id[m.tri[f][2]]};
    const auto n = face_normal(m, f);
    const auto& a = m.pos[m.tri[f][0]];
    const auto& b = m.pos[m.tri[f][1]];
    const auto& c = m.pos[m.tri[f][2]];
    const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const double cx = u[1] * w[2] - u[2] * w[1], cy = u[2] * w[0] - u[0] * w[2], cz = u[0] * w[1] - u[1] * w[0];
    if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2] || std::sqrt(cx * cx + cy * cy + cz * cz) <= 1e-14 * d * d) {
      ++t.degenerate_triangles;
      (void)n;
      continue;
    }
    for (int k = 0; k < 3; ++k) {
      const uint32_t p = v[k], q = v[(k + 1) % 3];
      const uint64_t key = (uint64_t(std::min(p, q)) << 32) | std::max(p, q);
      auto& e = edges[key];
      ++e[0];
      if (p < q) ++e[1];
    }
  }
  std::vector<uint32_t> parent;
  std::unordered_map<uint32_t, uint32_t> slot;
  auto find = [&](uint32_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  };
  for (const auto& [key, e] : edges) {
    if (e[0] == 1) {
      ++t.boundary_edges;
      const uint32_t a = uint32_t(key >> 32), b = uint32_t(key & 0xffffffffu);
      for (uint32_t x : {a, b})
        if (!slot.count(x)) {
          slot[x] = uint32_t(parent.size());
          parent.push_back(uint32_t(parent.size()));
        }
      const uint32_t ra = find(slot[a]), rb = find(slot[b]);
      if (ra != rb) parent[ra] = rb;
    } else if (e[0] > 2) ++t.overfull_edges;
    else if (e[0] == 2 && e[1] != 1) ++t.inconsistent_winding_edges;
  }
  for (uint32_t i = 0; i < parent.size(); ++i) t.boundary_loops += find(i) == i;
  return t;
}

std::vector<uint8_t> render_unlit(const FMesh& m, double yaw_deg, double elevation_deg, int size, std::array<double, 3> centre, double radius) {
  if (radius <= 0) {
    const auto b = box(m);
    centre = {(b.lo[0] + b.hi[0]) / 2, (b.lo[1] + b.hi[1]) / 2, (b.lo[2] + b.hi[2]) / 2};
    radius = b.diag() / 2;
  }
  const auto f = raster(m, yaw_deg, elevation_deg, size, centre, radius);
  std::vector<uint8_t> out(size_t(size) * size * 4, 0);
  for (size_t i = 0; i < f.face.size(); ++i) {
    if (f.face[i] < 0) continue;
    uint8_t c[4] = {200, 200, 200, 255};
    if (!sample(m, f.face[i], f.bary[i], c)) {
      const auto n = face_normal(m, size_t(f.face[i]));
      const double k = 0.35 + 0.65 * std::abs(n[0] * f.fwd[0] + n[1] * f.fwd[1] + n[2] * f.fwd[2]);
      c[0] = c[1] = c[2] = uint8_t(std::lround(200 * k));
    }
    std::memcpy(&out[i * 4], c, 3);
    out[i * 4 + 3] = 255;
  }
  return out;
}

json Fidelity::to_json() const {
  return {{"distance", {{"mean_pct", distance_mean_pct}, {"p95_pct", distance_p95_pct}, {"max_pct", distance_max_pct},
                        {"measure", "symmetric, area-weighted surface samples to the other surface, % of the original's bounding-box diagonal"}}},
          {"silhouette_iou", {{"per_view", silhouette_iou}, {"min", silhouette_iou_min}, {"mean", silhouette_iou_mean},
                              {"views", "yaw 0,45,...,315 at elevation 10 + top; orthographic; the original's frame for both"}}},
          {"normal_mean_deg", normal_mean_deg},
          {"colour", colour_mean_de < 0 ? json(nullptr)
                                         : json{{"mean_delta_e", colour_mean_de}, {"p95_delta_e", colour_p95_de},
                                                {"block_mean_delta_e", colour_block_mean_de}, {"block_p95_delta_e", colour_block_p95_de},
                                                {"space", "Oklab, unlit base colour; per pixel and on 8x8-pixel block means (judged)"}}},
          {"topology", {{"original", original.to_json()}, {"reduced", reduced.to_json()}}}};
}

Fidelity measure_fidelity(const FMesh& a, const FMesh& b, const FidelityOptions& opt) {
  Fidelity r;
  const auto bx = box(a);
  const double diag = bx.diag();
  if (!(diag > 0)) throw std::runtime_error("the original has no extent");
  // Distance.
  auto da = distances(surface_samples(a, opt.samples, 1), b), db = distances(surface_samples(b, opt.samples, 2), a);
  da.insert(da.end(), db.begin(), db.end());
  std::sort(da.begin(), da.end());
  if (!da.empty()) {
    r.distance_mean_pct = std::accumulate(da.begin(), da.end(), 0.0) / double(da.size()) / diag * 100;
    r.distance_p95_pct = da[std::min(da.size() - 1, size_t(0.95 * double(da.size())))] / diag * 100;
    r.distance_max_pct = da.back() / diag * 100;
  }
  // Views.
  const std::array<double, 3> c{(bx.lo[0] + bx.hi[0]) / 2, (bx.lo[1] + bx.hi[1]) / 2, (bx.lo[2] + bx.hi[2]) / 2};
  const bool colour = opt.colour && a.textured() && b.textured();
  double nsum = 0;
  size_t ncount = 0;
  std::vector<double> des, block_des;
  std::vector<std::pair<double, double>> views;
  for (int y = 0; y < 360; y += 45) views.push_back({double(y), 10.0});
  views.push_back({0.0, 89.0});
  for (const auto& [yaw, el] : views) {
    const auto fa = raster(a, yaw, el, opt.size, c, diag / 2), fb = raster(b, yaw, el, opt.size, c, diag / 2);
    size_t inter = 0, uni = 0;
    const int nblk = (opt.size + 7) / 8;
    std::vector<std::array<double, 7>> blocks(size_t(nblk) * nblk, std::array<double, 7>{0, 0, 0, 0, 0, 0, 0});  // A lab, B lab, n
    for (size_t i = 0; i < fa.face.size(); ++i) {
      const bool ia = fa.face[i] >= 0, ib = fb.face[i] >= 0;
      inter += ia && ib;
      uni += ia || ib;
      if (!(ia && ib)) continue;
      auto na = face_normal(a, size_t(fa.face[i])), nb = face_normal(b, size_t(fb.face[i]));
      // Two-sided: both normals turned towards the camera.
      auto towards = [&](std::array<double, 3>& n) {
        if (n[0] * fa.fwd[0] + n[1] * fa.fwd[1] + n[2] * fa.fwd[2] > 0)
          for (auto& x : n) x = -x;
      };
      towards(na);
      towards(nb);
      nsum += std::acos(std::clamp(na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2], -1.0, 1.0)) * 180 / kPi;
      ++ncount;
      if (colour) {
        uint8_t ca[4], cb[4];
        if (sample(a, fa.face[i], fa.bary[i], ca) && sample(b, fb.face[i], fb.bary[i], cb)) {
          const auto la = oklab_srgb8(ca), lb = oklab_srgb8(cb);
          auto& blk = blocks[size_t((i / size_t(opt.size)) / 8) * nblk + (i % size_t(opt.size)) / 8];
          for (int k = 0; k < 3; ++k) {
            blk[size_t(k)] += la[size_t(k)];
            blk[size_t(3 + k)] += lb[size_t(k)];
          }
          blk[6] += 1;
          des.push_back(std::sqrt(double(la[0] - lb[0]) * (la[0] - lb[0]) + double(la[1] - lb[1]) * (la[1] - lb[1]) + double(la[2] - lb[2]) * (la[2] - lb[2])));
        }
      }
    }
    r.silhouette_iou.push_back(uni ? double(inter) / double(uni) : 1.0);
    for (const auto& b : blocks) {
      if (b[6] < 16) continue;
      double s2 = 0;
      for (int k = 0; k < 3; ++k) s2 += (b[size_t(k)] - b[size_t(3 + k)]) * (b[size_t(k)] - b[size_t(3 + k)]) / (b[6] * b[6]);
      block_des.push_back(std::sqrt(s2));
    }
  }
  r.silhouette_iou_min = *std::min_element(r.silhouette_iou.begin(), r.silhouette_iou.end());
  r.silhouette_iou_mean = std::accumulate(r.silhouette_iou.begin(), r.silhouette_iou.end(), 0.0) / double(r.silhouette_iou.size());
  r.normal_mean_deg = ncount ? nsum / double(ncount) : 0;
  if (colour && !des.empty()) {
    r.colour_mean_de = std::accumulate(des.begin(), des.end(), 0.0) / double(des.size());
    std::sort(des.begin(), des.end());
    r.colour_p95_de = des[std::min(des.size() - 1, size_t(0.95 * double(des.size())))];
  }
  if (colour && !block_des.empty()) {
    r.colour_block_mean_de = std::accumulate(block_des.begin(), block_des.end(), 0.0) / double(block_des.size());
    std::sort(block_des.begin(), block_des.end());
    r.colour_block_p95_de = block_des[std::min(block_des.size() - 1, size_t(0.95 * double(block_des.size())))];
  }
  r.original = topology(a);
  r.reduced = topology(b);
  return r;
}

json judge_fidelity(const Fidelity& f, const FidelityThresholds& t) {
  json reasons = json::array();
  auto over = [&](bool bad, const std::string& what) {
    if (bad) reasons.push_back(what);
  };
  char buf[200];
  std::snprintf(buf, sizeof buf, "surface distance p95 %.3f%% > %.3f%% of the diagonal", f.distance_p95_pct, t.max_distance_p95_pct);
  over(f.distance_p95_pct > t.max_distance_p95_pct, buf);
  std::snprintf(buf, sizeof buf, "surface distance mean %.3f%% > %.3f%%", f.distance_mean_pct, t.max_distance_mean_pct);
  over(f.distance_mean_pct > t.max_distance_mean_pct, buf);
  std::snprintf(buf, sizeof buf, "surface distance max %.3f%% > %.3f%% (a local spike or lost part)", f.distance_max_pct, t.max_distance_max_pct);
  over(f.distance_max_pct > t.max_distance_max_pct, buf);
  std::snprintf(buf, sizeof buf, "silhouette IoU %.4f < %.4f (worst view)", f.silhouette_iou_min, t.min_silhouette_iou);
  over(f.silhouette_iou_min < t.min_silhouette_iou, buf);
  std::snprintf(buf, sizeof buf, "normal difference %.2f deg > %.2f deg", f.normal_mean_deg, t.max_normal_mean_deg);
  over(f.normal_mean_deg > t.max_normal_mean_deg, buf);
  if (f.colour_block_mean_de >= 0) {
    std::snprintf(buf, sizeof buf, "colour difference (8x8 block means) dE %.4f > %.4f", f.colour_block_mean_de, t.max_colour_mean_de);
    over(f.colour_block_mean_de > t.max_colour_mean_de, buf);
  }
  const auto& o = f.original;
  const auto& r = f.reduced;
  auto added = [&](int64_t before, int64_t after, const char* what) {
    if (after > before) reasons.push_back(std::to_string(after - before) + " new " + what);
  };
  added(o.boundary_edges, r.boundary_edges, "boundary edges");
  added(o.boundary_loops, r.boundary_loops, "holes (boundary loops)");
  added(o.overfull_edges, r.overfull_edges, "overfull (non-manifold) edges");
  added(o.inconsistent_winding_edges, r.inconsistent_winding_edges, "edges with inconsistent winding");
  added(o.degenerate_triangles, r.degenerate_triangles, "degenerate triangles");
  return {{"status", reasons.empty() ? "PASS" : "FAIL"}, {"reasons", reasons},
          {"thresholds", {{"max_distance_p95_pct", t.max_distance_p95_pct}, {"max_distance_mean_pct", t.max_distance_mean_pct},
                          {"max_distance_max_pct", t.max_distance_max_pct},
                          {"min_silhouette_iou", t.min_silhouette_iou}, {"max_normal_mean_deg", t.max_normal_mean_deg},
                          {"max_colour_mean_de", t.max_colour_mean_de}}}};
}

}  // namespace p3dagent
