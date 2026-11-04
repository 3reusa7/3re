#pragma once
// Analytic stylised characters for LOW-POLY STUDIO tests (fixture E): flat
// colour fills, decals (eyes, collar, hair) and a 1 px ink line on the
// silhouette and between parts, like cel-shaded 2D character art. Each pixel
// also records which ground-truth part it shows, so tests get exact part masks
// (what a user would paint on the drawing). Test-only; the core never sees it.
#include <string>
#include <vector>

#include "pixel3d/common/math.hpp"
#include "pixel3d/image/image.hpp"
#include "pixel3d/views/view_role.hpp"

namespace p3dfix {

struct CharPart {
  int id = 0;           // 1000, 1001, … (user part ids)
  std::string name;
  std::string type;     // head | torso | limb | accessory | cloth
  pixel3d::Rgba8 key;   // colour of this part in the part-id maps
};

struct CharView {
  pixel3d::ImageRGBA art;    // the drawing
  pixel3d::ImageRGBA parts;  // part-id map: CharPart::key per visible pixel, transparent elsewhere
};

// Ground truth of one part, measured by sampling the analytic volume (world px).
struct PartExtent {
  int id = 0;
  pixel3d::vec3d lo, hi;  // bounding box of the part's own volume (overlaps included)
  double volume = 0;      // px^3, points owned by this part (first-wins priority)
};

struct CharFixture {
  std::string name;
  pixel3d::Dims dims;  // tight: the drawing touches every side of the box
  std::vector<CharPart> parts;
  CharView front, side;
  CharView top;                     // only for 3-view references (empty otherwise)
  std::vector<PartExtent> extents;  // only for 3D references (empty otherwise)
};

CharFixture make_cloaked_character();   // Hollow Knight-like: horned mask head, cloak body, two short legs
CharFixture make_humanoid_character();  // A-pose: head, torso, two-segment arms and legs
// 1.2.0 input B: a controlled 3D reference of a hooded character (hood with a face opening,
// hair fringe, scarf with a front tail, torso, arms touching the body, hands, belt and pouch,
// legs, boots) with consistent orthographic Front / Side / Top and per-part ground truth.
CharFixture make_hooded_reference();

}  // namespace p3dfix
