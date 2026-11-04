#pragma once
#include "fixturegen/gt_model.hpp"
#include "pixel3d/image/image.hpp"
#include "pixel3d/views/view_role.hpp"

namespace p3dfix {

// Orthographic pixel-art render of the ground truth: one ray per pixel centre,
// flat ramp shading by face orientation, and a 1 px outline wherever an edge
// (silhouette, part change or depth jump > 1.5) has interior pixels behind it.
// The image covers the full fixture box: view_image_size(role, gt.dims).
pixel3d::ImageRGBA render_gt_view(const GroundTruth& gt, pixel3d::ViewRole role);

}  // namespace p3dfix
