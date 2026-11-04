#pragma once
// Hand-authored pixel art as text. Format:
//   palette:
//   O=#2a1d17        (or #rrggbbaa)
//   ---
//   ..OO..           one character per pixel, '.' = transparent
#include <string_view>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/image/image.hpp"

namespace p3dfix {

pixel3d::Expected<pixel3d::ImageRGBA> image_from_text_map(std::string_view text);

}  // namespace p3dfix
