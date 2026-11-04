#pragma once
// Game fidelity (agent milestone 2, A1): does the reduced model LOOK like the original and is the file sound?
// Self-intersections are not part of it (engines draw touching/crossing layers fine); they are measured and
// reported elsewhere. Measures only; thresholds come from the spec (docs/agent/FIDELITY.md).
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_glb.hpp"

namespace p3dagent {

// All triangles of a GLB in world space (node hierarchy applied), with UVs and the base-colour image of each
// primitive when it has one. Positions are not welded: topology is read from the stored indices.
struct FMesh {
  std::vector<std::array<float, 3>> pos;
  std::vector<std::array<uint32_t, 3>> tri;
  std::vector<std::array<float, 2>> uv;   // empty or one per vertex
  std::vector<int32_t> tri_image;         // per triangle: index into images, -1 = none
  std::vector<int32_t> vert_prim;         // per vertex: the primitive it came from (welding never crosses primitives)
  struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;            // sRGB
  };
  std::vector<Image> images;
  bool textured() const;
};
FMesh fmesh_from_glb(const Glb& glb);     // throws on unreadable accessors / images

struct Topology {
  int64_t triangles = 0, boundary_edges = 0, boundary_loops = 0, overfull_edges = 0, inconsistent_winding_edges = 0,
          degenerate_triangles = 0;
  nlohmann::json to_json() const;
};
Topology topology(const FMesh& m);

struct FidelityOptions {
  int size = 256;             // render size per view
  int samples = 60000;        // surface samples per side for the distance
  bool colour = true;         // compare base colour when both are textured
};

struct Fidelity {
  // Symmetric surface distance (area-weighted samples on each surface to the other), % of the original diagonal.
  double distance_mean_pct = 0, distance_p95_pct = 0, distance_max_pct = 0;
  // Unlit renders, 8 yaws (0, 45, ..., 315) at elevation 10 + top, one frame from the original's box.
  std::vector<double> silhouette_iou;   // per view
  double silhouette_iou_min = 1, silhouette_iou_mean = 1;
  double normal_mean_deg = 0;           // mean angle between front-most surface normals on pixels both cover
  double colour_mean_de = -1;           // Oklab dE of base colour on pixels both cover (-1: not measured)
  double colour_p95_de = -1;
  // The same on 8x8-pixel block means (what a person sees at that size; a fine line moved by a pixel does not
  // count, a region painted another colour does). The judged colour number.
  double colour_block_mean_de = -1, colour_block_p95_de = -1;
  Topology original, reduced;
  nlohmann::json to_json() const;
};
Fidelity measure_fidelity(const FMesh& original, const FMesh& reduced, const FidelityOptions& opt = {});

// Unlit base-colour render (RGBA, transparent background), orthographic, yaw from +Z, elevation in degrees,
// frame = the mesh's own box (or the given centre/radius when radius > 0).
std::vector<uint8_t> render_unlit(const FMesh& m, double yaw_deg, double elevation_deg, int size,
                                  std::array<double, 3> centre = {0, 0, 0}, double radius = 0);

// PASS when every measured number is within its threshold and the reduced mesh adds no boundary edges, holes,
// overfull edges, inconsistent winding or degenerate triangles; otherwise FAIL with the reasons.
nlohmann::json judge_fidelity(const Fidelity& f, const FidelityThresholds& t);

}  // namespace p3dagent
