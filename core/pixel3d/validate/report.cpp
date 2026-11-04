#include "pixel3d/validate/report.hpp"

namespace pixel3d {

nlohmann::json metrics_json(const QualityMetrics& m) {
  nlohmann::json j;
  for (const auto& [role, v] : m.views)
    j["views"][std::string(to_string(role))] = {{"iou", v.iou},
                                                {"mismatch_px", v.mismatch_px},
                                                {"color_exact_fraction", v.color_exact_fraction},
                                                {"mask_area", v.mask_area}};
  j["rotations"] = nlohmann::json::array();
  for (const RotationMetric& r : m.rotations)
    j["rotations"].push_back({{"yaw", r.yaw}, {"iou", r.iou}, {"projected_width", r.projected_width}});
  j["thickness_ratio"] = m.thickness_ratio;
  j["turntable"] = {{"frames", m.turntable.frames},
                    {"min_area_ratio", m.turntable.min_area_ratio},
                    {"min_width_ratio", m.turntable.min_width_ratio},
                    {"all_non_empty", m.turntable.all_non_empty},
                    {"areas", m.turntable.areas},
                    {"widths", m.turntable.widths}};
  const TopologyReport& t = m.topology;
  j["mesh"] = {{"watertight", t.watertight},
               {"boundary_edges", t.boundary_edges},
               {"non_manifold_edges", t.nonmanifold_edges},
               {"non_manifold_vertices", t.nonmanifold_vertices},
               {"inconsistent_winding_edges", t.inconsistent_winding_edges},
               {"internal_faces", t.internal_faces},
               {"duplicate_faces", t.duplicate_faces},
               {"zero_area_faces", t.zero_area_faces},
               {"shells", t.shells},
               {"triangles", t.triangle_count},
               {"vertices", t.vertex_count},
               {"planar_regions", t.planar_regions}};
  j["volume"] = m.volume;
  j["inferred_volume_fraction"] = m.inferred_volume_fraction;
  j["ambiguous_volume_fraction"] = m.ambiguous_volume_fraction;
  j["texture"] = m.texture;
  j["glb"] = m.glb;
  if (m.volume_iou_gt) j["volume_iou_ground_truth"] = *m.volume_iou_gt;
  return j;
}

nlohmann::json make_report(const GateResult& gate, const QualityMetrics& m, const std::vector<Conflict>& conflicts,
                           const CorrespondenceResult& corr, const nlohmann::json& inputs, ToleranceProfile profile) {
  nlohmann::json r;
  r["schema"] = "pixel3d.report/1";
  r["status"] = std::string(to_string(gate.status));
  r["profile"] = std::string(to_string(profile));
  r["inputs"] = inputs;
  r["parts"] = nlohmann::json::array();
  for (const Part3D& p : corr.parts)
    r["parts"].push_back({{"id", p.id}, {"label", p.label}, {"status", to_string(p.status)}, {"candidates", p.candidates}});
  r["conflicts"] = conflicts;
  r["metrics"] = metrics_json(m);
  r["checks"] = nlohmann::json::array();
  for (const Check& c : gate.checks)
    r["checks"].push_back({{"name", c.name},
                           {"critical", c.critical},
                           {"passed", c.passed},
                           {"value", c.value},
                           {"threshold", c.threshold},
                           {"text", c.text}});
  return r;
}

}  // namespace pixel3d
