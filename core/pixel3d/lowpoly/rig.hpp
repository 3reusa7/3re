#pragma once
// Rig evaluation: poses, CPU linear blend skinning and the checks that prove the
// skinning is clean — every vertex has weights summing to 1 on at most two
// bones, the two are parent and child, blending happens only on joint loops,
// and under a pose every rigid (single-bone) vertex keeps its exact position in
// its bone's frame.
#include <array>
#include <map>
#include <string>
#include <vector>

#include "pixel3d/lowpoly/assemble.hpp"
#include "pixel3d/lowpoly/layout.hpp"

namespace pixel3d::lowpoly {

struct Mat3 {
  std::array<double, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
  vec3d operator*(vec3d v) const {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
  }
  Mat3 operator*(const Mat3& o) const;
  Mat3 transposed() const;
  static Mat3 axis_angle(vec3d axis, double deg);
};

// Rigid transform p ↦ R p + t.
struct Xform {
  Mat3 R;
  vec3d t;
  vec3d apply(vec3d p) const { return R * p + t; }
};

struct Pose {
  std::string name;
  std::map<int, Mat3> local;  // bone → rotation about its head (rest rotations are identity)
  std::vector<std::string> notes;
};

// The template's test pose: left arm raised 90° (shoulder), left knee bent 90°.
// Parts of it the rig cannot show (a cloaked character has no arms) are noted.
Pose test_pose(const Layout& l);

struct PoseResult {
  std::vector<Xform> bone_world;  // posed bone frames (rest frame = translation to the head)
  std::vector<Xform> skin;        // skinning transforms: posed ∘ rest⁻¹
  std::vector<vec3d> positions, normals;
};
PoseResult apply_pose(const SkinnedMesh& m, const Skeleton& s, const Pose& p);

struct RigCheck {
  int vertices = 0;
  int bad_weight_sum = 0;       // |Σw − 1| > 1e-5 or a negative weight
  int bad_joint_index = 0;
  int non_adjacent = 0;         // two influences that are not parent and child
  int blended_off_loop = 0;     // blended weights on a vertex that is not on a joint loop
  int unskinned = 0;            // no weight at all
  int rigid_vertices = 0;
  double rigid_max_error = 0;   // world units: posed vs the bone frame applied to the rest position
  double rigid_max_distance_change = 0;  // between vertices of one bone (independent of the matrices)
  std::vector<std::pair<std::string, double>> joint_volume;  // posed / rest volume around each bent joint
  bool weights_ok() const { return bad_weight_sum == 0 && bad_joint_index == 0 && unskinned == 0; }
  bool adjacency_ok() const { return non_adjacent == 0 && blended_off_loop == 0; }
  bool rigid_ok() const { return rigid_max_error < 1e-6 && rigid_max_distance_change < 1e-6; }
};
RigCheck check_rig(const SkinnedMesh& m, const Skeleton& s, const Pose& p, const PoseResult& r, const Assembly* as = nullptr);

}  // namespace pixel3d::lowpoly
