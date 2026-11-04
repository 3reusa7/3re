#pragma once
// Analytic ground truth for synthetic fixtures: a union of convex polyhedra
// (each a set of half-spaces n·p ≤ d). Test-only; the core never sees it.
#include <array>
#include <string>
#include <utility>
#include <vector>

#include "pixel3d/common/math.hpp"
#include "pixel3d/image/image.hpp"
#include "pixel3d/views/view_role.hpp"

namespace p3dfix {

using pixel3d::Dims;
using pixel3d::Rgba8;
using pixel3d::vec3d;

struct Plane {
  vec3d n;   // outward, not necessarily unit length
  double d;  // inside: dot(n, p) <= d
};

struct ConvexPart {
  std::string name;
  std::vector<Plane> planes;
  int material = 0;
  bool contains(vec3d p, double eps = 1e-9) const;
};

struct AABB {
  vec3d lo, hi;
  bool contains(vec3d p, double eps = 1e-6) const;
};

struct GroundTruth {
  std::string name;
  Dims dims;
  std::vector<ConvexPart> parts;
  std::vector<std::array<Rgba8, 4>> ramps;  // per material: [shadow, slope, camera-facing, up-facing]
  Rgba8 outline{0x2a, 0x1d, 0x17, 0xff};
  std::vector<AABB> shadow_volumes;  // surfaces inside are drawn with ramp[0]

  bool inside(vec3d p) const;
  // Volume by sampling `samples_per_unit`³ points per unit cube.
  double volume(int samples_per_unit = 4) const;
};

ConvexPart box(std::string name, vec3d lo, vec3d hi, int material);

GroundTruth make_fixture_A();  // synthetic four-leg chair with a reclined back
GroundTruth make_fixture_C();  // asymmetric open planter on three legs with one handle

}  // namespace p3dfix
