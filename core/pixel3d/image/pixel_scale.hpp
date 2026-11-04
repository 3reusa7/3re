#pragma once
#include "pixel3d/image/image.hpp"

namespace pixel3d {

// Pixel art exported at k× is made of uniform k×k blocks whose grid starts at
// (ox, oy) modulo k. k == 1 means native resolution.
struct ScaleInfo {
  int k = 1;
  int ox = 0;
  int oy = 0;
  bool operator==(const ScaleInfo&) const = default;
};

// Largest k ≤ max_k consistent with every colour change in the image. Fully
// transparent pixels compare equal regardless of their RGB. Returns k = 1 when
// the image carries no evidence of a block grid.
ScaleInfo detect_pixel_scale(const ImageRGBA& img, int max_k = 16);

// gcd of the spacings between colour-change positions on both axes; 0 when the
// image has fewer than two changes on each axis (no evidence).
int change_spacing_gcd(const ImageRGBA& img);
// Largest divisor of g that is ≤ max_k (1 if g ≤ 1).
int scale_from_gcd(int g, int max_k = 16);
// Grid offsets of `img` for a known block size k.
ScaleInfo scale_with_k(const ImageRGBA& img, int k);

// One output pixel per block. A partial block at the left/top edge (offset > 0)
// becomes its own output pixel, so no source pixel is dropped.
ImageRGBA downscale_nearest(const ImageRGBA& img, const ScaleInfo& s);
// The source pixel downscale_nearest samples for block (bx, by).
int block_source(int block_index, int k, int offset);

}  // namespace pixel3d
