#include "pixel3d/reconstruct/true_solid.hpp"

#include "pixel3d/reconstruct/part_geometry.hpp"

namespace pixel3d {

Mesh mesh_from_manifold(const manifold::Manifold& m, const std::map<int, int>& oid_to_part) {
  const manifold::MeshGL64 g = m.GetMeshGL64();
  Mesh out;
  const size_t np = g.numProp;
  for (size_t v = 0; v * np < g.vertProperties.size(); ++v)
    out.positions.push_back({g.vertProperties[v * np], g.vertProperties[v * np + 1], g.vertProperties[v * np + 2]});
  const size_t ntri = g.triVerts.size() / 3;
  out.tris.reserve(ntri);
  out.tri_part.assign(ntri, -1);
  for (size_t t = 0; t < ntri; ++t)
    out.tris.push_back({static_cast<uint32_t>(g.triVerts[3 * t]), static_cast<uint32_t>(g.triVerts[3 * t + 1]),
                        static_cast<uint32_t>(g.triVerts[3 * t + 2])});
  for (size_t r = 0; r < g.runOriginalID.size(); ++r) {
    const auto it = oid_to_part.find(static_cast<int>(g.runOriginalID[r]));
    if (it == oid_to_part.end()) continue;
    for (size_t t = g.runIndex[r] / 3; t < g.runIndex[r + 1] / 3; ++t) out.tri_part[t] = it->second;
  }
  return out;
}

SolidResult finish_solid(Mesh mesh, const CleanupOptions& opt) {
  SolidResult res;
  // Manifold output already shares every vertex it validly can: never weld.
  mesh = cleanup_mesh(mesh, opt, &res.cleanup, WeldOptions{false, 0});
  label_planar_regions(mesh);
  res.mesh = std::move(mesh);
  res.topology = analyze_topology(res.mesh);
  res.planar_regions = res.topology.planar_regions;
  return res;
}

SolidResult assemble_true_solid(const CandidateSet& cs, const std::vector<DepthConstraint>& constraints, Dims d,
                                const CleanupOptions& opt, const std::vector<int>& part_of_candidate) {
  return assemble_parts(true_solid_parts(cs, constraints, d, part_of_candidate), opt);
}

}  // namespace pixel3d
