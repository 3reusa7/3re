#pragma once
// Brush strokes on a view, in aligned pixels. The GUI paints with the mouse,
// MCP clients pass the stroke points; both turn them into the same pixels and
// offsets here, so a stroke means the same thing from either client.
#include <vector>

#include "pixel3d/common/math.hpp"
#include "pixel3d/edit/edit_ops.hpp"
#include "pixel3d/image/mask.hpp"

namespace pixel3d {

struct BrushParams {
  double radius = 1.5;    // pixels (a radius < 1 paints single pixels)
  double strength = 1.0;  // depth brush: offset in pixels at full weight
  double hardness = 0.5;  // 0 soft … 1 hard edge
  bool raise = true;      // depth brush: toward the camera (true) or into the surface
  double quantum = 1.0;   // depth snapping step (0 = no snapping)
};

// Pixels a stroke touches with their weight in (0, 1] (max over the dabs).
// Points are pixel coordinates (centres at x + 0.5); dabs are interpolated so
// fast strokes have no gaps.
std::vector<std::pair<std::pair<int, int>, double>> brush_weights(const std::vector<vec2d>& points, const BrushParams& p,
                                                                   int width, int height);

// Depth Brush samples: offset = ±strength × weight, snapped to `quantum`; zero offsets are dropped.
std::vector<DepthSample> depth_brush_samples(const std::vector<vec2d>& points, const BrushParams& p, int width, int height);

// Mask brush: every touched pixel (weight > 0), as a mask.
Mask2D mask_brush(const std::vector<vec2d>& points, const BrushParams& p, int width, int height);

}  // namespace pixel3d
