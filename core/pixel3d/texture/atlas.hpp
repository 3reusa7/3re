#pragma once
// Texture atlas: every aligned source view is copied untouched into its own
// cell (exact palette, no resampling). Opaque colours are dilated a few pixels
// into transparency in the atlas copy only, so nearest sampling at a silhouette
// edge never picks up transparency. One flat swatch per material family serves
// the Flat Palette Color mode.
#include <map>
#include <memory>
#include <vector>

#include "pixel3d/image/palette.hpp"
#include "pixel3d/views/source_view.hpp"

namespace pixel3d {

struct AtlasCell {
  ViewRole role = ViewRole::Front;
  int x0 = 0, y0 = 0, w = 0, h = 0;
};

struct Atlas {
  std::shared_ptr<const ImageRGBA> image;
  std::vector<AtlasCell> cells;
  std::map<int, vec2d> swatch_uv;  // family → UV of the swatch centre
  const AtlasCell* cell(ViewRole r) const;
  // UV of image point (u, v) of view r (pixel units, not texel centres); (0, 0) if r has no cell.
  vec2d uv(ViewRole r, double u, double v) const;
};

// Pixels with alpha below `alpha_threshold` (the project setting) are transparent.
Atlas build_atlas(const std::vector<SourceView>& views, const MaterialFamilies& fam, int gutter = 2, int dilate = 2,
                  uint8_t alpha_threshold = 128);

}  // namespace pixel3d
