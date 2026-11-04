#pragma once
#include "pixel3d/mesh/mesh.hpp"

namespace pixel3d {

struct TopologyReport {
  bool watertight = false;             // every edge used by exactly two triangles
  int boundary_edges = 0;              // used once
  int nonmanifold_edges = 0;           // used three or more times
  int nonmanifold_vertices = 0;        // incident triangles form more than one fan
  int inconsistent_winding_edges = 0;  // used twice in the same direction
  int duplicate_faces = 0;             // extra copies with the same orientation
  int internal_faces = 0;              // coincident pairs with opposite orientation
  int zero_area_faces = 0;
  int shells = 0;                      // edge-connected components
  double signed_volume = 0;
  int triangle_count = 0;
  int vertex_count = 0;                // referenced vertices
  int planar_regions = 0;
};

TopologyReport analyze_topology(const Mesh& m, double area_eps = 1e-12);

}  // namespace pixel3d
