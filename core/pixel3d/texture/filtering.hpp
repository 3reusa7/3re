#pragma once
// Texture prepared for filtered sampling (polished styles, step 6). The pixel
// atlas is made for NEAREST sampling without mipmaps; engines that build
// mipmaps or filter at a distance would average transparent texels and other
// islands into edges. pad_for_filtering fills every texel no colour was given
// (nearest opaque colour, breadth first; texels that already had a colour keep
// it) and scales the atlas up by an integer factor with nearest sampling, so
// each island gets scale × (gutter + dilation) texels of padding. UVs stay
// valid and every sampled colour is unchanged.
#include "pixel3d/texture/projector.hpp"

namespace pixel3d {

TextureResult pad_for_filtering(const TextureResult& tex, int scale = 8);

}  // namespace pixel3d
