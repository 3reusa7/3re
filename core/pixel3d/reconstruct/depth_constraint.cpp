#include "pixel3d/reconstruct/depth_constraint.hpp"

#include "pixel3d/polygon/raster.hpp"
#include "pixel3d/reconstruct/prism.hpp"

namespace pixel3d {

manifold::Manifold carve_volume(const DepthConstraint& c, Dims d) {
  const double extent = view_depth_extent(c.view, d);
  manifold::Manifold carve;
  if (c.depth_from > -1.0) carve = carve + make_prism_range(c.view, d, c.region, -1.0, c.depth_from);
  if (c.depth_to < extent + 1.0) carve = carve + make_prism_range(c.view, d, c.region, c.depth_to, extent + 1.0);
  return carve;
}

VoxelGrid apply_depth_constraints(VoxelGrid g, const std::vector<DepthConstraint>& cs, Dims d) {
  for (const DepthConstraint& c : cs) {
    const ImageSize sz = view_image_size(c.view, d);
    const Mask2D inside = rasterize_pixel_centres(c.region, sz.w, sz.h);
    for (int z = 0; z < d.D; ++z)
      for (int y = 0; y < d.H; ++y)
        for (int x = 0; x < d.W; ++x) {
          if (!g.get(x, y, z)) continue;
          const vec3d uvd = view_from_world(c.view, d, {x + 0.5, y + 0.5, z + 0.5});
          const auto [u, v] = cell_pixel(c.view, d, x, y, z);
          if (inside.get_or_false(u, v) && (uvd.z < c.depth_from || uvd.z > c.depth_to)) g.set(x, y, z, false);
        }
  }
  return g;
}

}  // namespace pixel3d
