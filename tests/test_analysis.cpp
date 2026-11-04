#include <doctest/doctest.h>

#include "pipeline_helpers.hpp"
#include "pixel3d/analysis/conflicts.hpp"
#include "pixel3d/analysis/recess.hpp"

using namespace pixel3d;
using namespace p3dtest;

namespace {

std::vector<RecessFinding> recesses_of(const Corr& c) {
  const VoxelGrid kept = voxelize_candidates(c.cands, c.masks, c.bands, c.dims);
  return analyze_recesses(c.regions, kept, c.dims);
}

int count(const std::vector<RecessFinding>& f, const std::string& kind) {
  int n = 0;
  for (const auto& r : f) n += r.kind == kind ? 1 : 0;
  return n;
}

const Conflict* find(const std::vector<Conflict>& cs, const std::string& kind) {
  for (const auto& c : cs)
    if (c.kind == kind) return &c;
  return nullptr;
}

}  // namespace

TEST_CASE("analysis: C cavity is possible_recess on Top") {
  const auto f = recesses_of(run_fixture("C_asym_planter"));
  REQUIRE(count(f, "possible_recess") == 1);
  const RecessFinding* r = nullptr;
  for (const auto& x : f)
    if (x.kind == "possible_recess") r = &x;
  CHECK(r->view == ViewRole::Top);
  CHECK(r->luminance_ratio < 0.88);
  // Upper bound of what may be empty: the filled cavity (14×10×11) plus at most the floor below it.
  CHECK(r->column_volume >= 14 * 10 * 11);
  CHECK(r->column_volume <= 14 * 10 * 11 * 1.4);
  // The flagged columns cover the cavity footprint (aligned x 2..16, z 2..12).
  CHECK(r->box.x0 <= 2);
  CHECK(r->box.x1 >= 16);
  CHECK(r->box.y0 <= 2);
  CHECK(r->box.y1 >= 12);
}

TEST_CASE("analysis: A has no possible_recess") {
  CHECK(count(recesses_of(run_fixture("A_synthetic_chair")), "possible_recess") == 0);
  CHECK(count(recesses_of(run_fixture("B_hand_chair")), "possible_recess") == 0);
}

TEST_CASE("analysis: painted same-luminance detail is inferred_flush") {
  // A box with an enclosed, lighter painted logo on its top: depth unknown but nothing suggests a hole.
  const Rgba8 brown{150, 100, 60, 255}, logo{230, 200, 80, 255};
  std::vector<Box> boxes = {Box{0, 0, 0, 10, 4, 10, brown}};
  const Dims d{10, 4, 10};
  Corr c = run_boxes(boxes, d);
  for (auto& v : c.views)
    if (v.role == ViewRole::Top) {
      ImageRGBA img = *v.aligned;
      for (int z = 3; z < 7; ++z)
        for (int x = 3; x < 7; ++x) img.at(x, z) = logo;
      v.aligned = std::make_shared<const ImageRGBA>(img);
    }
  c = run(c.views, d);
  const auto f = recesses_of(c);
  CHECK(count(f, "possible_recess") == 0);
  CHECK(count(f, "inferred_flush") == 1);
}

TEST_CASE("analysis: conflicts mark blocking kinds per spec table") {
  AlignResult align;
  align.messages.push_back(Message{"extent_mismatch", "x", Severity::Error, {}});
  align.profile_conflicts[ViewRole::Front] = 3;
  CorrespondenceResult corr;
  corr.ambiguities.push_back(Ambiguity{"ambiguous_correspondence", 0, {1, 2}, {}, "t"});
  corr.ambiguities.push_back(Ambiguity{"evidence_conflict", 0, {3}, {}, "t"});
  CandidateSet cands;
  Candidate grid;
  grid.status = CandStatus::InferredGrid;
  cands.cands.push_back(grid);
  std::vector<RecessFinding> rec = {RecessFinding{ViewRole::Top, 0, "possible_recess", 10, 0.5, 100, {}},
                                    RecessFinding{ViewRole::Front, 1, "inferred_flush", 4, 1.1, 8, {}}};
  const auto cs = collect_conflicts(align, corr, cands, rec, nullptr, {});
  REQUIRE(find(cs, "extent_mismatch"));
  CHECK(find(cs, "extent_mismatch")->blocks_pass);
  CHECK(find(cs, "ambiguous_correspondence")->blocks_pass);
  CHECK(find(cs, "evidence_conflict")->blocks_pass);
  CHECK(find(cs, "possible_recess")->blocks_pass);
  REQUIRE(find(cs, "inferred_flush"));
  CHECK_FALSE(find(cs, "inferred_flush")->blocks_pass);
  REQUIRE(find(cs, "inferred_grid"));
  CHECK_FALSE(find(cs, "inferred_grid")->blocks_pass);
  CHECK(find(cs, "inferred_grid")->severity == Severity::Info);
  REQUIRE(find(cs, "silhouette_conflict"));
  CHECK(find(cs, "silhouette_conflict")->where["view"] == "front");
}
