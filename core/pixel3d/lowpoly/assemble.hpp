#pragma once
// Assembly: planned tubes → one skinned render/export mesh. Each tube body is
// one UV island (cut along a seam at the back, so ring vertices on the seam are
// duplicated), each cap its own island (cap vertices are copies with the cap's
// own normal). Normals are smooth around each cross-section (the section is meant
// round) and hard wherever the surface turns by more than kCreaseDeg between
// faces along the profile, at caps and cone tips (vertices split there). Weights come from the rings: 1.0 on the ring's bone,
// blended between two adjacent bones on joint loops only.
#include <vector>

#include "pixel3d/lowpoly/allocate.hpp"
#include "pixel3d/lowpoly/layout.hpp"
#include "pixel3d/lowpoly/loft.hpp"
#include "pixel3d/mesh/mesh.hpp"

namespace pixel3d::lowpoly {

inline constexpr double kCreaseDeg = 40.0;

struct Island {
  int shell = -1;
  int kind = 0;           // 0 body, 1 start cap, 2 end cap
  int part = -1;          // owning part (body: the tube's first part)
  vec2d lo, hi;           // island-local bounds (world units)
  // Filled by packing: placement in the atlas (texels).
  int x0 = 0, y0 = 0, w = 0, h = 0;
};

struct Assembly {
  SkinnedMesh mesh;                 // uvs are island-local (world units) until pack_islands()
  std::vector<int> vertex_island;
  std::vector<Island> islands;
  std::vector<TubeMesh> shells;     // welded shells (for topology checks)
  std::vector<TubeShape> shapes;
};

Assembly assemble(const Layout& l, const Allocation& a, int model_height);

// Mesh of one welded shell (positions shared at seams), for topology checks.
Mesh shell_mesh(const TubeMesh& t);

}  // namespace pixel3d::lowpoly
