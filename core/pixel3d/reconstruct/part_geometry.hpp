#pragma once
// The editable form of a model: one closed Manifold solid per part. Parts keep
// their identity through every boolean (Manifold original ids), so triangles of
// the assembled mesh always know their part. Auto parts are 0…n−1 (from
// correspondence); parts created by the user or by edits get ids from 1000.
#include <map>

#include <manifold/manifold.h>

#include "pixel3d/reconstruct/true_solid.hpp"
#include "pixel3d/reconstruct/volume.hpp"

namespace pixel3d {

inline constexpr int kFirstUserPart = 1000;

struct PartGeometry {
  std::map<int, manifold::Manifold> parts;  // id → solid (never empty after normalize())
  void normalize();                          // drops empty parts
  bool has(int id) const { return parts.contains(id); }
  manifold::Manifold all() const;            // union of every part
};

// Union of the parts (each keeps its triangle ids), simplified and cleaned.
SolidResult assemble_parts(const PartGeometry& g, const CleanupOptions& opt);

// TRUE SOLID parts: kept candidates united per part, minus the depth constraints.
PartGeometry true_solid_parts(const CandidateSet& cs, const std::vector<DepthConstraint>& constraints, Dims d,
                              const std::vector<int>& part_of_candidate);
// VOXEL mode: one part (id 0) made of the occupied cells.
PartGeometry voxel_parts(const VoxelGrid& g);

}  // namespace pixel3d
