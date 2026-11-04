#pragma once
#include <memory>
#include <string>
#include <vector>

#include "pixel3d/image/image.hpp"
#include "pixel3d/image/mask.hpp"
#include "pixel3d/image/pixel_scale.hpp"
#include "pixel3d/views/view_role.hpp"

namespace pixel3d {

// What input normalisation did to one view (image/normalize.hpp).
struct ViewNormalization {
  bool applied = false;             // `working` replaced the original for alignment
  bool background_removed = false;
  Rgba8 background{};
  double background_share = 0;      // removed pixels / all pixels
  int alpha_snapped = 0;            // pixels whose partial alpha was snapped to 0 or 255
  int colours_before = 0, colours_after = 0;
  double block = 1;                 // source pixels per art pixel (exact grid or estimated)
  bool block_exact = true;
  double factor = 1;                // source pixels per working pixel (block × rescale × fit)
};

// One supplied drawing. `original` is never modified; `aligned` is the same
// artwork at native pixel scale, cropped/flipped/padded to exactly
// view_image_size(role, dims). Every aligned pixel has a colour of the source
// image (copied, or the dominant colour of its block after normalisation).
struct SourceView {
  ViewRole role = ViewRole::Front;
  std::string original_path;
  std::string sha256;
  std::shared_ptr<const ImageRGBA> original;
  std::shared_ptr<const std::vector<uint8_t>> original_bytes;  // the file exactly as imported
  std::shared_ptr<const ImageRGBA> working;  // normalised input (derived, never stored); null = the original
  ViewNormalization norm;
  std::shared_ptr<const ImageRGBA> aligned;
  ScaleInfo scale;
  Mask2D::Box crop;          // in downscaled coordinates
  int pad_x = 0, pad_y = 0;  // transparent pixels added before the cropped art (aligned = pad + art)
  bool flipped_x = false;
  bool flip_user_override = false;
};

}  // namespace pixel3d
