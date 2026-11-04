#pragma once
// Y-band decomposition of the views that contain the vertical axis (Front,
// Back, Side, Right). A band is a maximal interval of world rows over which the
// run structure of every such view is continuous (1-to-1 overlap of runs between
// consecutive rows, diagonal contact counts). Strands chain the runs through a
// band; atoms are column slices of strands, split where a neighbouring band
// branches into more strands (e.g. where legs leave a seat).
#include <map>
#include <vector>

#include "pixel3d/image/mask.hpp"
#include "pixel3d/views/view_role.hpp"

namespace pixel3d {

struct Run {
  int y = 0;  // world row Y
  int a = 0;  // [a, b) along the view's horizontal image axis
  int b = 0;
};

struct Strand {
  int id = 0;
  std::vector<Run> runs;  // one run per band row, bottom to top
  int a = 0, b = 0;       // union extent of the runs
};

struct Atom {
  int id = 0;
  int strand = 0;
  int a = 0, b = 0;    // [a, b) image columns, constant over the band
  bool split = false;  // produced by splitting a strand at a neighbour's branch
};

struct BandView {
  ViewRole role = ViewRole::Front;
  std::vector<Strand> strands;  // sorted left to right
  std::vector<Atom> atoms;      // sorted left to right
};

struct Band {
  int id = 0;
  int y0 = 0, y1 = 0;  // world rows [y0, y1)
  std::vector<BandView> views;
};

struct BandSet {
  std::vector<Band> bands;  // sorted bottom to top
  const BandView* view(int band, ViewRole r) const;
  const Band* band_containing(int y) const;
};

// `y_views`: aligned masks of the Y-containing views (any of Front/Back/Side/Right).
BandSet compute_bands(const std::map<ViewRole, Mask2D>& y_views, int H);

}  // namespace pixel3d
