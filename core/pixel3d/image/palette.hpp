#pragma once
// Palette analysis: groups the exact source colours into material families.
// A family is a colour ramp (shades of one hue), so lighting/shading never
// splits a part. Dark, desaturated 1-px line colours form the outline family,
// which separates regions but is never a part by itself.
#include <span>
#include <unordered_map>
#include <vector>

#include "pixel3d/image/image.hpp"

namespace pixel3d {

struct PaletteEntry {
  Rgba8 color;  // alpha forced to 255; palette identity is RGB
  int count = 0;
  int family = -1;
};

struct MaterialFamilies {
  std::vector<PaletteEntry> entries;
  int family_count = 0;
  int outline_family = -1;  // -1 if the art has no outline colour

  // -1 for transparent pixels and colours not present in the palette.
  int family_of(Rgba8 c, uint8_t alpha_threshold = 128) const;
  // Index into `entries`, or -1.
  int entry_of(Rgba8 c) const;

  std::unordered_map<uint32_t, int> index;  // packed RGB → entry index
};

MaterialFamilies analyze_palette(std::span<const ImageRGBA* const> views, uint8_t alpha_threshold = 128);

double relative_luminance(Rgba8 c);  // sRGB → linear, Rec.709 weights, [0, 1]

struct OkLch {
  double L = 0, C = 0, h = 0;  // h in degrees [0, 360)
};
OkLch to_oklch(Rgba8 c);

}  // namespace pixel3d
