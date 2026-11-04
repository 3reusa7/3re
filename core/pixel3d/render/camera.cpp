#include "pixel3d/render/camera.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pixel3d {

vec3d Camera::project(vec3d p) const {
  const vec3d d = p - eye;
  return {offset.x + dot(d, right) * scale, offset.y - dot(d, up) * scale, dot(d, forward)};
}

Camera camera_for_view(ViewRole role, Dims d, int ppu) {
  Camera c;
  c.eye = world_from_view(role, d, 0, 0, 0);
  c.right = world_from_view(role, d, 1, 0, 0) - c.eye;
  c.up = -(world_from_view(role, d, 0, 1, 0) - c.eye);
  c.forward = view_dir(role);
  c.scale = ppu;
  c.offset = {0, 0};
  const ImageSize sz = view_image_size(role, d);
  c.width = sz.w * ppu;
  c.height = sz.h * ppu;
  return c;
}

Camera camera_orbit(Dims d, double yaw_deg, double elevation_deg, int width, int height, double margin) {
  const double yaw = yaw_deg * std::numbers::pi / 180.0, el = elevation_deg * std::numbers::pi / 180.0;
  const vec3d centre{d.W / 2.0, d.H / 2.0, d.D / 2.0};
  const vec3d to_cam{std::sin(yaw) * std::cos(el), std::sin(el), std::cos(yaw) * std::cos(el)};
  Camera c;
  c.forward = -to_cam;
  c.right = normalize(cross(c.forward, vec3d{0, 1, 0}));
  c.up = cross(c.right, c.forward);
  const double diag = length(vec3d{double(d.W), double(d.H), double(d.D)});
  c.eye = centre - c.forward * diag;
  // Fit the bounding sphere so every yaw/elevation shares one scale (turntable
  // frames are directly comparable in pixels).
  const double radius = std::max(diag / 2.0, 1e-9);
  c.width = width;
  c.height = height;
  c.scale = std::min(width, height) * (0.5 - margin) / radius;
  c.offset = {width / 2.0, height / 2.0};
  return c;
}

}  // namespace pixel3d
