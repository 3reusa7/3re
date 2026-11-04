#pragma once
#include "pixel3d/reconstruct/true_solid.hpp"
#include "pixel3d/reconstruct/volume.hpp"

namespace pixel3d {

// VOXEL mode: intentional block output, one cube per occupied cell, merged
// into watertight axis-aligned faces.
SolidResult build_voxel_mode(const VoxelGrid& g, const CleanupOptions& opt);

}  // namespace pixel3d
