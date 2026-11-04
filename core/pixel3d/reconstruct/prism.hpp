#pragma once
#include <map>
#include <vector>

#include <manifold/manifold.h>

#include "pixel3d/polygon/contour.hpp"
#include "pixel3d/polygon/simplify.hpp"
#include "pixel3d/segment/bands.hpp"
#include "pixel3d/views/source_view.hpp"

namespace pixel3d {

// Extrudes a region given in the view's image coordinates along the view axis
// through the whole box (depth −1 … extent+1), placed in world space.
manifold::Manifold make_prism(ViewRole role, Dims d, const Polygons2D& region_in_image_coords);
// Same, limited to depths [depth0, depth1] along the view direction.
manifold::Manifold make_prism_range(ViewRole role, Dims d, const Polygons2D& region_in_image_coords, double depth0,
                                    double depth1);

struct ViewPolygons {
  ViewRole role = ViewRole::Front;
  Polygons2D simplified;  // feature-preserving, exact at pixel centres
  Polygons2D exact;       // rectilinear pixel outline
};

std::map<ViewRole, ViewPolygons> build_view_polygons(const std::vector<SourceView>& views, const SimplifyOptions& opt,
                                                     uint8_t alpha_threshold);

// The view polygon restricted to one atom: band slab ∩ the atom's corridor
// (its columns, widened to the middle of the gaps to neighbouring strands).
Polygons2D atom_region(const ViewPolygons& vp, const Band& band, const BandView& bv, const Atom& atom, int H);

}  // namespace pixel3d
