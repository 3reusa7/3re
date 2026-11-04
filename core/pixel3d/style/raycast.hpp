#pragma once
// Ray queries against a triangle mesh (bounding-volume hierarchy), used by the
// polished-style bakers. Deterministic; no threads.
#include <cstdint>
#include <vector>

#include "pixel3d/common/math.hpp"

namespace pixel3d {

class TriangleBVH {
 public:
  TriangleBVH(const std::vector<vec3d>& positions, const std::vector<uint32_t>& indices);
  // Nearest hit distance along the unit direction `dir` in (tmin, tmax), or a negative value.
  double hit(vec3d origin, vec3d dir, double tmin, double tmax) const;
  bool occluded(vec3d origin, vec3d dir, double tmin, double tmax) const { return hit(origin, dir, tmin, tmax) >= 0; }

 private:
  struct Node {
    vec3d lo, hi;
    int left = -1, right = -1;  // children, or -1 for a leaf
    int first = 0, count = 0;   // leaf triangles in order_
  };
  int build(int first, int count);
  const std::vector<vec3d>& p_;
  const std::vector<uint32_t>& idx_;
  std::vector<int> order_;
  std::vector<Node> nodes_;
};

}  // namespace pixel3d
