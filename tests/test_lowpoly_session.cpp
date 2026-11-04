#include <doctest/doctest.h>

#include <filesystem>

#include "lowpoly_helpers.hpp"
#include "pixel3d/engine/lowpoly_stage.hpp"
#include "pixel3d/engine/session.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

// LOW-POLY STUDIO through the Session: the one core every client (GUI, CLI, MCP) calls.

namespace {

std::string efix(const std::string& variant, const std::string& role) {
  return p3dtest::lowpoly_fixture_dir(variant) + "/" + role + ".png";
}

fs::path tmp(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / "pixel3d_tests" / "lowpoly_session";
  fs::create_directories(dir);
  return dir / name;
}

ProjectSettings lowpoly_project() {
  ProjectSettings ps;
  ps.name = "lowpoly";
  ps.lowpoly.enabled = true;
  return ps;
}

// A Low-Poly Studio project from fixture E with the suggested parts applied.
void start(Session& s, const std::string& variant, bool side) {
  REQUIRE(s.new_project(lowpoly_project()).success);
  std::vector<ViewImport> views{{ViewRole::Front, efix(variant, "front"), {}}};
  if (side) views.push_back({ViewRole::Side, efix(variant, "side"), {}});
  REQUIRE(s.import_views(views).success);
  const OpResult sg = s.lowpoly_suggest_parts({.apply = true});
  REQUIRE_MESSAGE(sg.success, to_json(sg).dump());
}

}  // namespace

TEST_CASE("lowpoly session: front + side builds a skinned character within the budget") {
  Session s;
  start(s, "cloaked", true);
  CHECK(s.document().part_types.size() == s.document().masks.parts.size());
  const OpResult b = s.lowpoly_build();
  REQUIRE_MESSAGE(b.success, to_json(b).dump());
  const auto& lp = b.data["lowpoly"];
  CHECK(lp["triangles"].get<int>() <= 1000);
  CHECK(lp["budget"] == 1000);
  CHECK(lp["glb"]["ok"] == true);
  CHECK(b.status != ValidationStatus::NotValidated);
  CHECK(b.status == s.document().lowpoly->validation.status);
  CHECK(b.validation["mode"] == "lowpoly_studio");
  CHECK(b.stats.triangles == lp["triangles"].get<int>());
  // Every part has a type and triangles; nothing is reported as measured that was not.
  for (const auto& p : lp["parts"]) {
    CHECK(p["triangles"].get<int>() > 0);
    CHECK_FALSE(p["type"].get<std::string>().empty());
  }
}

TEST_CASE("lowpoly session: the visual-hull stages are refused in a Low-Poly Studio project") {
  Session s;
  start(s, "cloaked", true);
  const OpResult r = s.reconstruct(ReconstructOptions{});
  REQUIRE_FALSE(r.success);
  CHECK(r.errors.front().code == "state.wrong_mode");
  CHECK_FALSE(s.validate(ValidateOptions{}).success);
  CHECK_FALSE(s.export_glb(GlbExportRequest{tmp("x.glb")}).success);
}

TEST_CASE("lowpoly session: a standard project refuses the low-poly build") {
  Session s;
  REQUIRE(s.new_project(ProjectSettings{}).success);
  REQUIRE(s.import_views({{ViewRole::Front, efix("cloaked", "front"), {}}}).success);
  const OpResult r = s.lowpoly_build();
  REQUIRE_FALSE(r.success);
  CHECK(r.errors.front().code == "state.wrong_mode");
}

TEST_CASE("lowpoly session: parts are required, typed, and the suggestion changes nothing until applied") {
  Session s;
  REQUIRE(s.new_project(lowpoly_project()).success);
  REQUIRE(s.import_views({{ViewRole::Front, efix("cloaked", "front"), {}}, {ViewRole::Side, efix("cloaked", "side"), {}}}).success);
  const OpResult none = s.lowpoly_build();
  REQUIRE_FALSE(none.success);
  CHECK(none.errors.front().code == "lowpoly.no_parts");
  const uint64_t rev = s.document().revision;
  const size_t steps = s.history_steps().size();
  const OpResult proposal = s.lowpoly_suggest_parts({});
  REQUIRE(proposal.success);
  CHECK(proposal.data["parts"].size() >= 3);
  CHECK(proposal.data["applied"] == false);
  CHECK(s.document().revision == rev);
  CHECK(s.history_steps().size() == steps);
  CHECK(s.document().masks.parts.empty());
  REQUIRE(s.lowpoly_suggest_parts({.apply = true}).success);
  const int first = s.document().masks.parts.begin()->first;
  // Applying again over existing parts needs replace.
  const OpResult again = s.lowpoly_suggest_parts({.apply = true});
  REQUIRE_FALSE(again.success);
  CHECK(again.errors.front().code == "lowpoly.parts_exist");
  REQUIRE(s.lowpoly_suggest_parts({.apply = true, .replace = true}).success);
  CHECK_FALSE(s.document().masks.parts.contains(first));
  // A user part without a type cannot be built.
  REQUIRE(s.create_part("extra", false).success);
  const int extra = s.document().masks.parts.rbegin()->first;
  REQUIRE(s.paint_part_mask(MaskPaintRequest{extra, ViewRole::Front, {{10, 10}, {11, 10}}, {}, {}, false}).success);
  const OpResult untyped = s.lowpoly_build();
  REQUIRE_FALSE(untyped.success);
  CHECK(untyped.errors.front().code == "lowpoly.part_type_missing");
  REQUIRE(s.set_part_type({extra, lowpoly::PartType::Accessory, {}}).success);
  CHECK(s.document().part_types.at(extra) == lowpoly::PartType::Accessory);
  CHECK_FALSE(s.set_part_type({999999, lowpoly::PartType::Head, {}}).success);
}

TEST_CASE("lowpoly session: settings changes are undoable and every input change drops the build") {
  Session s;
  start(s, "cloaked", true);
  REQUIRE(s.lowpoly_build().success);
  REQUIRE(current_lowpoly(s.document()));
  const OpResult small = s.lowpoly_build(LowpolyUpdate{.tri_budget = 600});
  REQUIRE(small.success);
  CHECK(small.data["lowpoly"]["triangles"].get<int>() <= 600);
  CHECK(s.document().settings.lowpoly.build.tri_budget == 600);
  REQUIRE(s.undo().success);
  CHECK(s.document().settings.lowpoly.build.tri_budget == 1000);
  REQUIRE(current_lowpoly(s.document()));  // the earlier build belongs to the restored state
  CHECK(current_lowpoly(s.document())->input.settings.tri_budget == 1000);
  // Part type change → the build no longer matches and is dropped.
  const int part = s.document().masks.parts.begin()->first;
  REQUIRE(s.set_part_type({part, lowpoly::PartType::Accessory, {}}).success);
  CHECK_FALSE(s.document().lowpoly);
  CHECK(s.lowpoly_render({}, nullptr).errors.front().code == "state.not_ready");
  CHECK_FALSE(s.set_lowpoly_settings(LowpolyUpdate{.tri_budget = 100}).success);  // below the minimum
  CHECK(s.set_lowpoly_settings(LowpolyUpdate{.style = lowpoly::Style::Flat}).success);
  CHECK(s.document().settings.lowpoly.build.style == lowpoly::Style::Flat);
}

TEST_CASE("lowpoly session: renders at fixed yaws, the test pose and a contact sheet") {
  Session s;
  start(s, "humanoid", true);
  REQUIRE(s.lowpoly_build().success);
  std::vector<NamedImage> imgs;
  LowpolyRenderRequest req;
  req.size = 96;
  req.contact_sheet = true;
  const OpResult r = s.lowpoly_render(req, &imgs);
  REQUIRE(r.success);
  REQUIRE(imgs.size() == 5);
  CHECK(imgs[0].name == "yaw_000");
  CHECK(imgs[3].name == "yaw_270");
  CHECK(imgs[0].image.width() == 96);
  CHECK(imgs.back().image.width() == 4 * 96);
  std::vector<NamedImage> posed;
  req.pose = true;
  req.contact_sheet = false;
  req.yaws = {0, 90};
  REQUIRE(s.lowpoly_render(req, &posed).success);
  CHECK(posed[0].name == "pose_000");
  // Deterministic.
  std::vector<NamedImage> again;
  req.pose = false;
  req.yaws = {0};
  REQUIRE(s.lowpoly_render(req, &again).success);
  CHECK(again[0].image == imgs[0].image);
}

TEST_CASE("lowpoly session: export writes the round-tripped GLB; front only needs an explicit decision") {
  Session s;
  start(s, "cloaked", false);  // front only: depth inferred → NEEDS_REVIEW
  const OpResult b = s.lowpoly_build();
  REQUIRE(b.success);
  CHECK(b.status == ValidationStatus::NeedsReview);
  const fs::path glb = tmp("cloaked_front.glb");
  fs::remove(glb);
  const OpResult refused = s.lowpoly_export({glb});
  REQUIRE_FALSE(refused.success);
  CHECK(refused.errors.front().code == "lowpoly.export_refused");
  CHECK_FALSE(fs::exists(glb));
  // The user accepts inferred depth for batch/export: allowed, the status stays NEEDS_REVIEW.
  REQUIRE(s.set_lowpoly_settings(LowpolyUpdate{.accept_inferred_depth = true}).success);
  const OpResult ok = s.lowpoly_export({glb, false, true});
  REQUIRE_MESSAGE(ok.success, to_json(ok).dump());
  CHECK(ok.status == ValidationStatus::NeedsReview);
  CHECK(fs::file_size(glb) == s.document().lowpoly->glb.size());
  CHECK(ok.data["files"].size() == 4);
  CHECK(fs::exists(tmp("cloaked_front_toon.gdshader")));
  CHECK_FALSE(ok.warnings.empty());
}

TEST_CASE("lowpoly session: save and load keep the low-poly inputs and rebuild the same character") {
  Session s;
  start(s, "humanoid", true);
  REQUIRE(s.lowpoly_build(LowpolyUpdate{.tri_budget = 800, .joint_loops = 1}).success);
  const int tris = s.document().lowpoly->result.triangles;
  const auto glb = s.document().lowpoly->glb;
  const fs::path p = tmp("humanoid.p3d");
  REQUIRE(s.save(p).success);
  Session t;
  REQUIRE(t.load(p).success);
  CHECK(t.document().settings.lowpoly == s.document().settings.lowpoly);
  CHECK(t.document().part_types == s.document().part_types);
  CHECK(t.document().masks == s.document().masks);
  const OpResult b = t.lowpoly_build();
  REQUIRE(b.success);
  CHECK(t.document().lowpoly->result.triangles == tris);
  CHECK(t.document().lowpoly->glb == glb);
}
