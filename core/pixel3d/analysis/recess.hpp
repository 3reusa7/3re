#pragma once
// Enclosed-region analysis (spec §6.2): a colour region drawn inside a rim,
// over which the reconstructed surface is flat and flush with the rim, has a
// depth no silhouette determined. Darker than its rim → possible_recess
// (AMBIGUOUS); otherwise inferred_flush (painted detail, INFO). Geometry is
// never carved automatically.
#include <map>
#include <string>
#include <vector>

#include "pixel3d/reconstruct/volume.hpp"
#include "pixel3d/segment/regions.hpp"

namespace pixel3d {

struct RecessFinding {
  ViewRole view = ViewRole::Top;
  int region = -1;
  std::string kind;              // possible_recess | inferred_flush
  double area = 0;               // pixels, including the region's own outline ring
  double luminance_ratio = 1;    // region / surrounding regions
  double column_volume = 0;      // solid directly under the region (upper bound of a hidden hollow)
  Mask2D::Box box;               // pixels in the view's image
};

std::vector<RecessFinding> analyze_recesses(const std::map<ViewRole, RegionMap>& regions, const VoxelGrid& kept, Dims d,
                                            double recess_luminance_delta = 0.12);

}  // namespace pixel3d
