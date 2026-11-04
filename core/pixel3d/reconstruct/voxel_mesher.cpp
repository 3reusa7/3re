#include "pixel3d/reconstruct/voxel_mesher.hpp"

#include "pixel3d/reconstruct/part_geometry.hpp"

namespace pixel3d {

SolidResult build_voxel_mode(const VoxelGrid& g, const CleanupOptions& opt) {
  return assemble_parts(voxel_parts(g), opt);
}

}  // namespace pixel3d
