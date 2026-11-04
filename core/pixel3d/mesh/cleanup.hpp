#pragma once
#include <vector>

#include "pixel3d/mesh/mesh.hpp"

namespace pixel3d {

// The cleanup a user/agent can configure. (Vertex welding is not an option:
// every model is Manifold output, where welding would only create
// non-manifold edges — see WeldOptions.)
struct CleanupOptions {
  double min_fragment_volume = 0.5;  // shells smaller than this (in voxels) are accidental
};

// Low-level: merge vertices at the same position. For raw meshes (tests,
// future imports) only. Manifold output keeps separate vertices on purpose
// where solids touch along an edge or at a point.
struct WeldOptions {
  bool enabled = true;
  double eps = 1e-9;
};

struct CleanupReport {
  int welded = 0;
  int removed_zero_area = 0;
  int removed_duplicates = 0;
  int removed_internal = 0;    // triangles removed as coincident opposite pairs
  int reoriented_faces = 0;    // faces flipped to agree with their neighbours
  int flipped_shells = 0;      // shells turned inside out to face outwards
  int removed_fragments = 0;   // shells
  std::vector<double> kept_shell_volumes;
};

// Safe cleanup that never moves a vertex: weld duplicates, drop degenerate,
// duplicate and internal faces, make winding consistent and outward, and drop
// tiny accidental fragments. Part and region labels follow their triangles.
Mesh cleanup_mesh(const Mesh& in, const CleanupOptions& opt, CleanupReport* report, const WeldOptions& weld = {});

// Groups edge-connected coplanar triangles; fills m.tri_region with 0..n-1.
void label_planar_regions(Mesh& m, double angle_eps = 1e-9);

}  // namespace pixel3d
