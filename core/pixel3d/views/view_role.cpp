#include "pixel3d/views/view_role.hpp"

namespace pixel3d {

std::string_view to_string(ViewRole r) {
  switch (r) {
    case ViewRole::Front: return "front";
    case ViewRole::Back: return "back";
    case ViewRole::Side: return "side";
    case ViewRole::Right: return "right";
    case ViewRole::Top: return "top";
    case ViewRole::Bottom: return "bottom";
  }
  return "front";
}

std::optional<ViewRole> view_role_from_string(std::string_view s) {
  for (ViewRole r : kAllViewRoles)
    if (to_string(r) == s) return r;
  if (s == "left") return ViewRole::Side;
  return std::nullopt;
}

ImageSize view_image_size(ViewRole r, Dims d) {
  switch (r) {
    case ViewRole::Front:
    case ViewRole::Back: return {d.W, d.H};
    case ViewRole::Side:
    case ViewRole::Right: return {d.D, d.H};
    case ViewRole::Top:
    case ViewRole::Bottom: return {d.W, d.D};
  }
  return {};
}

vec3d view_dir(ViewRole r) {
  switch (r) {
    case ViewRole::Front: return {0, 0, -1};
    case ViewRole::Back: return {0, 0, 1};
    case ViewRole::Side: return {1, 0, 0};
    case ViewRole::Right: return {-1, 0, 0};
    case ViewRole::Top: return {0, -1, 0};
    case ViewRole::Bottom: return {0, 1, 0};
  }
  return {};
}

double view_depth_extent(ViewRole r, Dims d) {
  switch (r) {
    case ViewRole::Front:
    case ViewRole::Back: return d.D;
    case ViewRole::Side:
    case ViewRole::Right: return d.W;
    case ViewRole::Top:
    case ViewRole::Bottom: return d.H;
  }
  return 0;
}

vec3d world_from_view(ViewRole r, Dims d, double u, double v, double depth) {
  switch (r) {
    case ViewRole::Front: return {u, d.H - v, d.D - depth};
    case ViewRole::Back: return {d.W - u, d.H - v, depth};
    case ViewRole::Side: return {depth, d.H - v, u};
    case ViewRole::Right: return {d.W - depth, d.H - v, d.D - u};
    case ViewRole::Top: return {u, d.H - depth, v};
    case ViewRole::Bottom: return {u, depth, d.D - v};
  }
  return {};
}

vec3d view_from_world(ViewRole r, Dims d, vec3d p) {
  switch (r) {
    case ViewRole::Front: return {p.x, d.H - p.y, d.D - p.z};
    case ViewRole::Back: return {d.W - p.x, d.H - p.y, p.z};
    case ViewRole::Side: return {p.z, d.H - p.y, p.x};
    case ViewRole::Right: return {d.D - p.z, d.H - p.y, d.W - p.x};
    case ViewRole::Top: return {p.x, p.z, d.H - p.y};
    case ViewRole::Bottom: return {p.x, d.D - p.z, p.y};
  }
  return {};
}

}  // namespace pixel3d
