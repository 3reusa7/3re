#pragma once
// Lofted tubes: a closed shell of N-sided rings along a spine (quads between
// rings, a flat fan or an apex at each end). LoftRing k of N has vertices
// c + cos θ·a·n + sin θ·b·ẑ (θ = 2πj/N from +n), so the Front silhouette
// extremes (θ = 0, π) are exact; b is scaled so the N-gon's depth extent is
// exactly the measured/inferred depth. N is even.
#include <array>
#include <cstdint>
#include <vector>

#include "pixel3d/common/math.hpp"
#include "pixel3d/lowpoly/profile.hpp"

namespace pixel3d::lowpoly {

struct LoftRing {
  vec3d c;       // world centre
  vec3d n;       // world cross direction (unit, z = 0)
  double a = 0;  // half width along n
  double b = 0;  // half depth along z (the depth extent; the N-gon scaling is applied when building)
  double s = 0;  // spine arc length
  int owner = -1;            // part id
  int bone_a = -1, bone_b = -1;
  float w_b = 0;             // weight of bone_b (0 = rigid on bone_a)
  bool joint_loop = false;
  bool lathe = false;        // placed on a round part's fitted ellipsoid (regular latitude)
};

enum class Cap { Flat, Apex };

struct TubeShape {
  int sides = 6;
  std::vector<LoftRing> rings;
  Cap cap_start = Cap::Flat, cap_end = Cap::Flat;
  vec3d apex_start, apex_end;  // used for Cap::Apex
};

struct TubeMesh {
  std::vector<vec3d> positions;           // rings × sides, then the apexes
  std::vector<std::array<uint32_t, 3>> tris;
  std::vector<int> vertex_ring;           // ring index; apex vertices take their end ring
  std::vector<int> vertex_side;           // j in 0 … sides−1; −1 for apexes
};

int cap_triangles(Cap c, int sides);
int tube_triangles(int sides, int rings, Cap start, Cap end);

// World ring from a profile station (image → world: x = u, y = H − v).
LoftRing ring_from_station(const Station& st, int model_height);
// Profile station at arc length s (linear between the dense stations).
Station station_at(const std::vector<Station>& dense, double s);

// Consecutive rings must not fold over each other where the spine bends: the
// cross directions are blended towards their neighbours until every side
// advances along the spine. Returns the number of rings changed.
int untangle(std::vector<LoftRing>& rings, int sides);

// Closed, outward-wound shell (exactly tube_triangles() triangles).
TubeMesh build_tube(const TubeShape& t);

}  // namespace pixel3d::lowpoly
