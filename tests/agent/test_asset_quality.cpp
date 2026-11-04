// Tests of the asset-quality steps (docs/agent/ASSET-QUALITY.md). Written before the code.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "agent/agent_fidelity.hpp"
#include "agent/agent_glb.hpp"
#include "agent/agent_hygiene.hpp"

using namespace p3dagent;
using nlohmann::json;
constexpr double M_PI_ = 3.14159265358979323846;

namespace {

// A UV sphere (outward winding) with outward normals, optional tangents and a normal-mapped material.
struct SphereOpts {
  bool normals = true, invert_normals = false, tangents = false, normal_map = false;
  int seg = 12, rings = 6;
};
Glb sphere(const SphereOpts& o = {}) {
  std::vector<float> v, n, t;
  std::vector<uint32_t> f;
  auto add = [&](double x, double y, double z) {
    v.insert(v.end(), {float(x), float(y), float(z)});
    const float s = o.invert_normals ? -1.f : 1.f;
    n.insert(n.end(), {s * float(x), s * float(y), s * float(z)});
    t.insert(t.end(), {1, 0, 0, 1});
  };
  add(0, 1, 0);
  for (int r = 1; r < o.rings; ++r)
    for (int s = 0; s < o.seg; ++s) {
      const double th = M_PI_ * r / o.rings, ph = 2 * M_PI_ * s / o.seg;
      add(std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph));
    }
  add(0, -1, 0);
  const uint32_t south = uint32_t(v.size() / 3 - 1);
  auto ring = [&](int r, int s) { return uint32_t(1 + (r - 1) * o.seg + (s % o.seg)); };
  for (int s = 0; s < o.seg; ++s) f.insert(f.end(), {0, ring(1, s + 1), ring(1, s)});
  for (int r = 1; r < o.rings - 1; ++r)
    for (int s = 0; s < o.seg; ++s) {
      f.insert(f.end(), {ring(r, s), ring(r, s + 1), ring(r + 1, s + 1)});
      f.insert(f.end(), {ring(r, s), ring(r + 1, s + 1), ring(r + 1, s)});
    }
  for (int s = 0; s < o.seg; ++s) f.insert(f.end(), {south, ring(o.rings - 1, s), ring(o.rings - 1, s + 1)});
  const size_t nv = v.size() / 3, vb = v.size() * 4, ib = f.size() * 4, tb = t.size() * 4;
  Glb g;
  g.bin.resize(2 * vb + tb + ib);
  std::memcpy(g.bin.data(), v.data(), vb);
  std::memcpy(g.bin.data() + vb, n.data(), vb);
  std::memcpy(g.bin.data() + 2 * vb, t.data(), tb);
  std::memcpy(g.bin.data() + 2 * vb + tb, f.data(), ib);
  json attrs = {{"POSITION", 0}};
  if (o.normals) attrs["NORMAL"] = 1;
  if (o.tangents) attrs["TANGENT"] = 2;
  json prim = {{"attributes", attrs}, {"indices", 3}, {"mode", 4}};
  g.json = {{"asset", {{"version", "2.0"}}}, {"scene", 0}, {"scenes", json::array({{{"nodes", {0}}}})},
            {"nodes", json::array({{{"mesh", 0}, {"name", "body"}}})},
            {"meshes", json::array({{{"primitives", json::array({prim})}}})},
            {"buffers", json::array({{{"byteLength", g.bin.size()}}})},
            {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", vb}},
                                         {{"buffer", 0}, {"byteOffset", vb}, {"byteLength", vb}},
                                         {{"buffer", 0}, {"byteOffset", 2 * vb}, {"byteLength", tb}},
                                         {{"buffer", 0}, {"byteOffset", 2 * vb + tb}, {"byteLength", ib}}})},
            {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"},
                                        {"min", {-1, -1, -1}}, {"max", {1, 1, 1}}},
                                       {{"bufferView", 1}, {"componentType", 5126}, {"count", nv}, {"type", "VEC3"}},
                                       {{"bufferView", 2}, {"componentType", 5126}, {"count", nv}, {"type", "VEC4"}},
                                       {{"bufferView", 3}, {"componentType", 5125}, {"count", f.size()}, {"type", "SCALAR"}}})}};
  if (o.normal_map) {
    g.json["meshes"][0]["primitives"][0]["material"] = 0;
    g.json["materials"] = json::array({{{"normalTexture", {{"index", 0}}}}});
  }
  return g;
}

double signed_volume(const FMesh& m) {
  double v = 0;
  for (const auto& t : m.tri) {
    const auto &a = m.pos[t[0]], &b = m.pos[t[1]], &c = m.pos[t[2]];
    v += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6;
  }
  return v;
}

}  // namespace

TEST_CASE("G0 audit: a clean sphere passes") {
  const auto a = audit_hygiene(sphere());
  CHECK(a.negative_det.empty());
  CHECK(a.non_uniform.empty());
  CHECK(a.unbaked_scale.empty());
  CHECK(a.normal_checked == a.triangles);
  CHECK(a.normal_disagree == 0);
  CHECK(judge_hygiene(a, 0.02).status == "PASS");
}

TEST_CASE("G0 audit: a negative node scale (mirror) is a FAIL") {
  auto g = sphere();
  g.json["nodes"][0]["scale"] = {-1, 1, 1};
  const auto a = audit_hygiene(g);
  CHECK(a.negative_det.size() == 1);
  // glTF draws a mirrored node with reversed winding, so its normals still agree: only the transform fails.
  CHECK(a.normal_disagree == 0);
  const auto v = judge_hygiene(a, 0.02);
  CHECK(v.status == "FAIL");
}

TEST_CASE("G0 audit: a mirror hidden in a parent or a matrix is found") {
  auto g = sphere();
  g.json["nodes"].push_back({{"name", "root"}, {"matrix", {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1}}, {"children", {0}}});
  g.json["scenes"][0]["nodes"] = {1};
  const auto a = audit_hygiene(g);
  CHECK(a.negative_det.size() >= 1);
  CHECK(judge_hygiene(a, 0.02).status == "FAIL");
}

TEST_CASE("G0 audit: non-uniform scale is a FAIL, a uniform unbaked scale is REVIEW") {
  auto g = sphere();
  g.json["nodes"][0]["scale"] = {1, 2, 1};
  CHECK(audit_hygiene(g).non_uniform.size() == 1);
  CHECK(judge_hygiene(audit_hygiene(g), 0.02).status == "FAIL");
  g.json["nodes"][0]["scale"] = {2, 2, 2};
  const auto a = audit_hygiene(g);
  CHECK(a.non_uniform.empty());
  CHECK(a.unbaked_scale.size() == 1);
  CHECK(judge_hygiene(a, 0.02).status == "REVIEW");
}

TEST_CASE("G0 audit: normals that oppose the winding are counted and fail above the threshold") {
  const auto a = audit_hygiene(sphere({.invert_normals = true}));
  CHECK(a.normal_disagree == a.triangles);
  CHECK(a.normal_disagree_fraction() == doctest::Approx(1.0));
  CHECK(judge_hygiene(a, 0.02).status == "FAIL");
}

TEST_CASE("G0 audit: a normal-mapped primitive needs tangents with w = +-1") {
  auto a = audit_hygiene(sphere({.normal_map = true}));
  CHECK(a.normal_mapped_primitives == 1);
  CHECK(a.missing_tangents == 1);
  CHECK(judge_hygiene(a, 0.02).status == "FAIL");
  a = audit_hygiene(sphere({.tangents = true, .normal_map = true}));
  CHECK(a.missing_tangents == 0);
  CHECK(a.bad_tangent_w == 0);
  CHECK(judge_hygiene(a, 0.02).status == "PASS");
}

TEST_CASE("G0 audit: rigid part pivots pass, a scaled part pivot is flagged") {
  auto g = sphere();
  // root (identity) -> part with rotation + translation: a pivot, rigid.
  g.json["nodes"].push_back({{"name", "root"}, {"children", {0}}});
  g.json["scenes"][0]["nodes"] = {1};
  g.json["nodes"][0]["translation"] = {0.5, 1, 0};
  g.json["nodes"][0]["rotation"] = {0, 0.7071068, 0, 0.7071068};
  auto a = audit_hygiene(g);
  CHECK(a.non_rigid_pivots.empty());
  CHECK(judge_hygiene(a, 0.02).status == "PASS");
  g.json["nodes"][0]["scale"] = {0.5, 0.5, 0.5};
  a = audit_hygiene(g);
  CHECK(a.non_rigid_pivots.size() == 1);
  CHECK(judge_hygiene(a, 0.02).status != "PASS");
}

TEST_CASE("G0 bake: a mirrored, scaled hierarchy becomes identity nodes with the same world triangles") {
  auto g = sphere({.tangents = true, .normal_map = true});
  g.json["nodes"][0]["scale"] = {-2, 1, 3};
  g.json["nodes"][0]["translation"] = {1, 0, 0};
  g.json["nodes"].push_back({{"name", "root"}, {"rotation", {0, 0.3826834, 0, 0.9238795}}, {"children", {0}}});
  g.json["scenes"][0]["nodes"] = {1};
  const auto before = fmesh_from_glb(g);
  std::string why;
  const auto baked = bake_node_transforms(g, &why);
  REQUIRE_MESSAGE(baked.has_value(), why);
  const auto a = audit_hygiene(*baked);
  CHECK(a.negative_det.empty());
  CHECK(a.non_uniform.empty());
  CHECK(a.unbaked_scale.empty());
  CHECK(a.normal_disagree == 0);
  CHECK(a.missing_tangents == 0);
  CHECK(a.bad_tangent_w == 0);
  CHECK(judge_hygiene(a, 0.02).status == "PASS");
  for (const auto& n : baked->json["nodes"])
    for (const char* k : {"matrix", "translation", "rotation", "scale"}) CHECK_FALSE(n.contains(k));
  const auto after = fmesh_from_glb(*baked);
  REQUIRE(after.pos.size() == before.pos.size());
  REQUIRE(after.tri.size() == before.tri.size());
  for (size_t i = 0; i < before.pos.size(); ++i)
    for (int k = 0; k < 3; ++k) CHECK(after.pos[i][size_t(k)] == doctest::Approx(before.pos[i][size_t(k)]).epsilon(1e-5));
  // Same orientation: outward volume, positive and equal.
  CHECK(signed_volume(after) > 0);
  CHECK(signed_volume(after) == doctest::Approx(signed_volume(before)).epsilon(1e-4));
  // The file round-trips through the GLB encoder.
  const auto again = parse_glb(encode_glb(*baked));
  CHECK(audit_hygiene(again).triangles == a.triangles);
  // POSITION min/max are rewritten for the new vertices (the exact world box of the old vertices).
  const auto info = inspect_glb(*baked);
  float lo = 1e30f, hi = -1e30f;
  for (const auto& p : before.pos) lo = std::min(lo, p[0]), hi = std::max(hi, p[0]);
  CHECK(info.size()[0] == doctest::Approx(double(hi - lo)).epsilon(1e-4));
}

TEST_CASE("G0 bake: refuses what it cannot bake exactly") {
  auto g = sphere();
  g.json["nodes"][0]["skin"] = 0;
  g.json["skins"] = json::array({{{"joints", {0}}}});
  std::string why;
  CHECK_FALSE(bake_node_transforms(g, &why).has_value());
  CHECK(why.find("skin") != std::string::npos);
  g = sphere();
  g.json["nodes"].push_back({{"mesh", 0}, {"translation", {2, 0, 0}}});  // the same mesh drawn twice, moved
  g.json["scenes"][0]["nodes"] = {0, 1};
  CHECK_FALSE(bake_node_transforms(g, &why).has_value());
  CHECK(why.find("two nodes") != std::string::npos);
}

TEST_CASE("G0 bake: a clean file is returned unchanged") {
  const auto g = sphere();
  const auto baked = bake_node_transforms(g);
  REQUIRE(baked.has_value());
  CHECK(baked->json == g.json);
  CHECK(baked->bin == g.bin);
}
