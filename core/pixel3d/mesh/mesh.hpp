#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "pixel3d/common/math.hpp"

namespace pixel3d {

// Indexed triangle mesh in world units (1 = one source pixel). Triangles are
// counter-clockwise when seen from outside.
struct Mesh {
  std::vector<vec3d> positions;
  std::vector<std::array<uint32_t, 3>> tris;
  std::vector<int> tri_part;    // 3D part id per triangle (-1 if unknown)
  std::vector<int> tri_region;  // planar region id per triangle (label_planar_regions)
};

inline vec3d tri_normal_raw(const Mesh& m, size_t t) {
  const vec3d a = m.positions[m.tris[t][0]], b = m.positions[m.tris[t][1]], c = m.positions[m.tris[t][2]];
  return cross(b - a, c - a);
}

}  // namespace pixel3d
