#pragma once
// Rounded volumes inside the visual hull (polished styles, step 2).
//
// A part is rounded through its three axis projections: each projection's
// convex corners are replaced by a superellipse arc (exponent 2 = circular,
// 1 = chamfer, > 2 = squarer), the rounded outlines are extruded along their
// axis and intersected with the part. The result is always inside the part.
//
// Silhouettes stay pixel-exact by construction: every corner's size is clamped
// up front so that no source pixel centre the outline covered is uncovered
// (pixel-centre sampling, see rasterize_pixel_centres). Corners of features at
// most `thin_px` wide are not rounded, so 1–2 px parts keep their drawn shape
// (step 3 may still give them a small `bevel`, pixel-safe like the rest). Putting
// back whole pixel squares is only a last-resort guarantee, and it is reported.
#include <manifold/manifold.h>

#include <vector>

#include "pixel3d/polygon/contour.hpp"
#include "pixel3d/reconstruct/part_geometry.hpp"

namespace pixel3d {

struct CornerOptions {
  double radius = 1.5;    // requested corner radius in source px (a 90° corner's arc radius); upper bound
  double exponent = 2;    // superellipse exponent: 2 round, 1 chamfer, > 2 squarer (1–8)
  double thin_px = 2.0;   // corners of features at most this wide (px) are not rounded
  double bevel = 0;       // step 3: radius (px) of the small bevel every other convex corner gets (0 = sharp)
  double margin = 0.06;   // px a kept pixel centre must stay inside the new outline
  int segments = 6;       // straight pieces per rounded corner (chamfer: 1)
};

struct CornerReport {
  int corners = 0;          // convex corners seen
  int rounded = 0;          // corners that got an arc
  int clamped = 0;          // rounded, but smaller than requested (pixel centres or edge lengths)
  int thin = 0;             // not rounded because the feature there is thin
  int beveled = 0;          // thin corners that got the small bevel instead
  int joined = 0;           // left as they are: the outline continues into a touching part
  int restored_pixels = 0;  // last resort: pixel squares put back (expected 0)
  double max_size = 0;      // largest tangent length used (px)
  CornerReport& operator+=(const CornerReport& o);
};

// Rounds the convex corners of `p` (positive-area outer rings, NonZero fill).
// Guarantees: the result is inside `p`, and every pixel centre inside `p` is
// inside the result (rasterize_pixel_centres is unchanged).
// `joined` (optional): the outline of this shape together with the shapes that
// touch it. A corner where the outline continues into that union (a post split
// into stacked parts) is left as it is, so joints get no groove.
Polygons2D round_corners(const Polygons2D& p, const CornerOptions& opt, CornerReport* report = nullptr,
                         const Polygons2D* joined = nullptr);

struct PartRoundReport {
  int part = 0;
  CornerReport corners;        // summed over the three projections
  bool fallback = false;       // the part kept its unrounded shape
  std::string reason;          // why it fell back
};

// One part through its Front (along Z), Side (along X) and Top (along Y)
// projections. The result must be a valid closed solid inside `part` that keeps
// each of its own projections' pixel centres; otherwise `part` comes back
// unchanged and the report says why.
manifold::Manifold round_part(const manifold::Manifold& part, const CornerOptions& opt, PartRoundReport* report = nullptr);

// Every part of `g` (reports in part id order). Corners where a part continues
// into a part it touches are not rounded (no grooves at joints). Silhouettes are checked on the
// whole model in all three axes: a part whose rounding uncovers a pixel centre
// of the model goes back to its crisp shape (fallback, with the reason).
PartGeometry round_parts(const PartGeometry& g, const CornerOptions& opt, std::vector<PartRoundReport>* reports = nullptr);

}  // namespace pixel3d
