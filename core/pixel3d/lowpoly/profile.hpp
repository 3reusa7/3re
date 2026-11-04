#pragma once
// Cross-section profile along a spine: width from the Front outline, depth from
// the Side (or Top) mask when there is one — measured — or from silhouette
// inflation otherwise — INFERRED. Inflation follows sketch-inflation work
// (Teddy, Monster Mash): at distance-transform value r from the outline of a
// section of half width R the surface of a round body stands sqrt(r(2R − r))
// off the drawing plane, so free ends dome and the middle is round.
#include <string>
#include <vector>

#include "pixel3d/image/mask.hpp"
#include "pixel3d/lowpoly/spine.hpp"
#include "pixel3d/lowpoly/types.hpp"
#include "pixel3d/polygon/contour.hpp"

namespace pixel3d::lowpoly {

// Euclidean distance (pixels) from each inside pixel centre to the outline
// (the nearest outside pixel centre minus half a pixel); 0 outside.
struct DistanceField {
  int w = 0, h = 0;
  std::vector<float> d;
  float at(int x, int y) const { return (x < 0 || y < 0 || x >= w || y >= h) ? 0.f : d[static_cast<size_t>(y) * w + x]; }
  double sample(vec2d p) const;  // bilinear between pixel centres
  float max() const;
};
DistanceField distance_transform(const Mask2D& m);

struct Station {
  double s = 0;      // spine arc length
  vec2d centre;      // image coordinates: middle of the cross-line extent
  vec2d n;           // cross-line direction (image coordinates, unit)
  double half_w = 0; // half width along n
  double z_c = 0, half_d = 0;  // world depth centre and half depth
  bool measured = false;       // depth from a drawing (Side/Top), not inferred
  int owner = -1;              // part id (filled by the layout)
};

// Cross-line extents at every spine point. `outer_extent`: the full extent of
// every outline crossing on the line (straight spines); otherwise the nearest
// crossing on each side of the spine point (medial spines).
std::vector<Station> measure_widths(const Spine& sp, const Polygons2D& outline, bool outer_extent);

struct DepthOptions {
  double box_depth = 0;          // D of the model box (world units)
  const Mask2D* side = nullptr;  // the part's Side mask (aligned Side pixels; u = world z)
  const Mask2D* top = nullptr;   // the part's Top mask (aligned Top pixels; v = world z)
  double roundness = 1.0;        // inflation: half depth = roundness · dome height
  double min_ratio = 0;          // inflation: half depth ≥ min_ratio · half width
  // Per station (same order as the stations; empty = the scalars above): parts of one
  // tube (the trunk's body and head) keep their own type's shape.
  std::vector<double> roundness_per_station, min_ratio_per_station;
  double depth_limit = 0;        // total depth limit (0 = the box depth)
  const DepthCue* cue = nullptr; // Smart Pipeline hook (b)
  double min_half_depth = 0.5;
};

// Fills z_c / half_d / measured. Inflated half depth = max(roundness · dome · cue,
// min_ratio · half width), clamped to the depth limit. Returns the depth source: "side", "top",
// "inflation", or "side+inflation" / "top+inflation" when some stations had no
// drawing pixels in their rows (or columns).
std::string measure_depth(std::vector<Station>& st, const Spine& sp, const DistanceField& dt, const DepthOptions& o);

// Greedy station selection (the "simplify to a vertex budget" step on the 1D
// profile): starting from the ends and `fixed`, repeatedly insert the station
// whose cross-section (left/right outline points, depth bounds) deviates most
// from linear interpolation between its kept neighbours. Never decimates a
// mesh: it only chooses where the rings go.
struct Reduction {
  std::vector<int> initial;     // ends + fixed, sorted
  std::vector<int> order;       // insertion order of the other stations
  std::vector<double> errors;   // max deviation (px) of the refinable stations after inserting 0, 1, … of `order`
};
// `floor` (per station, optional): deviation below it is not worth a ring — the
// stop threshold that keeps spare triangles away from pixel noise and flat cloth.
Reduction reduce_stations(const std::vector<Station>& st, const std::vector<int>& fixed,
                          const std::vector<bool>& forbidden, const std::vector<double>* floor = nullptr);

// Deviation of a cross-section from the interpolation of two others (px).
double station_deviation(const Station& a, const Station& b, const Station& x);

}  // namespace pixel3d::lowpoly
