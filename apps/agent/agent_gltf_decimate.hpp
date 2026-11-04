#pragma once
// Multi-mesh, UV-preserving decimation of a foreign GLB (agent milestone 2, A3): every triangle mesh keeps its
// own vertices, UVs, normals, materials and images; only new index buffers are written. The triangle budget is
// shared between meshes by world surface area (instances counted). meshoptimizer 0.24 (MIT),
// meshopt_simplifyWithAttributes: UV and normal seams are kept, open borders locked first.
#include <cstdint>
#include <nlohmann/json.hpp>
#include "agent_glb.hpp"

namespace p3dagent {

struct GltfDecimateResult {
  Glb glb;
  int64_t triangles_before = 0, triangles_after = 0;
  bool budget_met = false;
  nlohmann::json report;  // per mesh: triangles, target, error, border locked
};
// target = total triangles (instances counted). Throws on unreadable accessors.
GltfDecimateResult decimate_gltf(const Glb& in, int64_t target);

}  // namespace p3dagent
