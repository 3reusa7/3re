#pragma once
// Orthographic view conventions (spec §4). World: +Y up, the object's front
// faces +Z, 1 source pixel = 1 unit, bounding box [0,W]×[0,H]×[0,D].
// Image pixel (u, v) covers [u,u+1]×[v,v+1] with v pointing down.
// `depth` is the distance along view_dir() from the camera-side face of the box.
#include <optional>
#include <string_view>

#include "pixel3d/common/math.hpp"

namespace pixel3d {

enum class ViewRole { Front, Back, Side, Right, Top, Bottom };  // Side == left view

std::string_view to_string(ViewRole r);
std::optional<ViewRole> view_role_from_string(std::string_view s);

struct Dims {
  int W = 0, H = 0, D = 0;
  bool operator==(const Dims&) const = default;
};

struct ImageSize {
  int w = 0, h = 0;
};

// Front/Back: W×H, Side/Right: D×H, Top/Bottom: W×D.
ImageSize view_image_size(ViewRole r, Dims d);
// Direction the camera looks (Front: (0,0,-1)).
vec3d view_dir(ViewRole r);
vec3d world_from_view(ViewRole r, Dims d, double u, double v, double depth);
// Returns (u, v, depth).
vec3d view_from_world(ViewRole r, Dims d, vec3d p);
// Extent of the box along the view direction.
double view_depth_extent(ViewRole r, Dims d);

constexpr ViewRole kAllViewRoles[] = {ViewRole::Front, ViewRole::Back, ViewRole::Side,
                                      ViewRole::Right, ViewRole::Top,  ViewRole::Bottom};
constexpr ViewRole kRequiredViewRoles[] = {ViewRole::Front, ViewRole::Side, ViewRole::Top};

}  // namespace pixel3d
