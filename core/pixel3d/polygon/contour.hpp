#pragma once
// 2D polygons in image coordinates (u right, v down). Rings are closed
// implicitly. Outer rings have positive signed (shoelace) area, holes negative;
// fill rule is NonZero everywhere.
#include <vector>

#include "pixel3d/common/math.hpp"
#include "pixel3d/image/mask.hpp"

namespace pixel3d {

using Ring = std::vector<vec2d>;
using Polygons2D = std::vector<Ring>;

// Exact rectilinear outline of the mask (Clipper2 union of row-run rectangles).
Polygons2D mask_to_polygons(const Mask2D& m);

Polygons2D clip(const Polygons2D& subject, const Polygons2D& clipper);    // intersection
Polygons2D unite(const Polygons2D& a, const Polygons2D& b);               // union
Polygons2D subtract(const Polygons2D& subject, const Polygons2D& clipper);
Polygons2D rect_polys(double u0, double v0, double u1, double v1);
double signed_area(const Ring& r);
double area(const Polygons2D& p);  // sum of signed ring areas (holes subtract)

}  // namespace pixel3d
