#pragma once
#include "pixel3d/image/mask.hpp"
#include "pixel3d/polygon/contour.hpp"

namespace pixel3d {

// Pixel (u, v) is set iff its centre (u+0.5, v+0.5) has nonzero winding.
// This is the sampling rule every silhouette guarantee in Pixel3D refers to.
Mask2D rasterize_pixel_centres(const Polygons2D& p, int w, int h);

}  // namespace pixel3d
