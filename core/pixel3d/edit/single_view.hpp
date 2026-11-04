#pragma once
// SINGLE VIEW mode (spec M4 §5): one drawing, depth by preset or number. The
// silhouette is exact; its depth and every hidden surface are INFERRED — a
// single drawing determines neither, and nothing unseen is invented.
#include <string>

#include <nlohmann/json.hpp>

#include "pixel3d/edit/part_masks.hpp"
#include "pixel3d/reconstruct/volume.hpp"

namespace pixel3d {

struct SingleViewSettings {
  bool enabled = false;
  ViewRole role = ViewRole::Front;
  std::string preset = "medium";  // flat | thin | medium | thick | custom
  double depth = 0;               // custom depth in source pixels
  bool user_depth = false;        // the depth was chosen explicitly (not the default)
  bool operator==(const SingleViewSettings&) const = default;
};

// Depth in source pixels for a drawing of w × h pixels (after crop/scale).
double single_view_depth(const SingleViewSettings& s, int w, int h);
bool valid_depth_preset(const std::string& p);

nlohmann::json single_view_to_json(const SingleViewSettings& s);
Expected<SingleViewSettings> single_view_from_json(const nlohmann::json& j);

// Part 0 = the silhouette swept over `depth`, centred in the box; user parts
// with masks on the drawing get their own depth (UserPart::depth) instead.
PartGeometry single_view_parts(const SourceView& v, Dims d, double depth, const PartMasks& masks,
                               const SimplifyOptions& opt, uint8_t alpha_threshold);
// The same geometry as analysis cells (for recess analysis and metrics).
VoxelGrid single_view_volume(const SourceView& v, Dims d, double depth, const PartMasks& masks, uint8_t alpha_threshold);

}  // namespace pixel3d
