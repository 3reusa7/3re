#pragma once
#include <array>
#include <map>
#include <vector>

#include "pixel3d/image/mask.hpp"
#include "pixel3d/views/view_role.hpp"

namespace pixel3d {

// Occupancy grid at source-pixel resolution: cell (x, y, z) is the unit cube
// [x,x+1]×[y,y+1]×[z,z+1]. Used for analysis, metrics and VOXEL mode.
class VoxelGrid {
 public:
  VoxelGrid() = default;
  explicit VoxelGrid(Dims d) : d_(d), cells_(static_cast<size_t>(d.W) * d.H * d.D, 0) {}
  Dims dims() const { return d_; }
  bool in_bounds(int x, int y, int z) const { return x >= 0 && y >= 0 && z >= 0 && x < d_.W && y < d_.H && z < d_.D; }
  bool get(int x, int y, int z) const { return cells_[index(x, y, z)] != 0; }
  bool get_or_false(int x, int y, int z) const { return in_bounds(x, y, z) && get(x, y, z); }
  void set(int x, int y, int z, bool v) { cells_[index(x, y, z)] = v ? 1 : 0; }
  int count() const;
  bool operator==(const VoxelGrid&) const = default;

 private:
  size_t index(int x, int y, int z) const { return (static_cast<size_t>(z) * d_.H + y) * d_.W + x; }
  Dims d_;
  std::vector<uint8_t> cells_;
};

// Pixel (u, v) of `role` that the cell centre projects to.
std::array<int, 2> cell_pixel(ViewRole role, Dims d, int x, int y, int z);

// Cells whose centre projects onto an opaque pixel in every view.
VoxelGrid visual_hull(const std::map<ViewRole, Mask2D>& masks, Dims d);

}  // namespace pixel3d
