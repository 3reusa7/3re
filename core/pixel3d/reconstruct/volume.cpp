#include "pixel3d/reconstruct/volume.hpp"

#include <algorithm>
#include <cmath>

namespace pixel3d {

int VoxelGrid::count() const { return static_cast<int>(std::count(cells_.begin(), cells_.end(), uint8_t{1})); }

std::array<int, 2> cell_pixel(ViewRole role, Dims d, int x, int y, int z) {
  const vec3d uvd = view_from_world(role, d, {x + 0.5, y + 0.5, z + 0.5});
  return {static_cast<int>(std::floor(uvd.x)), static_cast<int>(std::floor(uvd.y))};
}

VoxelGrid visual_hull(const std::map<ViewRole, Mask2D>& masks, Dims d) {
  VoxelGrid g(d);
  for (int z = 0; z < d.D; ++z)
    for (int y = 0; y < d.H; ++y)
      for (int x = 0; x < d.W; ++x) {
        bool in = true;
        for (const auto& [role, m] : masks) {
          const auto [u, v] = cell_pixel(role, d, x, y, z);
          if (!m.get_or_false(u, v)) {
            in = false;
            break;
          }
        }
        g.set(x, y, z, in);
      }
  return g;
}

}  // namespace pixel3d
