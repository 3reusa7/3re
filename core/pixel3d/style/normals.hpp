#pragma once
// Smooth shading normals for polished styles (step 4). The crisp style never
// calls this: its export keeps one flat normal per face.
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

struct NormalOptions {
  double smooth_angle_deg = 40;  // faces meeting at a smaller angle share a smooth normal
};

struct NormalReport {
  int hard_edges = 0;   // edges kept sharp by the angle threshold
  int part_edges = 0;   // edges kept sharp because two parts (materials) meet
  int split_vertices = 0;  // vertices added so each hard edge keeps two normals
};

// Same triangles, positions and UVs; normals are angle-weighted averages of the
// faces around each corner that are in the same part and within the angle. A
// vertex is split wherever its corners need different normals.
TexturedMesh smooth_normals(const TexturedMesh& m, const NormalOptions& opt = {}, NormalReport* report = nullptr);

}  // namespace pixel3d
