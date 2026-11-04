#pragma once
#include "pixel3d/image/mask.hpp"
#include "pixel3d/polygon/contour.hpp"

namespace pixel3d {

struct SimplifyOptions {
  bool angle_snap = true;  // regular 1:k staircases get exactly slope 1/k
  int min_unit_steps = 2;  // a staircase needs at least this many unit steps
};

struct SimplifyStats {
  int staircases_replaced = 0;
  int reverted = 0;            // candidates rejected by the deviation or raster check
  double max_deviation = 0;    // max distance of a removed corner from its replacement line
};

// Feature-preserving simplification of exact rectilinear pixel outlines:
// regular staircases (unit steps alternating with runs in {k, k+1}) become one
// straight segment on the staircase mid-line; straight edges, corners and
// notches are kept exactly. Guarantee: rasterize_pixel_centres(result) equals
// `source` exactly and no removed corner is farther than 0.5 px from its
// replacement; any staircase violating either is reverted.
Polygons2D simplify_feature_preserving(const Polygons2D& exact, const Mask2D& source, const SimplifyOptions& opt,
                                       SimplifyStats* stats = nullptr);

}  // namespace pixel3d
