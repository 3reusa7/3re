#pragma once
// View sets LOW-POLY STUDIO accepts: Front + Side + Top (the usual alignment),
// Front + Side, Front + Top, or Front only. Each view is scaled and cropped as
// usual; shared axes must agree within the alignment tolerance (a 1 px
// difference is padded and reported, like extent_padded).
#include <vector>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/views/align.hpp"

namespace pixel3d::lowpoly {

// `front_only_depth`: box depth (source pixels) when there is no Side or Top view;
// 0 = automatic (as deep as the Front drawing is wide).
Expected<AlignResult> align_views(std::vector<SourceView>& views, const AlignOptions& opt, double front_only_depth);

}  // namespace pixel3d::lowpoly
