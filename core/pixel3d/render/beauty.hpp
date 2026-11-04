#pragma once
// Presentation renderer ("beauty" style): perspective camera, studio three-point
// lighting, soft shadows on a ground plane, supersampled anti-aliasing. It only
// makes pictures for people. Validation never uses it: every check renders with
// the deterministic rasterizer in rasterizer.hpp, whose output this file never
// touches.
#include <optional>
#include <string_view>

#include "pixel3d/common/math.hpp"
#include "pixel3d/image/image.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

enum class RenderStyle { Validation, Beauty };
std::string_view to_string(RenderStyle s);
std::optional<RenderStyle> render_style_from_string(std::string_view s);

struct BeautyOptions {
  int width = 512, height = 512;
  int supersample = 3;        // samples per axis per output pixel (1–4)
  double fov_deg = 28;        // vertical field of view
  bool shadows = true;        // key-light shadow on the model and the ground
  bool ground = true;         // ground plane with contact shadow
  bool transparent = false;   // background alpha 0; the ground keeps only its shadow
};

// Orbit camera around the model's bounding box: yaw 0 = from the front (+Z),
// 90 = from the right (+X); elevation > 0 looks down (same angles as
// camera_orbit). Lights follow the camera, so every turntable frame is lit alike.
ImageRGBA render_beauty(const TexturedMesh& m, double yaw_deg, double elevation_deg, const BeautyOptions& opt = {});

}  // namespace pixel3d
