#pragma once
#include "pixel3d/common/math.hpp"
#include "pixel3d/views/view_role.hpp"

namespace pixel3d {

// Orthographic camera. Screen x grows right, y grows down; depth grows along
// `forward`. Pixel (i, j) samples the screen point (i + 0.5, j + 0.5).
struct Camera {
  vec3d eye, right, up, forward;  // orthonormal frame
  double scale = 1;               // pixels per world unit
  vec2d offset;                   // screen position of `eye`
  int width = 0, height = 0;
  vec3d project(vec3d p) const;   // (sx, sy, depth)
};

// Camera reproducing a source view exactly: at pixels_per_unit = 1 every
// screen pixel centre coincides with the source pixel centre (spec §4).
Camera camera_for_view(ViewRole role, Dims d, int pixels_per_unit = 1);

// Orbit around the box centre. yaw 0 = from +Z (front), 90 = from +X (right);
// elevation > 0 looks down. The box's bounding sphere fits the image with
// `margin` on each side, so the scale is the same for every angle.
Camera camera_orbit(Dims d, double yaw_deg, double elevation_deg, int width, int height, double margin = 0.08);

}  // namespace pixel3d
