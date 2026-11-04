#pragma once
// Maps baked into the unique UV set (TexturedMesh::uvs1) for polished styles.
//
// Ambient occlusion (step 7): per texel, the share of a cosine-weighted
// hemisphere of rays that leave the model within `max_distance` source px.
// Shading bevel (step 4b): a tangent-space normal map that makes convex crisp
// edges look rounded with `radius`, larger than the geometry may be (the
// silhouette never changes). Near such an edge the normal is that of a round
// fillet of the given radius; elsewhere it is the surface's own normal.
// Texels outside every chart take the nearest chart texel (mipmap-safe).
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

struct AOOptions {
  int rays = 32;
  double max_distance = 4.0;  // source px
};

ImageRGBA bake_ambient_occlusion(const TexturedMesh& m, int width, int height, const AOOptions& opt = {});

struct BevelOptions {
  double radius = 2.0;  // source px
};

// `crisp`: the unstyled solid whose convex edges are rounded in shading (its
// tri_region labels its planar faces).
ImageRGBA bake_bevel_normals(const TexturedMesh& m, const Mesh& crisp, int width, int height, const BevelOptions& opt = {});

}  // namespace pixel3d
