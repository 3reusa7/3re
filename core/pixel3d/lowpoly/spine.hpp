#pragma once
// Spines of parts in the Front drawing (image coordinates: u right, v down).
// A spine is a polyline resampled every `step` pixels of arc length; each point
// carries the unit cross-line direction n = (−t.y, t.x) of its tangent t, so a
// spine running up the image has n = +u (world +X).
#include <vector>

#include "pixel3d/common/math.hpp"
#include "pixel3d/image/mask.hpp"
#include "pixel3d/polygon/contour.hpp"

namespace pixel3d::lowpoly {

struct Spine {
  std::vector<vec2d> pts;
  std::vector<vec2d> normals;
  double step = 0.5;
  bool straight = false;  // cross-sections take the outer extent of the outline
  double length() const;
  vec2d tangent(size_t i) const;  // unit
  // Prepends `dist` pixels before the first point along the reversed start tangent.
  void extend_start(double dist);
  void reverse();
};

// Exact pixel outline with stair noise removed (the existing feature-preserving simplifier).
Polygons2D part_outline(const Mask2D& m);

// Straight spine with unit direction `dir` through the mask's centroid, covering
// the mask's extent along `dir`.
Spine straight_spine(const Mask2D& m, vec2d dir);

// Longest path of the mask's medial skeleton (Zhang–Suen thinning), smoothed,
// extended along its end tangents to the outline, oriented from the end nearest
// `start_hint` (or its first skeleton end when the hint is NaN).
Spine medial_spine(const Mask2D& m, vec2d start_hint = {std::nan(""), std::nan("")});

// Principal axis of the mask pixels: unit direction (pointing up the image, v < 0)
// and elongation = sqrt(λ1 / λ2) (1 for a disk).
struct Axis {
  vec2d dir{0, -1};
  double elongation = 1;
  vec2d centroid;
};
Axis principal_axis(const Mask2D& m);

}  // namespace pixel3d::lowpoly
