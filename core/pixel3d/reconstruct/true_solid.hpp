#pragma once
#include <map>
#include <vector>

#include <manifold/manifold.h>

#include "pixel3d/correspond/candidates.hpp"
#include "pixel3d/mesh/cleanup.hpp"
#include "pixel3d/mesh/topology.hpp"
#include "pixel3d/reconstruct/depth_constraint.hpp"

namespace pixel3d {

struct SolidResult {
  Mesh mesh;
  TopologyReport topology;
  CleanupReport cleanup;
  int planar_regions = 0;
};

// Union of the kept candidates (grouped per part so each part keeps its own
// triangle run), minus the depth constraints' carve volumes, simplified and
// cleaned. tri_part comes from `part_of_candidate`.
SolidResult assemble_true_solid(const CandidateSet& cs, const std::vector<DepthConstraint>& constraints, Dims d,
                                const CleanupOptions& opt, const std::vector<int>& part_of_candidate = {});

// Converts a Manifold; triangles of original id `oid` get part oid_to_part[oid] (-1 otherwise).
Mesh mesh_from_manifold(const manifold::Manifold& m, const std::map<int, int>& oid_to_part);

// Cleanup (without vertex welding) + planar region labels + topology for a Manifold-built mesh.
SolidResult finish_solid(Mesh mesh, const CleanupOptions& opt);

}  // namespace pixel3d
