#include "agent_hygiene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>

namespace p3dagent {
namespace {

using nlohmann::json;
using Mat4 = std::array<double, 16>;  // column-major (glTF)

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int c = 0; c < 4; ++c)
    for (int r0 = 0; r0 < 4; ++r0) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[size_t(k * 4 + r0)] * b[size_t(c * 4 + k)];
      r[size_t(c * 4 + r0)] = s;
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
    for (int i = 0; i < 3; ++i) t[size_t(12 + i)] = n["translation"].at(size_t(i)).get<double>();
  if (n.contains("rotation")) {
    const double x = n["rotation"].at(0), y = n["rotation"].at(1), z = n["rotation"].at(2), w = n["rotation"].at(3);
    r = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 0, 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 0,
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y), 0, 0, 0, 0, 1};
  }
  if (n.contains("scale"))
    for (int i = 0; i < 3; ++i) s[size_t(i * 5)] = n["scale"].at(size_t(i)).get<double>();
  return mul(t, mul(r, s));
}
double det3(const Mat4& m) {
  return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
}
// Column lengths of the 3x3 part: the axis scales.
std::array<double, 3> axis_scales(const Mat4& m) {
  std::array<double, 3> s;
  for (int c = 0; c < 3; ++c) s[size_t(c)] = std::sqrt(m[size_t(c * 4)] * m[size_t(c * 4)] + m[size_t(c * 4 + 1)] * m[size_t(c * 4 + 1)] + m[size_t(c * 4 + 2)] * m[size_t(c * 4 + 2)]);
  return s;
}
// Columns orthogonal (a shear is non-uniform too).
bool orthogonal(const Mat4& m, double tol) {
  for (int a = 0; a < 3; ++a)
    for (int b = a + 1; b < 3; ++b) {
      double d = 0;
      for (int k = 0; k < 3; ++k) d += m[size_t(a * 4 + k)] * m[size_t(b * 4 + k)];
      const auto s = axis_scales(m);
      if (std::abs(d) > tol * s[size_t(a)] * s[size_t(b)]) return false;
    }
  return true;
}
constexpr double kTol = 1e-4;

std::string node_label(const json& n, int i) { return n.value("name", std::string("node ")) + (n.contains("name") ? "" : std::to_string(i)); }

struct View {
  const uint8_t* data = nullptr;
  size_t count = 0, stride = 0;
  int comp = 0, ncomp = 0;
};
View view_of(const Glb& g, int index) {
  const auto& a = g.json.at("accessors").at(size_t(index));
  if (!a.contains("bufferView")) throw std::runtime_error("sparse or empty accessor");
  const auto& bv = g.json.at("bufferViews").at(a["bufferView"].get<size_t>());
  if (bv.value("buffer", 0) != 0) throw std::runtime_error("accessor outside the GLB binary chunk");
  static const std::map<std::string, int> kN{{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}};
  View v;
  v.comp = a.at("componentType");
  v.ncomp = kN.at(a.at("type").get<std::string>());
  v.count = a.at("count");
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
float f_at(const View& v, size_t i, int k) {
  float f;
  std::memcpy(&f, v.data + i * v.stride + 4 * size_t(k), 4);
  return f;
}
uint32_t idx_at(const View& v, size_t i) {
  const uint8_t* p = v.data + i * v.stride;
  if (v.comp == 5125) { uint32_t u; std::memcpy(&u, p, 4); return u; }
  if (v.comp == 5123) { uint16_t u; std::memcpy(&u, p, 2); return u; }
  if (v.comp == 5121) return p[0];
  throw std::runtime_error("unsupported index component type");
}

// Depth-first over the default scene with world matrices; each node once, at most 64 levels.
void walk(const json& j, const std::function<void(int, const json&, const Mat4&, int)>& fn) {
  const auto& nodes = j.value("nodes", json::array());
  std::vector<uint8_t> seen(nodes.size(), 0);
  std::function<void(int, const Mat4&, int)> visit = [&](int ni, const Mat4& parent, int depth) {
    if (ni < 0 || size_t(ni) >= nodes.size() || seen[size_t(ni)] || depth > 64) throw std::runtime_error("invalid or cyclic node hierarchy");
    seen[size_t(ni)] = 1;
    const auto& n = nodes[size_t(ni)];
    const Mat4 w = mul(parent, local_matrix(n));
    fn(ni, n, w, depth);
    for (const auto& c : n.value("children", json::array())) visit(c.get<int>(), w, depth + 1);
  };
  const int scene = j.value("scene", 0);
  const auto scenes = j.value("scenes", json::array());
  if (scene >= 0 && size_t(scene) < scenes.size())
    for (const auto& n : scenes[size_t(scene)].value("nodes", json::array())) visit(n.get<int>(), identity(), 0);
}

}  // namespace

json HygieneAudit::to_json() const {
  return {{"nodes", nodes}, {"negative_determinant", negative_det}, {"non_uniform_scale", non_uniform}, {"unbaked_scale", unbaked_scale},
          {"non_rigid_pivots", non_rigid_pivots}, {"triangles", triangles}, {"normal_checked", normal_checked},
          {"normal_disagree", normal_disagree}, {"normal_disagree_fraction", normal_disagree_fraction()},
          {"normal_mapped_primitives", normal_mapped_primitives}, {"missing_tangents", missing_tangents}, {"bad_tangent_w", bad_tangent_w},
          {"unreadable", unreadable}};
}

HygieneAudit audit_hygiene(const Glb& g) {
  HygieneAudit a;
  const json& j = g.json;
  a.nodes = int(j.value("nodes", json::array()).size());
  walk(j, [&](int ni, const json& n, const Mat4& world, int depth) {
    const Mat4 local = local_matrix(n);
    const auto s = axis_scales(local);
    const double smax = std::max({s[0], s[1], s[2]}), smin = std::min({s[0], s[1], s[2]});
    const std::string label = node_label(n, ni);
    if (det3(world) < 0 || det3(local) < 0) a.negative_det.push_back(label);
    if (smax - smin > kTol * smax || !orthogonal(local, kTol)) a.non_uniform.push_back(label);
    else if (std::abs(smax - 1) > kTol) {
      // A part under a parent must be rigid (G3 pivots); a scaled root is an unbaked delivery transform.
      if (depth > 0 && n.contains("mesh")) a.non_rigid_pivots.push_back(label);
      else a.unbaked_scale.push_back(label);
    }
    if (!n.contains("mesh")) return;
    const bool mirrored = det3(world) < 0;
    // Normal matrix = cofactor of the 3x3 part (M^-T up to the positive factor |det|, with det's sign).
    const double* m = world.data();
    const double sgn = mirrored ? -1.0 : 1.0;
    const std::array<double, 9> cof = {sgn * (m[5] * m[10] - m[6] * m[9]), sgn * (m[6] * m[8] - m[4] * m[10]), sgn * (m[4] * m[9] - m[5] * m[8]),
                                       sgn * (m[2] * m[9] - m[1] * m[10]), sgn * (m[0] * m[10] - m[2] * m[8]), sgn * (m[1] * m[8] - m[0] * m[9]),
                                       sgn * (m[1] * m[6] - m[2] * m[5]), sgn * (m[2] * m[4] - m[0] * m[6]), sgn * (m[0] * m[5] - m[1] * m[4])};
    const auto& meshes = j.at("meshes");
    const auto& prims = meshes.at(n["mesh"].get<size_t>()).value("primitives", json::array());
    for (size_t pi = 0; pi < prims.size(); ++pi) {
      const auto& p = prims[pi];
      const std::string where = label + " primitive " + std::to_string(pi);
      if (p.value("mode", 4) != 4) continue;
      const auto attrs = p.value("attributes", json::object());
      if (p.contains("material")) {
        const auto& mat = j.at("materials").at(p["material"].get<size_t>());
        if (mat.contains("normalTexture")) {
          ++a.normal_mapped_primitives;
          if (!attrs.contains("TANGENT")) ++a.missing_tangents;
        }
      }
      try {
        const auto pv = view_of(g, attrs.at("POSITION").get<int>());
        if (pv.comp != 5126 || pv.ncomp != 3) throw std::runtime_error("POSITION is not float VEC3");
        std::vector<std::array<double, 3>> pos(pv.count), nrm;
        for (size_t i = 0; i < pv.count; ++i) {
          const double x = f_at(pv, i, 0), y = f_at(pv, i, 1), z = f_at(pv, i, 2);
          for (int k = 0; k < 3; ++k) pos[i][size_t(k)] = m[k] * x + m[4 + k] * y + m[8 + k] * z + m[12 + k];
        }
        if (attrs.contains("NORMAL")) {
          const auto nv = view_of(g, attrs["NORMAL"].get<int>());
          if (nv.comp != 5126 || nv.ncomp != 3 || nv.count != pv.count) throw std::runtime_error("NORMAL is not float VEC3 with POSITION's count");
          nrm.resize(nv.count);
          for (size_t i = 0; i < nv.count; ++i) {
            const double x = f_at(nv, i, 0), y = f_at(nv, i, 1), z = f_at(nv, i, 2);
            for (int k = 0; k < 3; ++k) nrm[i][size_t(k)] = cof[size_t(k)] * x + cof[size_t(3 + k)] * y + cof[size_t(6 + k)] * z;
          }
        }
        if (attrs.contains("TANGENT")) {
          const auto tv = view_of(g, attrs["TANGENT"].get<int>());
          if (tv.comp != 5126 || tv.ncomp != 4) ++a.bad_tangent_w;
          else
            for (size_t i = 0; i < tv.count; ++i)
              if (std::abs(std::abs(f_at(tv, i, 3)) - 1.0f) > 1e-3f) {
                ++a.bad_tangent_w;
                break;
              }
        }
        std::vector<uint32_t> idx;
        if (p.contains("indices")) {
          const auto iv = view_of(g, p["indices"].get<int>());
          idx.resize(iv.count);
          for (size_t i = 0; i < iv.count; ++i) idx[i] = idx_at(iv, i);
        } else {
          idx.resize(pv.count);
          for (size_t i = 0; i < pv.count; ++i) idx[i] = uint32_t(i);
        }
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
          uint32_t i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
          if (i0 >= pos.size() || i1 >= pos.size() || i2 >= pos.size()) throw std::runtime_error("index out of range");
          ++a.triangles;
          if (mirrored) std::swap(i1, i2);  // glTF: a mirrored node draws reversed
          if (nrm.empty()) continue;
          const auto &p0 = pos[i0], &p1 = pos[i1], &p2 = pos[i2];
          const std::array<double, 3> e1{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
          const std::array<double, 3> fn{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
          const double area2 = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
          if (!(area2 > 0)) continue;  // degenerate: no orientation to judge
          double dot = 0;
          for (uint32_t vi : {i0, i1, i2})
            for (int k = 0; k < 3; ++k) dot += fn[size_t(k)] * nrm[vi][size_t(k)];
          ++a.normal_checked;
          if (dot < 0) ++a.normal_disagree;
        }
      } catch (const std::exception& e) {
        a.unreadable.push_back(where + ": " + e.what());
      }
    }
  });
  return a;
}

HygieneVerdict judge_hygiene(const HygieneAudit& a, double max_disagree_fraction) {
  HygieneVerdict v{"PASS", {}};
  auto fail = [&](const std::string& r) { v.status = "FAIL"; v.reasons.push_back(r); };
  auto review = [&](const std::string& r) {
    if (v.status == "PASS") v.status = "REVIEW";
    v.reasons.push_back(r);
  };
  auto list = [](const std::vector<std::string>& s) {
    std::string out;
    for (size_t i = 0; i < s.size() && i < 4; ++i) out += (i ? ", " : "") + s[i];
    return s.size() > 4 ? out + ", ..." : out;
  };
  if (!a.negative_det.empty()) fail("mirrored node transform (negative determinant): " + list(a.negative_det));
  if (!a.non_uniform.empty()) fail("non-uniform or sheared node scale: " + list(a.non_uniform));
  if (!a.non_rigid_pivots.empty()) fail("part pivot node is not rigid (scale must be 1): " + list(a.non_rigid_pivots));
  if (a.missing_tangents) fail(std::to_string(a.missing_tangents) + " normal-mapped primitive(s) without TANGENT");
  if (a.bad_tangent_w) fail(std::to_string(a.bad_tangent_w) + " primitive(s) with TANGENT.w other than +-1");
  if (!a.unreadable.empty()) fail("unreadable primitive: " + list(a.unreadable));
  // Over half wound against the normals = an inverted mesh (FAIL); above the measured threshold = normals smoothed
  // across thin parts that shade dark (REVIEW; 971 existing deliverables: p50 1.0 %, p90 3.5 %, max 13 %).
  if (a.normal_disagree_fraction() > 0.5)
    fail(std::to_string(a.normal_disagree) + " of " + std::to_string(a.normal_checked) + " triangles wound against their normals (inverted)");
  else if (a.normal_disagree_fraction() > max_disagree_fraction)
    review(std::to_string(a.normal_disagree) + " of " + std::to_string(a.normal_checked) + " triangles wound against their normals");
  if (!a.unbaked_scale.empty()) review("delivery scale not baked into the vertices: " + list(a.unbaked_scale));
  return v;
}

std::optional<Glb> bake_node_transforms(const Glb& in, std::string* why) {
  auto refuse = [&](const std::string& r) -> std::optional<Glb> {
    if (why) *why = r;
    return std::nullopt;
  };
  const json& j = in.json;
  if (j.contains("skins") && !j["skins"].empty()) return refuse("the file has a skin: joints carry the transforms");
  if (j.contains("animations") && !j["animations"].empty()) return refuse("the file is animated: node transforms are animated");
  // Collect mesh nodes with their world matrices; nothing to do when every transform is already identity.
  struct Use { int node; Mat4 world; };
  std::vector<Use> uses;
  std::map<int, int> mesh_users;
  bool any = false;
  try {
    walk(j, [&](int ni, const json& n, const Mat4& w, int) {
      for (const char* k : {"matrix", "translation", "rotation", "scale"})
        if (n.contains(k)) any = true;
      if (n.contains("skin")) throw std::runtime_error("skin");
      if (n.contains("mesh")) {
        uses.push_back({ni, w});
        ++mesh_users[n["mesh"].get<int>()];
      }
    });
  } catch (const std::exception& e) {
    return refuse(std::string(e.what()) == "skin" ? "a node has a skin" : e.what());
  }
  if (!any) return in;
  for (const auto& [mi, k] : mesh_users)
    if (k > 1) return refuse("mesh " + std::to_string(mi) + " is drawn by two nodes (instancing cannot be baked per node)");
  Glb g = in;
  json& o = g.json;
  // Every accessor is rewritten at most once (an accessor shared by two primitives with different worlds refuses).
  std::map<int, int> rewritten;  // old accessor -> node that rewrote it
  auto put_floats = [&](const std::vector<float>& f, const std::string& type, int count, bool minmax) {
    while (g.bin.size() % 4) g.bin.push_back(0);
    const size_t off = g.bin.size();
    g.bin.resize(off + f.size() * 4);
    std::memcpy(g.bin.data() + off, f.data(), f.size() * 4);
    o["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", off}, {"byteLength", f.size() * 4}, {"target", 34962}});
    json acc = {{"bufferView", o["bufferViews"].size() - 1}, {"componentType", 5126}, {"count", count}, {"type", type}};
    if (minmax && count > 0) {
      std::array<double, 3> lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
      for (int i = 0; i < count; ++i)
        for (int k = 0; k < 3; ++k) {
          lo[size_t(k)] = std::min(lo[size_t(k)], double(f[size_t(i * 3 + k)]));
          hi[size_t(k)] = std::max(hi[size_t(k)], double(f[size_t(i * 3 + k)]));
        }
      acc["min"] = lo;
      acc["max"] = hi;
    }
    o["accessors"].push_back(acc);
    return int(o["accessors"].size() - 1);
  };
  try {
    for (const auto& u : uses) {
      const json& node = j["nodes"][size_t(u.node)];
      const int mi = node["mesh"].get<int>();
      const double* m = u.world.data();
      const bool mirrored = det3(u.world) < 0;
      const double sgn = mirrored ? -1.0 : 1.0;
      const std::array<double, 9> cof = {sgn * (m[5] * m[10] - m[6] * m[9]), sgn * (m[6] * m[8] - m[4] * m[10]), sgn * (m[4] * m[9] - m[5] * m[8]),
                                         sgn * (m[2] * m[9] - m[1] * m[10]), sgn * (m[0] * m[10] - m[2] * m[8]), sgn * (m[1] * m[8] - m[0] * m[9]),
                                         sgn * (m[1] * m[6] - m[2] * m[5]), sgn * (m[2] * m[4] - m[0] * m[6]), sgn * (m[0] * m[5] - m[1] * m[4])};
      auto& prims = o["meshes"][size_t(mi)]["primitives"];
      for (auto& p : prims) {
        auto& attrs = p["attributes"];
        for (const auto& [name, idx] : attrs.items()) {
          if (name != "POSITION" && name != "NORMAL" && name != "TANGENT") continue;
          const int ai = idx.get<int>();
          if (rewritten.count(ai) && rewritten[ai] != u.node) return refuse("an accessor is shared by two nodes");
        }
        auto map_attr = [&](const char* name, int ncomp, auto&& fn) {
          if (!attrs.contains(name)) return;
          const int ai = attrs[name].get<int>();
          const auto v = view_of(in, ai);
          if (v.comp != 5126 || v.ncomp != ncomp) throw std::runtime_error(std::string(name) + " is not float");
          std::vector<float> out(v.count * size_t(ncomp));
          for (size_t i = 0; i < v.count; ++i) {
            std::array<double, 4> q{0, 0, 0, 0};
            for (int k = 0; k < ncomp; ++k) q[size_t(k)] = f_at(v, i, k);
            const auto r = fn(q);
            for (int k = 0; k < ncomp; ++k) out[i * size_t(ncomp) + size_t(k)] = float(r[size_t(k)]);
          }
          rewritten[ai] = u.node;
          attrs[name] = put_floats(out, ncomp == 3 ? "VEC3" : "VEC4", int(v.count), std::string(name) == "POSITION");
        };
        map_attr("POSITION", 3, [&](const std::array<double, 4>& q) {
          std::array<double, 4> r{};
          for (int k = 0; k < 3; ++k) r[size_t(k)] = m[k] * q[0] + m[4 + k] * q[1] + m[8 + k] * q[2] + m[12 + k];
          return r;
        });
        map_attr("NORMAL", 3, [&](const std::array<double, 4>& q) {
          std::array<double, 4> r{};
          for (int k = 0; k < 3; ++k) r[size_t(k)] = cof[size_t(k)] * q[0] + cof[size_t(3 + k)] * q[1] + cof[size_t(6 + k)] * q[2];
          const double l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
          if (l > 0) for (int k = 0; k < 3; ++k) r[size_t(k)] /= l;
          return r;
        });
        map_attr("TANGENT", 4, [&](const std::array<double, 4>& q) {
          std::array<double, 4> r{};
          for (int k = 0; k < 3; ++k) r[size_t(k)] = m[k] * q[0] + m[4 + k] * q[1] + m[8 + k] * q[2];
          const double l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
          if (l > 0) for (int k = 0; k < 3; ++k) r[size_t(k)] /= l;
          r[3] = q[3] * sgn;  // the bitangent cross(N, T) * w flips with a mirror
          return r;
        });
        if (mirrored && p.value("mode", 4) == 4) {
          std::vector<uint32_t> idx;
          if (p.contains("indices")) {
            const auto iv = view_of(in, p["indices"].get<int>());
            idx.resize(iv.count);
            for (size_t i = 0; i < iv.count; ++i) idx[i] = idx_at(iv, i);
          } else {
            const auto pv = view_of(g, attrs["POSITION"].get<int>());
            idx.resize(pv.count);
            for (size_t i = 0; i < pv.count; ++i) idx[i] = uint32_t(i);
          }
          for (size_t t = 0; t + 2 < idx.size(); t += 3) std::swap(idx[t + 1], idx[t + 2]);
          while (g.bin.size() % 4) g.bin.push_back(0);
          const size_t off = g.bin.size();
          g.bin.resize(off + idx.size() * 4);
          std::memcpy(g.bin.data() + off, idx.data(), idx.size() * 4);
          o["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", off}, {"byteLength", idx.size() * 4}, {"target", 34963}});
          o["accessors"].push_back({{"bufferView", o["bufferViews"].size() - 1}, {"componentType", 5125}, {"count", idx.size()}, {"type", "SCALAR"}});
          p["indices"] = o["accessors"].size() - 1;
        }
      }
    }
  } catch (const std::exception& e) {
    return refuse(e.what());
  }
  // Mesh nodes become identity scene roots; transform-only nodes without a mesh are dropped from the scene.
  json roots = json::array();
  for (const auto& u : uses) {
    auto& n = o["nodes"][size_t(u.node)];
    for (const char* k : {"matrix", "translation", "rotation", "scale", "children"}) n.erase(k);
    roots.push_back(u.node);
  }
  for (auto& n : o["nodes"]) {
    if (!n.contains("mesh")) n.erase("children");
    for (const char* k : {"matrix", "translation", "rotation", "scale"}) n.erase(k);
  }
  o["scenes"][size_t(o.value("scene", 0))]["nodes"] = roots;
  o["buffers"][0]["byteLength"] = g.bin.size();
  return g;
}

}  // namespace p3dagent
