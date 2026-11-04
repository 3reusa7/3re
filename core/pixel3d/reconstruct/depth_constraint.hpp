#pragma once
#include <string>
#include <vector>

#include <manifold/manifold.h>

#include "pixel3d/polygon/contour.hpp"
#include "pixel3d/reconstruct/volume.hpp"

namespace pixel3d {

// Inside `region` (image coordinates of `view`), only depths in
// [depth_from, depth_to] along the view direction may hold material; the rest
// of that column is carved. Depth is measured from the camera-side box face.
struct DepthConstraint {
  ViewRole view = ViewRole::Top;
  Polygons2D region;
  double depth_from = 0;
  double depth_to = 1e9;
  std::string label;
};

// Material removed by the constraint (empty when it removes nothing).
manifold::Manifold carve_volume(const DepthConstraint& c, Dims d);

VoxelGrid apply_depth_constraints(VoxelGrid g, const std::vector<DepthConstraint>& cs, Dims d);

}  // namespace pixel3d
