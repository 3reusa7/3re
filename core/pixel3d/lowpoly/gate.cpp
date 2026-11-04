#include "pixel3d/lowpoly/gate.hpp"

#include <algorithm>
#include <cmath>

#include "pixel3d/mesh/topology.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d::lowpoly {

std::map<ViewRole, double> silhouette_iou(const Input& in, const Result& r) {
  std::map<ViewRole, double> out;
  const SkinnedMesh& M = r.mesh();
  Mesh flat;
  flat.positions = M.positions;
  const size_t n = M.hull_appended ? M.hull_first_index : M.indices.size();
  for (size_t t = 0; t + 2 < n; t += 3) flat.tris.push_back({M.indices[t], M.indices[t + 1], M.indices[t + 2]});
  flat.tri_part.assign(flat.tris.size(), 0);
  for (const auto& [role, subject] : r.layout.subject) {
    const RenderTarget rt = render_mesh_flat(flat, camera_for_view(role, in.dims, 1));
    out[role] = mask_iou(rt.coverage, subject);
  }
  return out;
}

LowpolyValidation validate_lowpoly(const Input& in, const Result& r, const SkinnedCheck* exported) {
  LowpolyValidation v;
  const SkinnedMesh& M = r.mesh();
  auto check = [&](std::string name, bool critical, bool passed, double value, double threshold, std::string text) {
    v.checks.push_back({std::move(name), critical, passed, value, threshold, std::move(text)});
  };
  const int budget = in.settings.tri_budget;
  check("lowpoly.tri_budget", true, r.triangles <= budget, r.triangles, budget,
        "exported triangles (hull included) must not exceed the budget");
  int open = 0;
  for (const auto& sh : r.assembly.shells) {
    const TopologyReport t = analyze_topology(shell_mesh(sh));
    open += !(t.watertight && t.inconsistent_winding_edges == 0 && t.signed_volume > 0);
  }
  check("lowpoly.shells_watertight", true, open == 0, open, 0, "every part shell is closed, consistently and outward wound");
  const Pose pose = test_pose(r.layout);
  const PoseResult pr = apply_pose(M, r.layout.skeleton, pose);
  v.rig = check_rig(M, r.layout.skeleton, pose, pr, &r.assembly);
  check("lowpoly.skin_weights", true, v.rig.weights_ok(), v.rig.bad_weight_sum + v.rig.bad_joint_index + v.rig.unskinned, 0,
        "every vertex skinned, weights sum to 1, valid joints");
  check("lowpoly.skin_adjacency", true, v.rig.adjacency_ok(), v.rig.non_adjacent + v.rig.blended_off_loop, 0,
        "blending only on joint loops, only between a bone and its parent");
  check("lowpoly.rigid_invariance", true, v.rig.rigid_ok(), std::max(v.rig.rigid_max_error, v.rig.rigid_max_distance_change), 1e-6,
        "under the test pose, single-bone vertices keep their place in their bone's frame");
  if (exported)
    check("lowpoly.gltf_roundtrip", true, exported->ok, exported->ok ? 1 : 0, 1,
          "the skinned GLB reloads (tinygltf) with every attribute, the skin and the joints intact");
  double min_volume = 1;
  for (const auto& [name, ratio] : v.rig.joint_volume) min_volume = std::min(min_volume, ratio);
  check("lowpoly.joint_volume", false, min_volume >= kJointVolumeMin, min_volume, kJointVolumeMin,
        "volume kept around each bent joint of the test pose (linear blend skinning thins joints)");
  v.silhouette_iou = silhouette_iou(in, r);
  for (const auto& [role, iou] : v.silhouette_iou) {
    const std::string name = "lowpoly.silhouette_iou_" + std::string(to_string(role));
    check(name, false, iou >= kSilhouetteIouMin, iou, kSilhouetteIouMin, "rest-pose silhouette vs the drawing");
    if (iou < kSilhouetteIouMin)
      v.conflicts.push_back({"lowpoly.silhouette_deviation", true,
                             "the " + std::string(to_string(role)) + " silhouette deviates from the drawing (IoU " +
                                 std::to_string(iou).substr(0, 5) + "): the budget cannot hold this outline",
                             {{"view", to_string(role)}, {"iou", iou}}});
  }
  // Conflicts from the build.
  const bool measured_depth = r.layout.subject.contains(ViewRole::Side) || r.layout.subject.contains(ViewRole::Top);
  v.inferred_depth_accepted = !measured_depth && in.settings.accept_inferred_depth;
  if (!measured_depth && !in.settings.user_depth)
    v.conflicts.push_back({"lowpoly.inferred_depth", true,
                           std::string("front only: every depth is inferred from the part types (depth/width minimums) and "
                                       "silhouette inflation; add a Side drawing or set a depth") +
                               (v.inferred_depth_accepted ? " — accepted by the user for batch/export (accept_inferred_depth)" : ""),
                           {{"accepted", v.inferred_depth_accepted}}});
  else if (!measured_depth)
    v.conflicts.push_back({"lowpoly.inferred_depth", false, "front only: depth inferred by inflation, clamped to the user's depth",
                           {{"depth", in.settings.depth}}});
  for (const auto& m : r.messages) {
    if (m.code == "lowpoly.template_mismatch") v.conflicts.push_back({m.code, true, m.text, m.where});
    else if (m.code == "lowpoly.inferred_joint" || m.code == "lowpoly.inferred_surface" || m.code == "lowpoly.mirrored" ||
             m.code == "lowpoly.ink_band_removed" || m.code == "lowpoly.baked_lighting" || m.code == "lowpoly.hull")
      v.conflicts.push_back({m.code, false, m.text, m.where});
  }
  for (const auto& t : r.tubes)
    if (t.depth_source.find("inflation") != std::string::npos && measured_depth)
      v.conflicts.push_back({"lowpoly.inferred_depth_part", false, t.name + ": some depth inferred (" + t.depth_source + ")",
                             {{"tube", t.name}, {"source", t.depth_source}}});
  if (r.layout.tubes.size() > 1)
    v.conflicts.push_back({"lowpoly.overlapping_shells", false,
                           "branches are separate closed shells overlapping their parent (no welding: rig-friendly, fewer triangles)", {}});
  const bool fail = std::any_of(v.checks.begin(), v.checks.end(), [](const Check& c) { return c.critical && !c.passed; });
  const bool blocking = std::any_of(v.conflicts.begin(), v.conflicts.end(), [](const LowpolyConflict& c) { return c.blocking; });
  v.status = fail ? ValidationStatus::Fail : (blocking || !exported) ? ValidationStatus::NeedsReview : ValidationStatus::Pass;
  // Report.
  nlohmann::json parts = nlohmann::json::array(), tubes = nlohmann::json::array();
  for (const auto& p : r.parts)
    parts.push_back({{"id", p.part}, {"name", p.name}, {"type", to_string(p.type)}, {"triangles", p.triangles},
                     {"depth_ratio", p.depth_ratio}, {"depth_source", p.depth_source}});
  for (const auto& t : r.tubes)
    tubes.push_back({{"name", t.name}, {"sides", t.sides}, {"rings", t.rings}, {"joints", t.joints}, {"joint_loops", t.joint_loops},
                     {"triangles", t.triangles}, {"profile_error_px", t.profile_error_px}, {"depth", t.depth_source}, {"branch", t.branch}});
  v.report = {{"template", to_string(r.layout.templ)},
              {"budget", budget},
              {"triangles", r.triangles},
              {"hull_triangles", r.hull_triangles},
              {"parts", parts},
              {"tubes", tubes},
              {"bones", r.layout.skeleton.bones.size()},
              {"atlas", {r.pack.width, r.pack.height}},
              {"texel_density", r.pack.density},
              {"surface", r.bake.share},
              {"inferred_depth_accepted", v.inferred_depth_accepted}};
  return v;
}

nlohmann::json to_json(const LowpolyValidation& v) {
  nlohmann::json checks = nlohmann::json::array(), conflicts = nlohmann::json::array();
  for (const auto& c : v.checks)
    checks.push_back({{"name", c.name}, {"critical", c.critical}, {"passed", c.passed}, {"value", c.value}, {"threshold", c.threshold}, {"text", c.text}});
  for (const auto& c : v.conflicts) conflicts.push_back({{"kind", c.kind}, {"blocking", c.blocking}, {"text", c.text}, {"where", c.where}});
  nlohmann::json j = v.report;
  j["status"] = to_string(v.status);
  j["inferred_depth_accepted"] = v.inferred_depth_accepted;
  j["checks"] = checks;
  j["conflicts"] = conflicts;
  return j;
}

ExportDecision export_decision(const LowpolyValidation& v, const Settings& s) {
  ExportDecision d;
  if (v.status == ValidationStatus::Pass) {
    d.allowed = true;
    d.reason = "PASS";
    return d;
  }
  if (v.status != ValidationStatus::NeedsReview) {
    d.reason = std::string(to_string(v.status)) + ": not exported";
    return d;
  }
  bool only_depth = true, depth = false;
  for (const auto& c : v.conflicts) {
    if (!c.blocking) continue;
    if (c.kind == "lowpoly.inferred_depth") depth = true;
    else only_depth = false;
  }
  if (depth && only_depth && s.accept_inferred_depth) {
    d.allowed = d.accepted_inferred_depth = true;
    d.reason = "NEEDS_REVIEW (inferred depth) accepted by the user (accept_inferred_depth)";
  } else {
    d.reason = "NEEDS_REVIEW: blocking conflicts remain";
  }
  return d;
}

}  // namespace pixel3d::lowpoly
