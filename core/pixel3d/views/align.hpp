#pragma once
#include <functional>
#include <map>
#include <vector>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/views/source_view.hpp"

namespace pixel3d {

struct AlignOptions {
  uint8_t alpha_threshold = 128;
  int extent_tolerance = 1;
  std::map<ViewRole, bool> force_flip;
};

struct AlignResult {
  Dims dims;
  std::vector<Message> messages;
  std::map<ViewRole, double> flip_confidence;  // |score(flipped) - score(normal)| of shared-axis profiles
  // Opaque pixels per view that no volume can explain: another view sharing an
  // axis has nothing at that coordinate.
  std::map<ViewRole, int> profile_conflicts;
};

// Normalises scale, crops, detects mirrored views, reconciles extents and fills
// SourceView::aligned for every view. Front, Side and Top are required.
// Reference axes: X from Front, Z from Top (front edge at image bottom), Y from Front.
Expected<AlignResult> auto_align(std::vector<SourceView>& views, const AlignOptions& opt);

// SINGLE VIEW: one drawing (any role). Scale and crop as above; the depth
// axis gets depth_for(width, height) cells (≥ 1).
Expected<AlignResult> align_single_view(std::vector<SourceView>& views, const AlignOptions& opt,
                                        const std::function<int(int, int)>& depth_for);

}  // namespace pixel3d
