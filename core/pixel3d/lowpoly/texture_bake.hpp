#pragma once
// Texture for the low-poly mesh: per-part UV islands packed into one atlas at a
// single (balanced) texel density, then every texel baked from the drawings.
//
// One source view per texel (the most frontal view that sees the point, depth
// tested against the whole mesh) — no colour averaging. The drawing's
// silhouette ink line is a 2D artefact (in 3D the outline style draws it):
// outline-colour pixels within `ink_band` px of the silhouette are replaced by
// the nearest inner colour, and counted. Texels no view sees are INFERRED:
// Opposite Projection (limb/torso/accessory only, unless enabled for head and
// cloth — the back of a head would show the face), then Edge Extend (nearest
// seen non-ink texel of the same island), then Flat Palette (the part's
// dominant colour). No new artwork is painted.
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pixel3d/image/palette.hpp"
#include "pixel3d/lowpoly/assemble.hpp"
#include "pixel3d/lowpoly/layout.hpp"

namespace pixel3d::lowpoly {

struct PackResult {
  bool ok = false;
  int width = 0, height = 0;
  double density = 0;  // texels per world unit (source pixel)
  int gutter = 4;
};
// Places every island (Island::x0 …) and turns the mesh UVs into atlas UVs.
PackResult pack_islands(Assembly& as, int max_side, double max_density = 4.0, int gutter = 4);

enum class TexelMode : uint8_t { None, Seen, Opposite, EdgeExtend, Flat };

struct BakeOptions {
  uint8_t alpha_threshold = 128;
  bool opposite_head_cloth = false;
  bool baked_cel = false;
  int tones = 3;
  vec3d light{-0.4, 0.8, 0.45};  // the standard light for baked_cel (world, normalised when used)
  int ink_band = 2;
};

struct BakeResult {
  std::shared_ptr<const ImageRGBA> texture;
  std::map<std::string, double> share;  // seen:<view>, opposite, edge_extend, flat_palette (share of island texels)
  int ink_band_px = 0;                  // drawing pixels treated as silhouette ink
  std::vector<Message> messages;
  std::vector<TexelMode> mode;          // per texel
};

BakeResult bake_texture(const Assembly& as, const PackResult& pk, const Input& in, const Layout& l,
                        const MaterialFamilies& fam, const BakeOptions& o);

// The drawing with its contour ink replaced by the nearest inner colour of the
// same part: outline-colour pixels within `band` px of the silhouette, and (with
// part labels: part id per pixel, −1 none) outline-colour pixels touching
// another part — where one part occludes another, the line is a contour the 3D
// outline draws. Ink inside a part (eyes, folds) stays.
ImageRGBA remove_silhouette_ink(const ImageRGBA& img, const MaterialFamilies& fam, uint8_t alpha_threshold, int band,
                                int* replaced, const std::vector<int>* part_labels = nullptr);

}  // namespace pixel3d::lowpoly
