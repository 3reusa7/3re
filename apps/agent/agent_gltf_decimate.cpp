#include "agent_gltf_decimate.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <vector>

#include "agent_fidelity.hpp"
#include "meshoptimizer.h"

namespace p3dagent {
namespace {

using nlohmann::json;

struct Accessor {
  const uint8_t* data = nullptr;
  size_t count = 0, stride = 0;
  int comp = 0, ncomp = 0;
  bool normalized = false;
};
Accessor view_of(const Glb& g, int index) {
  const auto& a = g.json.at("accessors").at(size_t(index));
  if (!a.contains("bufferView") || a.contains("sparse")) throw std::runtime_error("sparse or empty accessor");
  const auto& bv = g.json.at("bufferViews").at(a["bufferView"].get<size_t>());
  if (bv.value("buffer", 0) != 0) throw std::runtime_error("accessor outside the GLB binary chunk");
  static const std::map<std::string, int> kN{{"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}};
  Accessor v;
  v.comp = a.at("componentType");
  v.ncomp = kN.at(a.at("type").get<std::string>());
  v.count = a.at("count");
  v.normalized = a.value("normalized", false);
  const int cs = v.comp == 5126 || v.comp == 5125 ? 4 : v.comp == 5123 || v.comp == 5122 ? 2 : 1;
  v.stride = bv.value("byteStride", size_t(cs * v.ncomp));
  const size_t bv_off = bv.value("byteOffset", size_t(0)), bv_len = bv.at("byteLength").get<size_t>();
  size_t off = 0, bv_end = 0;
  if (__builtin_add_overflow(bv_off, a.value("byteOffset", size_t(0)), &off) || __builtin_add_overflow(bv_off, bv_len, &bv_end) ||
      v.stride == 0 || !span_ok(off, v.count, v.stride, size_t(cs * v.ncomp), std::min(g.bin.size(), bv_end)))
    throw std::runtime_error("accessor out of range");
  v.data = g.bin.data() + off;
  return v;
}
float comp_f(const Accessor& v, size_t i, int k) {
  const uint8_t* p = v.data + i * v.stride;
  switch (v.comp) {
    case 5126: { float f; std::memcpy(&f, p + 4 * k, 4); return f; }
    case 5123: { uint16_t u; std::memcpy(&u, p + 2 * k, 2); return v.normalized ? u / 65535.f : float(u); }
    case 5121: return v.normalized ? p[k] / 255.f : float(p[k]);
    case 5122: { int16_t s; std::memcpy(&s, p + 2 * k, 2); return v.normalized ? std::max(s / 32767.f, -1.f) : float(s); }
    case 5120: return v.normalized ? std::max(int8_t(p[k]) / 127.f, -1.f) : float(int8_t(p[k]));
    case 5125: { uint32_t u; std::memcpy(&u, p + 4 * k, 4); return float(u); }
  }
  throw std::runtime_error("unsupported component type");
}
uint32_t index_at(const Accessor& v, size_t i) {
  const uint8_t* p = v.data + i * v.stride;
  if (v.comp == 5125) { uint32_t u; std::memcpy(&u, p, 4); return u; }
  if (v.comp == 5123) { uint16_t u; std::memcpy(&u, p, 2); return u; }
  if (v.comp == 5121) return p[0];
  throw std::runtime_error("unsupported index type");
}

}  // namespace

GltfDecimateResult decimate_gltf(const Glb& in, int64_t target) {
  GltfDecimateResult r;
  r.glb = in;
  auto& j = r.glb.json;
  if (!j.contains("meshes")) throw std::runtime_error("the GLB has no meshes");
  // Instances and world area per mesh (through the node hierarchy, via the fidelity loader of one mesh at a time
  // would be costly: area per mesh in local space x the squared mean scale of each instance).
  const size_t nm = j["meshes"].size();
  std::vector<int> instances(nm, 0);  // nodes of the default scene drawing each mesh (counted in the walk below)
  // World area per mesh: the whole file in world space, triangles attributed to their mesh in loader order.
  const FMesh world = fmesh_from_glb(in);
  std::vector<double> mesh_area(nm, 0);
  std::vector<int64_t> mesh_tris(nm, 0);
  {
    // fmesh_from_glb visits nodes depth-first from the scene roots and appends each mesh's triangles; repeat the
    // same walk to know which mesh each run of triangles came from.
    std::vector<int> order;
    std::vector<uint8_t> seen(j.at("nodes").size(), 0);  // fmesh_from_glb (above) refuses cycles and shared nodes
    std::function<void(int)> walk = [&](int ni) {
      if (ni < 0 || size_t(ni) >= seen.size() || seen[size_t(ni)]) return;
      seen[size_t(ni)] = 1;
      const auto& node = j.at("nodes").at(size_t(ni));
      if (node.contains("mesh")) {
        order.push_back(node["mesh"].get<int>());
        ++instances.at(node["mesh"].get<size_t>());
      }
      for (const auto& c : node.value("children", json::array())) walk(c.get<int>());
    };
    const int scene = j.value("scene", 0);
    for (const auto& n : j.at("scenes").at(size_t(scene)).value("nodes", json::array())) walk(n.get<int>());
    size_t t = 0;
    for (int mi : order)
      for (const auto& prim : j.at("meshes").at(size_t(mi)).value("primitives", json::array())) {
        if (prim.value("mode", 4) != 4) continue;
        const size_t cnt = prim.contains("indices") ? j.at("accessors").at(prim["indices"].get<size_t>()).at("count").get<size_t>() / 3
                                                    : j.at("accessors").at(prim.at("attributes").at("POSITION").get<size_t>()).at("count").get<size_t>() / 3;
        for (size_t k = 0; k < cnt && t < world.tri.size(); ++k, ++t) {
          const auto& a = world.pos[world.tri[t][0]];
          const auto& b = world.pos[world.tri[t][1]];
          const auto& c = world.pos[world.tri[t][2]];
          const double u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
          const double x = u[1] * v[2] - u[2] * v[1], y = u[2] * v[0] - u[0] * v[2], z = u[0] * v[1] - u[1] * v[0];
          mesh_area[size_t(mi)] += 0.5 * std::sqrt(x * x + y * y + z * z);
          ++mesh_tris[size_t(mi)];
        }
      }
  }
  r.triangles_before = 0;
  double total_area = 0;
  for (size_t m = 0; m < nm; ++m) {
    r.triangles_before += mesh_tris[m];
    total_area += mesh_area[m];
  }
  r.report = {{"tool", "3re agent glTF decimation (meshoptimizer 0.24 simplifyWithAttributes)"}, {"target", target},
              {"triangles_before", r.triangles_before}, {"meshes", json::array()}};
  if (r.triangles_before <= target || total_area <= 0) {
    r.triangles_after = r.triangles_before;
    r.budget_met = r.triangles_before <= target;
    r.report["triangles_after"] = r.triangles_after;
    return r;
  }
  for (int pass = 0; pass < 2; ++pass) {
    const bool lock_border = pass == 0;
    r.glb = in;  // each pass starts from the original
    int64_t after = 0;
    json meshes = json::array();
    for (size_t m = 0; m < nm; ++m) {
      if (instances[m] == 0 || mesh_tris[m] == 0) continue;  // not drawn by the default scene: left as it is
      // Share of the budget by world area; a mesh never goes up and keeps at least 12 triangles per instance.
      const double share = double(target) * mesh_area[m] / total_area;
      const int inst = std::max(1, instances[m]);
      const int64_t per_mesh_now = mesh_tris[m] / inst;
      const int64_t want = std::clamp<int64_t>(int64_t(share / inst), std::min<int64_t>(12, per_mesh_now), per_mesh_now);
      int64_t mesh_after = 0;
      auto& prims = j["meshes"][m]["primitives"];
      // Prims of this mesh share its target by their own triangle counts.
      int64_t mesh_now = 0;
      for (auto& prim : prims)
        if (prim.value("mode", 4) == 4)
          mesh_now += prim.contains("indices") ? j.at("accessors").at(prim["indices"].get<size_t>()).at("count").get<int64_t>() / 3
                                               : j.at("accessors").at(prim.at("attributes").at("POSITION").get<size_t>()).at("count").get<int64_t>() / 3;
      for (auto& prim : prims) {
        if (prim.value("mode", 4) != 4) continue;
        const auto pos = view_of(r.glb, prim["attributes"]["POSITION"].get<int>());
        if (pos.comp != 5126 || pos.ncomp != 3) throw std::runtime_error("POSITION must be float VEC3");
        std::vector<unsigned int> idx;
        if (prim.contains("indices")) {
          const auto iv = view_of(r.glb, prim["indices"].get<int>());
          idx.resize(iv.count);
          for (size_t i = 0; i < iv.count; ++i) {
            idx[i] = index_at(iv, i);
            if (idx[i] >= pos.count) throw std::runtime_error("index out of range");
          }
          idx.resize(idx.size() - idx.size() % 3);
        } else {
          idx.resize(pos.count - pos.count % 3);
          for (size_t i = 0; i < idx.size(); ++i) idx[i] = unsigned(i);
        }
        const int64_t now = int64_t(idx.size() / 3);
        const int64_t t = mesh_now > 0 ? std::max<int64_t>(4, want * now / mesh_now) : now;
        std::vector<float> P(pos.count * 3), A;
        for (size_t i = 0; i < pos.count; ++i)
          for (int k = 0; k < 3; ++k) P[i * 3 + size_t(k)] = comp_f(pos, i, k);
        // Attributes: UV (weight 1) and normal (weight 0.5) when present: their seams are kept.
        std::vector<float> weights;
        const auto attrs = prim["attributes"];
        std::vector<Accessor> av;
        auto attr = [&](const char* key, int ncomp, std::initializer_list<float> w) {
          if (!attrs.contains(key)) return;
          const auto a = view_of(r.glb, attrs[key].get<int>());
          if (a.ncomp != ncomp || a.count != pos.count) throw std::runtime_error(std::string(key) + " has the wrong type or count");
          av.push_back(a);
          weights.insert(weights.end(), w);
        };
        attr("TEXCOORD_0", 2, {1.f, 1.f});
        attr("NORMAL", 3, {0.5f, 0.5f, 0.5f});
        const size_t na = weights.size();
        if (na) {
          A.resize(pos.count * na);
          for (size_t i = 0; i < pos.count; ++i) {
            size_t o = 0;
            for (const auto& a : av)
              for (int k = 0; k < a.ncomp; ++k) A[i * na + o++] = comp_f(a, i, k);
          }
        }
        // Vertices on edges that are already non-manifold or flipped (by exact position, within this primitive) are
        // locked: collapsing next to them is what would add new non-manifold edges to a file that already has some.
        std::vector<unsigned char> lock(pos.count, 0);
        {
          std::map<std::array<float, 3>, uint32_t> weld;
          std::vector<uint32_t> id(pos.count);
          for (size_t i = 0; i < pos.count; ++i) id[i] = weld.emplace(std::array<float, 3>{P[i * 3], P[i * 3 + 1], P[i * 3 + 2]}, uint32_t(weld.size())).first->second;
          std::map<std::pair<uint32_t, uint32_t>, std::array<int, 2>> uses;  // {count, count in min->max direction}
          for (size_t f = 0; f + 2 < idx.size(); f += 3)
            for (int k = 0; k < 3; ++k) {
              const uint32_t a = id[idx[f + size_t(k)]], b = id[idx[f + size_t((k + 1) % 3)]];
              auto& u = uses[{std::min(a, b), std::max(a, b)}];
              ++u[0];
              if (a < b) ++u[1];
            }
          std::vector<unsigned char> bad(weld.size(), 0);
          for (const auto& [e, u] : uses)
            if (u[0] > 2 || (u[0] == 2 && u[1] != 1)) bad[e.first] = bad[e.second] = 1;
          for (size_t i = 0; i < pos.count; ++i) lock[i] = bad[id[i]];
        }
        size_t locked = 0;
        for (auto l : lock) locked += l;
        std::vector<unsigned int> out(idx.size());
        float err = 0;
        const unsigned opts = lock_border ? meshopt_SimplifyLockBorder : 0;
        const float* attrs_ptr = na ? A.data() : P.data();  // with no attributes, the call reads none (count 0)
        const size_t n = meshopt_simplifyWithAttributes(out.data(), idx.data(), idx.size(), P.data(), pos.count, 12, attrs_ptr, na ? na * 4 : 12,
                                                        na ? weights.data() : nullptr, na, lock.data(), size_t(t * 3), 0.05f, opts, &err);
        out.resize(n);
        if (n == 0) {  // never an empty primitive: this one keeps its indices
          mesh_after += now;
          meshes.push_back({{"mesh", m}, {"instances", inst}, {"triangles_before", now}, {"target", t}, {"triangles_after", now}, {"kept", true}});
          continue;
        }
        // New index buffer (uint32) appended to the binary chunk; vertices and everything else unchanged. The old
        // index data stays in the binary chunk (unused), so the file is not smaller than the input on disk.
        while (r.glb.bin.size() % 4) r.glb.bin.push_back(0);
        const size_t off = r.glb.bin.size();
        r.glb.bin.resize(off + n * 4);
        if (n) std::memcpy(r.glb.bin.data() + off, out.data(), n * 4);
        j["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", off}, {"byteLength", n * 4}, {"target", 34963}});
        j["accessors"].push_back({{"bufferView", j["bufferViews"].size() - 1}, {"componentType", 5125}, {"count", n}, {"type", "SCALAR"}});
        prim["indices"] = j["accessors"].size() - 1;
        mesh_after += int64_t(n / 3);
        meshes.push_back({{"mesh", m}, {"instances", inst}, {"triangles_before", now}, {"target", t}, {"triangles_after", int64_t(n / 3)},
                          {"relative_error", err}, {"vertices_locked_on_non_manifold_edges", locked}});
      }
      after += mesh_after * inst;
    }
    j["buffers"][0]["byteLength"] = r.glb.bin.size();
    r.triangles_after = after;
    r.budget_met = after <= target;
    r.report["meshes"] = meshes;
    r.report["border_locked"] = lock_border;
    r.report["triangles_after"] = after;
    if (r.budget_met) break;  // the border-locked pass is preferred (keeps open sheets' outlines)
  }
  r.report["budget_met"] = r.budget_met;
  return r;
}

}  // namespace p3dagent
