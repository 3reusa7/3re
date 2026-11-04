#pragma once
// G0 transform hygiene (docs/agent/ASSET-QUALITY.md): every delivered hierarchy is free of mirrored
// (negative-determinant) and non-uniform node transforms, its winding agrees with its normals, and every
// normal-mapped primitive carries tangents. Measures and repairs; the thresholds live in ASSET-QUALITY.md.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_glb.hpp"

namespace p3dagent {

struct HygieneAudit {
  int nodes = 0;
  std::vector<std::string> negative_det;   // nodes whose world transform mirrors (det < 0)
  std::vector<std::string> non_uniform;    // nodes whose local scale/matrix scales the axes differently
  std::vector<std::string> unbaked_scale;  // uniform but not 1 (G0: transforms are baked into the vertices)
  std::vector<std::string> non_rigid_pivots;  // part nodes (mesh under a parent) whose transform is not rigid
  int64_t triangles = 0;
  int64_t normal_checked = 0, normal_disagree = 0;  // triangles whose face normal opposes their vertex normals
  int normal_mapped_primitives = 0, missing_tangents = 0, bad_tangent_w = 0;
  std::vector<std::string> unreadable;     // primitives that could not be read (reported, never guessed)
  double normal_disagree_fraction() const { return normal_checked ? double(normal_disagree) / double(normal_checked) : 0.0; }
  nlohmann::json to_json() const;
};
HygieneAudit audit_hygiene(const Glb& glb);

// PASS / REVIEW / FAIL with the reasons. `max_disagree_fraction` comes from ASSET-QUALITY.md (G0 thresholds).
struct HygieneVerdict {
  std::string status;
  std::vector<std::string> reasons;
};
HygieneVerdict judge_hygiene(const HygieneAudit& a, double max_disagree_fraction);

// Bakes every node transform into its mesh's vertices (positions by M, normals by M^-T, tangents by M with
// w *= sign(det M), triangle winding reversed where det M < 0) and leaves every mesh node a scene root with an
// identity transform. UVs, materials and indices of unchanged primitives are kept. nullopt + `why` when the file
// cannot be baked exactly (skins, animations, a mesh used by two nodes, non-float vertex data).
std::optional<Glb> bake_node_transforms(const Glb& glb, std::string* why = nullptr);

}  // namespace p3dagent
