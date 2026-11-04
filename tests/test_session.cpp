#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "pixel3d/engine/session.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

std::string fixture(const std::string& name, const std::string& role) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + role + ".png";
}

std::vector<ViewImport> fst(const std::string& name) {
  return {{ViewRole::Front, fixture(name, "front"), {}},
          {ViewRole::Side, fixture(name, "side"), {}},
          {ViewRole::Top, fixture(name, "top"), {}}};
}

fs::path temp_file(const std::string& name, const std::string& content) {
  const fs::path dir = fs::temp_directory_path() / "pixel3d_tests";
  fs::create_directories(dir);
  const fs::path p = dir / name;
  std::ofstream(p, std::ios::binary) << content;
  return p;
}

bool has_error(const OpResult& r, const std::string& code) {
  for (const auto& e : r.errors)
    if (e.code == code) return true;
  return false;
}

}  // namespace

TEST_CASE("session: full pipeline on A ends PASS") {
  Session s(Source::Cli);
  ProjectSettings ps;
  ps.profile = ToleranceProfile::Strict;
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.auto_align().success);
  REQUIRE(s.segment_parts().success);
  const OpResult m = s.match_parts();
  REQUIRE(m.success);
  CHECK(m.data["parts"].size() == 5);
  const OpResult r = s.reconstruct(ReconstructOptions{});
  REQUIRE(r.success);
  CHECK(r.stats.watertight);
  CHECK(r.stats.triangles > 0);
  REQUIRE(s.project_textures(TextureOptions{}).success);
  const OpResult v = s.validate(ValidateOptions{});
  REQUIRE(v.success);
  CHECK(v.status == ValidationStatus::Pass);
  CHECK(v.metrics["glb"]["ok"] == true);
}

TEST_CASE("session: reconstruct runs missing stages") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  const OpResult r = s.reconstruct(ReconstructOptions{});
  REQUIRE(r.success);
  CHECK(r.stats.parts == 5);
  CHECK(s.document().solid != nullptr);
}

TEST_CASE("session: undo returns to previous revision and redo reapplies") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  const auto rev = s.document().revision;
  const int tris = s.document().solid->topology.triangle_count;
  REQUIRE(s.set_depth(DepthConstraint{ViewRole::Top, rect_polys(4, 6, 10, 12), 1, 1e9, "dent"}).success);
  CHECK(s.document().revision == rev + 1);
  CHECK(s.document().depth_constraints.size() == 1);
  REQUIRE(s.undo().success);
  CHECK(s.document().revision == rev);
  CHECK(s.document().depth_constraints.empty());
  CHECK(s.document().solid->topology.triangle_count == tris);
  REQUIRE(s.redo().success);
  CHECK(s.document().depth_constraints.size() == 1);
  CHECK_FALSE(s.redo().success);  // nothing left to redo
}

TEST_CASE("session: failing op leaves state unchanged") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  const auto rev = s.document().revision;
  const OpResult r = s.import_views({{ViewRole::Front, "no/such/file.png", {}}});
  CHECK_FALSE(r.success);
  CHECK(has_error(r, "io.not_found"));
  CHECK(s.document().revision == rev);
  CHECK(s.document().views.size() == 3);
}

TEST_CASE("session: bad inputs return structured errors") {
  Session s;
  const fs::path junk = temp_file("junk.png", "this is not a png");
  OpResult r = s.import_views({{ViewRole::Front, junk, {}}});
  CHECK(has_error(r, "image.decode_failed"));
  const fs::path empty = temp_file("empty.png", "");
  {
    ImageRGBA clear(4, 4);
    save_png(clear, empty);
  }
  r = s.import_views({{ViewRole::Front, empty, {}}});
  CHECK(has_error(r, "view.empty"));
  REQUIRE(s.import_views({{ViewRole::Front, fixture("A_synthetic_chair", "front"), {}},
                          {ViewRole::Side, fixture("A_synthetic_chair", "side"), {}}})
              .success);
  r = s.reconstruct(ReconstructOptions{});
  CHECK_FALSE(r.success);
  CHECK(has_error(r, "view.missing_required"));
  r = s.project_textures(TextureOptions{});
  CHECK_FALSE(r.success);
}

TEST_CASE("session: checkpoint restore") {
  Session s;
  REQUIRE(s.import_views(fst("C_asym_planter")).success);
  REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  REQUIRE(s.create_checkpoint("base").success);
  const int tris = s.document().solid->topology.triangle_count;
  REQUIRE(s.set_depth(DepthConstraint{ViewRole::Top, rect_polys(2, 2, 16, 12), 11, 1e9, "cavity"}).success);
  CHECK(s.document().solid->topology.triangle_count != tris);
  REQUIRE(s.restore_checkpoint("base").success);
  CHECK(s.document().solid->topology.triangle_count == tris);
  CHECK(s.document().depth_constraints.empty());
  CHECK_FALSE(s.restore_checkpoint("nope").success);
  // reconstruct created an automatic checkpoint.
  bool auto_cp = false;
  for (const auto& n : s.checkpoint_names()) auto_cp |= n.rfind("auto:before-reconstruct-", 0) == 0;
  CHECK(auto_cp);
}

TEST_CASE("session: log records source and op names") {
  Session s(Source::Mcp);
  s.import_views(fst("A_synthetic_chair"));
  s.import_views({{ViewRole::Front, "missing.png", {}}});
  REQUIRE(s.log().size() == 2);
  CHECK(s.log()[0].name == "import_views");
  CHECK(s.log()[0].source == Source::Mcp);
  CHECK(s.log()[0].success);
  CHECK_FALSE(s.log()[1].success);
}

TEST_CASE("session: OpResult json snapshot") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  const nlohmann::json j = to_json(s.validate(ValidateOptions{}));
  for (const char* k : {"success", "errors", "warnings", "info", "changed", "stats", "metrics", "status", "data"})
    CHECK(j.contains(k));
  CHECK(j["status"] == "PASS");
  for (const char* k : {"triangles", "vertices", "parts", "volume", "watertight"}) CHECK(j["stats"].contains(k));
}

TEST_CASE("session: edit_part and set_depth resolve C") {
  Session s;
  REQUIRE(s.import_views(fst("C_asym_planter")).success);
  OpResult v = s.validate(ValidateOptions{});
  REQUIRE(v.success);
  CHECK(v.status == ValidationStatus::NeedsReview);
  // Decide the legs: keep the three real ones (aligned footprints), remove the phantoms.
  const int real[3][2] = {{1, 11}, {8, 1}, {15, 11}};
  PartEdit edit;
  for (const Candidate& c : s.document().candidates->cands) {
    if (c.status != CandStatus::Ambiguous) continue;
    bool keep = false;
    for (const auto& xz : real) keep |= c.footprint.x0 <= xz[0] && xz[0] < c.footprint.x1 && c.footprint.y0 <= xz[1] && xz[1] < c.footprint.y1;
    edit.decisions.push_back({c.id, keep});
  }
  REQUIRE(edit.decisions.size() == 6);
  REQUIRE(s.edit_part(edit).success);
  v = s.validate(ValidateOptions{});
  CHECK(v.status == ValidationStatus::NeedsReview);  // the cavity is still undecided
  bool recess = false, legs = false;
  for (const auto& c : v.data["conflicts"]) {
    recess |= c["kind"] == "possible_recess";
    legs |= c["kind"] == "ambiguous_correspondence";
  }
  CHECK(recess);
  CHECK_FALSE(legs);
  REQUIRE(s.set_depth(DepthConstraint{ViewRole::Top, rect_polys(2, 2, 16, 12), 11, 1e9, "cavity"}).success);
  v = s.validate(ValidateOptions{});
  CHECK(v.status == ValidationStatus::Pass);
}

TEST_CASE("session: voxel mode reconstructs blocks") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  ReconstructOptions ro;
  ro.mode = ReconstructMode::Voxel;
  const OpResult r = s.reconstruct(ro);
  REQUIRE(r.success);
  CHECK(r.stats.watertight);
  CHECK(s.validate(ValidateOptions{}).success);
}
