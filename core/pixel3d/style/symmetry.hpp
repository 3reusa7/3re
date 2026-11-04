#pragma once
// Left/right mirror symmetry for polished styles (step 5). Only pixel-exact
// mirror images count: when every drawing that shows the X axis (Front, Back,
// Top, Bottom) equals its own mirror image, the styled geometry is built from
// the left half and mirrored, so both sides match exactly. Textures are still
// projected from the drawings afterwards, so asymmetric painted details stay.
#include <map>
#include <string>
#include <vector>

#include "pixel3d/image/mask.hpp"
#include "pixel3d/reconstruct/part_geometry.hpp"
#include "pixel3d/views/view_role.hpp"

namespace pixel3d {

struct SymmetryResult {
  bool symmetric = false;
  double axis_x = 0;                 // world X of the mirror plane
  std::vector<ViewRole> checked;     // the drawings compared with their mirror image
  std::string reason;                // why not (first differing drawing / pixel), or empty
};

SymmetryResult detect_mirror_symmetry(const std::map<ViewRole, Mask2D>& masks, Dims d);

struct MirrorReport {
  int mirrored_pairs = 0;     // parts replaced by the mirror image of their counterpart
  int halved = 0;             // parts crossing the plane, rebuilt from their left half
  std::vector<int> skipped;   // parts left as they were (no exact counterpart)
};

// Right-hand parts become the mirror image of their left counterpart (matched
// by mirrored bounding box), parts crossing the plane are rebuilt from their
// left half. Part ids are kept. Parts without a counterpart stay unchanged.
PartGeometry mirror_parts(const PartGeometry& g, double axis_x, MirrorReport* report = nullptr);

}  // namespace pixel3d
