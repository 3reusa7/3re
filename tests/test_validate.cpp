#include <doctest/doctest.h>

#include "pipeline_helpers.hpp"
#include "pixel3d/analysis/conflicts.hpp"
#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/reconstruct/true_solid.hpp"
#include "pixel3d/texture/projector.hpp"
#include "pixel3d/validate/metrics.hpp"
#include "pixel3d/validate/quality_gate.hpp"
#include "pixel3d/validate/report.hpp"

using namespace pixel3d;
using namespace p3dtest;

namespace {

struct Validated {
  Corr corr;
  SolidResult solid;
  TextureResult tex;
  QualityMetrics metrics;
  std::vector<Conflict> conflicts;
  GateResult gate;
};

Validated validate_corr(Corr c, ToleranceProfile profile) {
  Validated v{std::move(c), {}, {}, {}, {}, {}};
  v.solid = assemble_true_solid(v.corr.cands, {}, v.corr.dims, CleanupOptions{}, v.corr.part_of_candidate());
  std::vector<const ImageRGBA*> imgs;
  for (const auto& s : v.corr.views) imgs.push_back(s.aligned.get());
  const MaterialFamilies fam = analyze_palette(imgs);
  v.tex = project_textures(v.solid.mesh, v.corr.views, fam, v.corr.dims, TextureOptions{}, {});
  v.metrics = compute_metrics(v.tex.mesh, v.solid.mesh, v.corr.views, v.corr.dims, v.corr.result, v.tex);
  const ExportOptions eo;
  v.metrics.glb = to_json(check_glb_roundtrip(write_glb(v.tex.mesh, eo), v.tex.mesh, eo, v.corr.dims));
  const VoxelGrid kept = voxelize_candidates(v.corr.cands, v.corr.masks, v.corr.bands, v.corr.dims);
  std::map<ViewRole, int> mismatch;
  for (const auto& [r, m] : v.metrics.views) mismatch[r] = m.mismatch_px;
  v.conflicts = collect_conflicts(AlignResult{}, v.corr.result, v.corr.cands,
                                  analyze_recesses(v.corr.regions, kept, v.corr.dims), &v.tex, mismatch);
  GateConfig cfg;
  cfg.profile = profile;
  v.gate = run_quality_gate(v.metrics, v.conflicts, cfg);
  return v;
}

const Check* check(const GateResult& g, const std::string& name) {
  for (const auto& c : g.checks)
    if (c.name == name) return &c;
  return nullptr;
}

}  // namespace

TEST_CASE("validate: cardboard plane fails thickness") {
  const Validated v = validate_corr(run_boxes({Box{0, 0, 0, 10, 10, 1}}, Dims{10, 10, 1}), ToleranceProfile::Strict);
  CHECK(v.metrics.thickness_ratio < 0.2);
  CHECK(v.metrics.turntable.min_area_ratio < 0.25);
  CHECK(v.gate.status == ValidationStatus::Fail);
  REQUIRE(check(v.gate, "thickness_ratio"));
  CHECK_FALSE(check(v.gate, "thickness_ratio")->passed);
  CHECK(check(v.gate, "thickness_ratio")->critical);
}

TEST_CASE("validate: A metrics strict pass") {
  const Validated v = validate_corr(run_fixture("A_synthetic_chair"), ToleranceProfile::Strict);
  for (const auto& [r, m] : v.metrics.views) {
    CAPTURE(to_string(r));
    CHECK(m.iou >= 0.98);
  }
  REQUIRE(v.metrics.rotations.size() == 3);
  for (const auto& rot : v.metrics.rotations) {
    CAPTURE(rot.yaw);
    CHECK(rot.iou >= 0.98);
  }
  CHECK(v.metrics.thickness_ratio >= 0.5);
  CHECK(v.metrics.turntable.frames == 24);
  CHECK(v.metrics.turntable.all_non_empty);
  CHECK(v.metrics.turntable.min_area_ratio >= 0.25);
  CHECK(v.metrics.topology.watertight);
  CHECK(v.gate.status == ValidationStatus::Pass);
}

TEST_CASE("validate: ambiguity blocks pass") {
  const Validated v = validate_corr(run_fixture("C_asym_planter"), ToleranceProfile::Strict);
  CHECK(v.gate.status == ValidationStatus::NeedsReview);
  CHECK(v.metrics.ambiguous_volume_fraction > 0);
}

TEST_CASE("validate: non-watertight is critical fail") {
  QualityMetrics m;
  m.topology.triangle_count = 12;
  m.topology.watertight = false;
  m.thickness_ratio = 1;
  m.turntable = TurntableMetric{24, 1, 1, true, {}, {}};
  const GateResult g = run_quality_gate(m, {}, GateConfig{});
  CHECK(g.status == ValidationStatus::Fail);
  CHECK_FALSE(check(g, "watertight")->passed);
}

TEST_CASE("validate: hand profile tolerates a few mismatched pixels") {
  QualityMetrics m;
  m.topology.triangle_count = 12;
  m.topology.watertight = true;
  m.topology.signed_volume = 1;
  m.glb = {{"ok", true}};
  m.thickness_ratio = 1;
  m.turntable = TurntableMetric{24, 1, 1, true, {}, {}};
  m.views[ViewRole::Front] = ViewMetric{0.97, 3, 1.0, 400};
  GateConfig hand;
  CHECK(run_quality_gate(m, {}, hand).status == ValidationStatus::Pass);
  GateConfig strict;
  strict.profile = ToleranceProfile::Strict;
  CHECK(run_quality_gate(m, {}, strict).status == ValidationStatus::Fail);
  m.views[ViewRole::Front].mismatch_px = 40;  // 10% of the mask
  CHECK(run_quality_gate(m, {}, hand).status == ValidationStatus::Fail);
}

TEST_CASE("validate: report has schema and all metric keys") {
  const Validated v = validate_corr(run_fixture("A_synthetic_chair"), ToleranceProfile::Strict);
  const nlohmann::json r = make_report(v.gate, v.metrics, v.conflicts, v.corr.result, {{"fixture", "A"}});
  CHECK(r["schema"] == "pixel3d.report/1");
  CHECK(r["status"] == "PASS");
  for (const char* k : {"schema", "status", "profile", "inputs", "parts", "conflicts", "metrics", "checks"})
    CHECK(r.contains(k));
  const auto& m = r["metrics"];
  for (const char* k : {"views", "rotations", "thickness_ratio", "turntable", "volume", "mesh", "texture",
                        "inferred_volume_fraction", "ambiguous_volume_fraction"})
    CHECK(m.contains(k));
  CHECK(m["views"].contains("front"));
  CHECK(m["mesh"].contains("non_manifold_edges"));
  CHECK(m["mesh"].contains("internal_faces"));
  CHECK(m["mesh"].contains("triangles"));
  CHECK(m["turntable"]["frames"] == 24);
  CHECK(r["parts"].size() == 5);
}

namespace {

QualityMetrics clean_metrics() {
  QualityMetrics m;
  m.topology.triangle_count = 12;
  m.topology.watertight = true;
  m.topology.signed_volume = 1;
  m.thickness_ratio = 1;
  m.turntable = TurntableMetric{24, 1, 1, true, {}, {}};
  m.glb = {{"ok", true}};
  return m;
}

}  // namespace

TEST_CASE("validate: PASS needs the GLB round trip") {
  QualityMetrics m = clean_metrics();
  CHECK(run_quality_gate(m, {}, GateConfig{}).status == ValidationStatus::Pass);
  m.glb = nullptr;  // not run
  const GateResult g = run_quality_gate(m, {}, GateConfig{});
  CHECK(g.status == ValidationStatus::NeedsReview);
  REQUIRE(check(g, "glb_roundtrip"));
  CHECK_FALSE(check(g, "glb_roundtrip")->passed);
}

TEST_CASE("validate: inside-out mesh is a critical failure") {
  QualityMetrics m = clean_metrics();
  m.topology.signed_volume = -1;
  const GateResult g = run_quality_gate(m, {}, GateConfig{});
  CHECK(g.status == ValidationStatus::Fail);
  CHECK(check(g, "winding")->critical);
}
