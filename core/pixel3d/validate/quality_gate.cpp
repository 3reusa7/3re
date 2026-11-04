#include "pixel3d/validate/quality_gate.hpp"

#include <algorithm>

namespace pixel3d {

std::string_view to_string(ValidationStatus s) {
  switch (s) {
    case ValidationStatus::Pass: return "PASS";
    case ValidationStatus::NeedsReview: return "NEEDS_REVIEW";
    case ValidationStatus::Fail: return "FAIL";
    case ValidationStatus::NotValidated: return "NOT_VALIDATED";
  }
  return "NOT_VALIDATED";
}

std::string_view to_string(ToleranceProfile p) { return p == ToleranceProfile::Strict ? "strict" : "hand"; }

GateResult run_quality_gate(const QualityMetrics& m, const std::vector<Conflict>& conflicts, const GateConfig& cfg) {
  GateResult g;
  g.profile = cfg.profile;
  auto add = [&](std::string name, bool critical, bool passed, double value, double threshold, std::string text) {
    g.checks.push_back(Check{std::move(name), critical, passed, value, threshold, std::move(text)});
  };
  const TopologyReport& t = m.topology;
  add("mesh_non_empty", true, t.triangle_count > 0, t.triangle_count, 1, "the model has geometry");
  add("watertight", true, t.watertight, t.watertight ? 1 : 0, 1, "every edge is shared by exactly two faces");
  add("non_manifold", true, t.nonmanifold_edges == 0 && t.nonmanifold_vertices == 0,
      t.nonmanifold_edges + t.nonmanifold_vertices, 0, "no non-manifold edges or vertices");
  add("internal_faces", false, t.internal_faces == 0 && t.duplicate_faces == 0, t.internal_faces + t.duplicate_faces, 0,
      "no internal or duplicate faces");
  // Critical: an inside-out or inconsistently wound mesh renders wrong in every engine.
  add("winding", true, t.inconsistent_winding_edges == 0 && t.signed_volume > 0, t.inconsistent_winding_edges, 0,
      "consistent outward normals");

  for (const auto& [role, v] : m.views) {
    const std::string name = "projection_" + std::string(to_string(role));
    if (cfg.profile == ToleranceProfile::Strict) {
      const bool ok = v.iou >= cfg.strict_iou;
      add(name, !(cfg.edited && !ok), ok, v.iou, cfg.strict_iou,
          ok || !cfg.edited ? "re-rendered silhouette IoU vs the source" : "deviates from the drawing after user edits");
    } else {
      const double limit = std::max<double>(cfg.hand_mismatch_min, cfg.hand_mismatch_frac * v.mask_area);
      const bool ok = v.mismatch_px <= limit;
      add(name, !(cfg.edited && !ok), ok, v.mismatch_px, limit,
          ok || !cfg.edited ? "mismatched silhouette pixels vs the source" : "deviates from the drawing after user edits");
    }
    add("color_exact_" + std::string(to_string(role)), false, v.color_exact_fraction >= cfg.color_exact_min,
        v.color_exact_fraction, cfg.color_exact_min, "re-rendered pixels with the exact source colour");
  }
  const double rot_min = cfg.profile == ToleranceProfile::Strict ? cfg.strict_iou : cfg.hand_rot_iou;
  for (const RotationMetric& r : m.rotations)
    add("rotation_" + std::to_string(r.yaw), !(cfg.edited && r.iou < rot_min), r.iou >= rot_min, r.iou, rot_min,
        "silhouette after rotating the model matches the mirrored source view");
  add("thickness_ratio", !cfg.single_view, m.thickness_ratio >= cfg.thickness_min, m.thickness_ratio, cfg.thickness_min,
      cfg.single_view ? "informational in single view mode (the depth was chosen)"
                      : "the model keeps real width when rotated (not cardboard)");
  add("turntable_area", !cfg.single_view,
      m.turntable.all_non_empty && m.turntable.min_area_ratio >= cfg.turntable_area_min, m.turntable.min_area_ratio,
      cfg.turntable_area_min,
      cfg.single_view ? "informational in single view mode" : "no turntable frame collapses or disappears");
  const bool glb_checked = !m.glb.is_null();
  if (glb_checked)
    add("glb_roundtrip", true, m.glb.value("ok", false), m.glb.value("ok", false) ? 1 : 0, 1,
        "the exported GLB reloads in an independent parser with identical data");
  else
    add("glb_roundtrip", false, false, 0, 1, "not run: without the GLB check the status cannot be PASS");
  if (m.volume_iou_gt)
    add("volume_iou_ground_truth", false, *m.volume_iou_gt >= 0.95, *m.volume_iou_gt, 0.95,
        "volume overlap with the fixture's ground truth");

  const bool critical_fail = std::any_of(g.checks.begin(), g.checks.end(), [](const Check& c) { return c.critical && !c.passed; });
  const bool blocking = std::any_of(conflicts.begin(), conflicts.end(), [](const Conflict& c) { return c.blocks_pass; });
  g.status = critical_fail                ? ValidationStatus::Fail
             : blocking || !glb_checked ? ValidationStatus::NeedsReview
                                        : ValidationStatus::Pass;
  return g;
}

}  // namespace pixel3d
