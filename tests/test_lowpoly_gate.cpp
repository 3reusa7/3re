#include <doctest/doctest.h>

#include <algorithm>

#include "lowpoly_helpers.hpp"
#include "pixel3d/lowpoly/gate.hpp"
#include "pixel3d/lowpoly/layout.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;
using namespace p3dtest;

namespace {

struct GateRun {
  Input in;
  Result r;
  SkinnedCheck exported;
  LowpolyValidation v;
};

GateRun run(const std::string& variant, int budget, bool side, double depth = 0, bool do_export = true) {
  GateRun x;
  x.in = load_lowpoly_fixture(variant, side, side ? 0 : (depth > 0 ? depth : 20));
  if (!side && depth <= 0) {
    x.in.settings.user_depth = false;
    x.in.settings.depth = 0;
  }
  x.in.settings.tri_budget = budget;
  auto r = build_lowpoly(x.in);
  REQUIRE(r);
  x.r = std::move(*r);
  SkinnedExportOptions o;
  const auto glb = write_skinned_glb(x.r.mesh(), x.r.layout.skeleton, *x.r.bake.texture, o);
  x.exported = check_skinned_glb(glb, x.r.mesh(), x.r.layout.skeleton, o);
  x.v = validate_lowpoly(x.in, x.r, do_export ? &x.exported : nullptr);
  return x;
}

const Check* find_check(const LowpolyValidation& v, const std::string& name) {
  for (const auto& c : v.checks)
    if (c.name == name) return &c;
  return nullptr;
}

bool has_conflict(const LowpolyValidation& v, const std::string& kind, bool blocking) {
  return std::any_of(v.conflicts.begin(), v.conflicts.end(),
                     [&](const LowpolyConflict& c) { return c.kind == kind && c.blocking == blocking; });
}

}  // namespace

TEST_CASE("lowpoly gate: front + side characters pass every critical check at 500, 1000 and 1500") {
  for (const char* variant : {"cloaked", "humanoid"})
    for (int budget : {500, 1000, 1500}) {
      CAPTURE(variant);
      CAPTURE(budget);
      const GateRun x = run(variant, budget, true);
      INFO(to_json(x.v).dump(1));
      for (const auto& c : x.v.checks)
        if (c.critical) CHECK(c.passed);
      CHECK(x.v.status != ValidationStatus::Fail);
      CHECK(find_check(x.v, "lowpoly.silhouette_iou_front"));
      CHECK(find_check(x.v, "lowpoly.silhouette_iou_side"));
      CHECK(has_conflict(x.v, "lowpoly.overlapping_shells", false));
      CHECK_FALSE(has_conflict(x.v, "lowpoly.inferred_depth", true));
    }
}

TEST_CASE("lowpoly gate: front only stays NEEDS_REVIEW until the user sets a depth") {
  const GateRun none = run("cloaked", 1000, false);
  CHECK(none.v.status == ValidationStatus::NeedsReview);
  CHECK(has_conflict(none.v, "lowpoly.inferred_depth", true));
  const GateRun set = run("cloaked", 1000, false, 22);
  CHECK(has_conflict(set.v, "lowpoly.inferred_depth", false));
  CHECK_FALSE(has_conflict(set.v, "lowpoly.inferred_depth", true));
  CHECK(set.v.status != ValidationStatus::Fail);
}

TEST_CASE("lowpoly gate: no round trip, no PASS; a broken budget is a FAIL") {
  const GateRun x = run("humanoid", 1000, true, 0, /*do_export=*/false);
  CHECK(x.v.status != ValidationStatus::Pass);
  GateRun y = run("humanoid", 1000, true);
  Input tight = y.in;
  tight.settings.tri_budget = 600;  // the mesh was built for 1000
  const LowpolyValidation v = validate_lowpoly(tight, y.r, &y.exported);
  CHECK(v.status == ValidationStatus::Fail);
  const Check* c = find_check(v, "lowpoly.tri_budget");
  REQUIRE(c);
  CHECK_FALSE(c->passed);
}

TEST_CASE("lowpoly gate: the report carries per-part triangles that add up") {
  const GateRun x = run("cloaked", 1000, true);
  int sum = 0;
  for (const auto& p : x.v.report["parts"]) sum += p["triangles"].get<int>();
  CHECK(sum == x.r.triangles);
  CHECK(x.v.report["triangles"].get<int>() <= 1000);
  CHECK(to_json(x.v)["status"].is_string());
}

TEST_CASE("lowpoly depth: front only gets an automatic depth per part type, with no manual depth") {
  Input in = load_lowpoly_fixture("cloaked", false, 0);  // no depth given
  CHECK_FALSE(in.settings.user_depth);
  auto r = build_lowpoly(in);
  REQUIRE(r);
  std::map<std::string, double> ratio;
  for (const auto& p : r->parts) ratio[p.name] = p.depth_ratio;
  // depth/width (median over the part's cross-sections) at least the type minimum.
  CHECK(ratio.at("head") >= 0.9 - 1e-6);
  CHECK(ratio.at("body") >= 0.6 - 1e-6);  // cloth
  CHECK(ratio.at("leg_l") >= 0.95);       // limbs are round
  CHECK(ratio.at("horn_l") >= 0.95);      // horns are cones
  for (const auto& p : r->parts) CHECK(p.depth_source.find("inflation") != std::string::npos);
  // A per-part override wins.
  for (auto& p : in.parts)
    if (p.name == "body") p.depth_ratio = 0.3;
  auto r2 = build_lowpoly(in);
  REQUIRE(r2);
  for (const auto& p : r2->parts)
    if (p.name == "body") CHECK(p.depth_ratio == doctest::Approx(0.3).epsilon(0.15));
}

TEST_CASE("lowpoly depth: flat accessories stay thin, elongated ones are cones") {
  CHECK(type_min_depth_ratio(PartType::Accessory, /*elongated=*/false) <= 0.4);
  CHECK(type_min_depth_ratio(PartType::Accessory, true) >= 0.95);
  CHECK(type_min_depth_ratio(PartType::Head, false) >= 0.9);
  CHECK(type_min_depth_ratio(PartType::Cloth, false) >= 0.6);
  CHECK(type_min_depth_ratio(PartType::Cloth, false) <= 0.8);
  CHECK(type_min_depth_ratio(PartType::Limb, false) == doctest::Approx(1.0));
}

TEST_CASE("lowpoly depth: accept_inferred_depth allows export but keeps NEEDS_REVIEW, and is reported") {
  Input in = load_lowpoly_fixture("cloaked", false, 0);
  auto r = build_lowpoly(in);
  REQUIRE(r);
  SkinnedExportOptions o;
  const auto glb = write_skinned_glb(r->mesh(), r->layout.skeleton, *r->bake.texture, o);
  const SkinnedCheck sc = check_skinned_glb(glb, r->mesh(), r->layout.skeleton, o);
  const LowpolyValidation v = validate_lowpoly(in, *r, &sc);
  CHECK(v.status == ValidationStatus::NeedsReview);
  CHECK_FALSE(export_decision(v, in.settings).allowed);
  in.settings.accept_inferred_depth = true;
  const LowpolyValidation va = validate_lowpoly(in, *r, &sc);
  CHECK(va.status == ValidationStatus::NeedsReview);  // the rules do not change
  const ExportDecision d = export_decision(va, in.settings);
  CHECK(d.allowed);
  CHECK(d.accepted_inferred_depth);
  CHECK(to_json(va)["inferred_depth_accepted"].get<bool>());
  // Acceptance covers inferred depth only: any other blocking conflict still blocks.
  LowpolyValidation other = va;
  other.conflicts.push_back({"lowpoly.silhouette_deviation", true, "test", {}});
  CHECK_FALSE(export_decision(other, in.settings).allowed);
}
