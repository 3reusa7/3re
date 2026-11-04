#pragma once
// Second UV set for polished styles (TEXCOORD_1): every surface point owns its
// own texels, as baked maps need (ambient occlusion, shading-bevel normals).
// Each planar region of a part becomes one chart, laid flat without distortion
// at `texels_per_unit` and packed with `padding` texels around it. Vertices are
// split along chart borders; positions, UV0, normals and triangles stay as
// they were. Tangents follow each chart's u direction (glTF TANGENT, w = 1).
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

struct UniqueUVReport {
  int charts = 0;
  int width = 0, height = 0;  // texels of the chart atlas
};

TexturedMesh add_unique_uv(const TexturedMesh& m, double texels_per_unit = 8, int padding = 2,
                           UniqueUVReport* report = nullptr);

}  // namespace pixel3d
