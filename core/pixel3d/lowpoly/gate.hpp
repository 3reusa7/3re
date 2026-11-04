#pragma once
// LOW-POLY STUDIO validation: a separate gate on a separate output (the
// existing gate and its thresholds are untouched). FAIL on a critical check
// (budget, closed shells, skin weights, adjacency, rigid invariance, glTF round
// trip); NEEDS_REVIEW on a blocking conflict (depth inferred from one view
// without a user depth, silhouette deviation beyond what the budget can hold,
// an unfitting forced template) or when the export was not round-tripped;
// otherwise PASS. Inferred things are listed, never counted as evidence.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/lowpoly/build.hpp"
#include "pixel3d/lowpoly/rig.hpp"
#include "pixel3d/lowpoly/skinned_gltf.hpp"
#include "pixel3d/validate/quality_gate.hpp"

namespace pixel3d::lowpoly {

inline constexpr double kSilhouetteIouMin = 0.90;
inline constexpr double kJointVolumeMin = 0.80;  // informational

struct LowpolyConflict {
  std::string kind;
  bool blocking = false;
  std::string text;
  nlohmann::json where;
};

struct LowpolyValidation {
  ValidationStatus status = ValidationStatus::NotValidated;
  std::vector<Check> checks;
  std::vector<LowpolyConflict> conflicts;
  std::map<ViewRole, double> silhouette_iou;
  RigCheck rig;
  nlohmann::json report;  // everything a client shows: counts, per-part triangles, inferred, checks
  bool inferred_depth_accepted = false;  // the user's accept_inferred_depth (front only)
};

// Front (and Side/Top) silhouette of the rest-pose mesh vs the subject masks.
std::map<ViewRole, double> silhouette_iou(const Input& in, const Result& r);

// `exported`: the round-trip check of the written GLB (nullptr: not exported yet → NEEDS_REVIEW at best).
LowpolyValidation validate_lowpoly(const Input& in, const Result& r, const SkinnedCheck* exported);

nlohmann::json to_json(const LowpolyValidation& v);

// May this result be exported / used in a batch? PASS: yes. NEEDS_REVIEW: only when
// every blocking conflict is inferred depth and the user enabled accept_inferred_depth
// (the status itself stays NEEDS_REVIEW). FAIL: never.
struct ExportDecision {
  bool allowed = false;
  bool accepted_inferred_depth = false;
  std::string reason;
};
ExportDecision export_decision(const LowpolyValidation& v, const Settings& s);

}  // namespace pixel3d::lowpoly
