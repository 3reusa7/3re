#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "lowpoly_helpers.hpp"
#include "pixel3d/lowpoly/allocate.hpp"
#include "pixel3d/lowpoly/assemble.hpp"
#include "pixel3d/lowpoly/rig.hpp"
#include "pixel3d/mesh/topology.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;
using namespace p3dtest;

namespace {

struct Built {
  Input in;
  Layout l;
  Allocation a;
  Assembly as;
};

Built build(const std::string& variant, int budget, int loops = 2, bool side = true) {
  Built b;
  b.in = load_lowpoly_fixture(variant, side, side ? 0 : 20);
  b.in.settings.tri_budget = budget;
  b.in.settings.joint_loops = loops;
  auto l = build_layout(b.in);
  REQUIRE(l);
  b.l = *l;
  auto a = allocate(b.l, b.in.settings);
  REQUIRE(a);
  b.a = *a;
  b.as = assemble(b.l, b.a, b.in.dims.H);
  return b;
}

}  // namespace

TEST_CASE("lowpoly rig: the assembled mesh keeps the budget, closed shells and full skinning") {
  for (const char* variant : {"cloaked", "humanoid"})
    for (int budget : {500, 1000, 1500}) {
      CAPTURE(variant);
      CAPTURE(budget);
      const Built b = build(variant, budget);
      CHECK(static_cast<int>(b.as.mesh.triangle_count()) == b.a.total);
      CHECK(static_cast<int>(b.as.mesh.triangle_count()) <= budget);
      for (size_t v = 0; v < b.as.mesh.normals.size(); ++v) {
        CAPTURE(v);
        CAPTURE(b.as.mesh.vertex_shell[v]);
        CAPTURE(b.as.islands[static_cast<size_t>(b.as.vertex_island[v])].kind);
        CHECK(length(b.as.mesh.normals[v]) == doctest::Approx(1));
      }
      for (const auto& sh : b.as.shells) {
        const TopologyReport r = analyze_topology(shell_mesh(sh));
        CHECK(r.watertight);
        CHECK(r.signed_volume > 0);
      }
      const Pose p = test_pose(b.l);
      const PoseResult pr = apply_pose(b.as.mesh, b.l.skeleton, p);
      const RigCheck rc = check_rig(b.as.mesh, b.l.skeleton, p, pr, &b.as);
      CHECK(rc.vertices == static_cast<int>(b.as.mesh.positions.size()));
      CHECK(rc.weights_ok());
      CHECK(rc.adjacency_ok());
      CHECK(rc.rigid_ok());
      CHECK(rc.rigid_vertices > rc.vertices / 2);
    }
}

TEST_CASE("lowpoly rig: the test pose bends the left knee (and the left arm when there is one)") {
  const Built c = build("cloaked", 1000);
  const Pose pc = test_pose(c.l);
  CHECK(pc.local.size() == 1);
  CHECK(pc.local.contains(c.l.skeleton.find("LeftLowerLeg")));
  CHECK_FALSE(pc.notes.empty());  // no arm to raise

  const Built h = build("humanoid", 1000);
  const Pose ph = test_pose(h.l);
  CHECK(ph.local.size() == 2);
  const PoseResult pr = apply_pose(h.as.mesh, h.l.skeleton, ph);
  // The left hand (end of the left lower arm) ends up above the shoulder.
  const int ua = h.l.skeleton.find("LeftUpperArm"), la = h.l.skeleton.find("LeftLowerArm");
  const Bone& lower = h.l.skeleton.bones[static_cast<size_t>(la)];
  const vec3d tip = pr.bone_world[static_cast<size_t>(la)].apply(lower.tail - lower.head);
  CHECK(tip.y > h.l.skeleton.bones[static_cast<size_t>(ua)].head.y);
  // The left foot moves backwards (−Z), the right one stays.
  const int ll = h.l.skeleton.find("LeftLowerLeg");
  const Bone& shin = h.l.skeleton.bones[static_cast<size_t>(ll)];
  const vec3d foot = pr.bone_world[static_cast<size_t>(ll)].apply(shin.tail - shin.head);
  CHECK(foot.z < shin.head.z - 0.5 * length(shin.tail - shin.head));
  // Joint volume around the bent joints is reported.
  const RigCheck rc = check_rig(h.as.mesh, h.l.skeleton, ph, pr, &h.as);
  CHECK(rc.joint_volume.size() == 2);
  for (const auto& [name, ratio] : rc.joint_volume) {
    CAPTURE(name);
    CHECK(ratio > 0.5);
    CHECK(ratio < 1.5);
  }
}

TEST_CASE("lowpoly rig: corrupted weights are caught") {
  Built b = build("humanoid", 800);
  const Pose p = test_pose(b.l);
  SkinnedMesh m = b.as.mesh;
  m.weights[0] = {0.7f, 0, 0, 0};
  size_t rigid = 0;
  while (m.weights[rigid][1] != 0 || m.vertex_joint_loop[rigid]) ++rigid;
  m.weights[rigid + 1] = {0.5f, 0.5f, 0, 0};  // a blend off the joint loops
  m.joints[rigid + 1][1] = static_cast<uint16_t>(b.l.skeleton.find("Head"));
  const RigCheck rc = check_rig(m, b.l.skeleton, p, apply_pose(m, b.l.skeleton, p));
  CHECK_FALSE(rc.weights_ok());
  CHECK_FALSE(rc.adjacency_ok());
}

TEST_CASE("lowpoly rig: identity pose moves nothing") {
  const Built b = build("cloaked", 700);
  const PoseResult pr = apply_pose(b.as.mesh, b.l.skeleton, Pose{});
  for (size_t i = 0; i < pr.positions.size(); ++i) CHECK(length(pr.positions[i] - b.as.mesh.positions[i]) < 1e-9);
}

TEST_CASE("lowpoly head: head rings are regular latitudes of one symmetric ellipsoid") {
  for (int budget : {500, 1000, 1500}) {
    CAPTURE(budget);
    const Built b = build("cloaked", budget);
    const TubeShape& trunk = b.as.shapes[0];
    std::vector<const LoftRing*> head;
    for (const auto& r : trunk.rings)
      if (r.owner == 1000 && !r.joint_loop && r.lathe) head.push_back(&r);
    REQUIRE(head.size() >= 3);
    // Every lathe ring lies on the same ellipsoid: (a/rx)² + ((y−y0)/ry)² = 1, b/a constant.
    const RoundFit* fit = nullptr;
    for (const auto& f : b.l.tubes[0].round_fits)
      if (f.part == 1000) fit = &f;
    REQUIRE(fit);
    CHECK(fit->ok);
    std::vector<double> lat;
    for (const LoftRing* r : head) {
      const double sy = (r->c.y - fit->y0) / fit->ry, ca = r->a / fit->rx;
      CHECK(ca * ca + sy * sy == doctest::Approx(1).epsilon(1e-6));
      CHECK(r->b / r->a == doctest::Approx(fit->rz / fit->rx).epsilon(1e-6));
      CHECK(r->c.x == doctest::Approx(fit->x0));
      lat.push_back(std::asin(std::clamp(sy, -1.0, 1.0)));
    }
    std::sort(lat.begin(), lat.end());
    // Equal spacing above the equator.
    std::vector<double> up;
    for (double v : lat)
      if (v >= -1e-9) up.push_back(v);
    for (size_t i = 2; i < up.size(); ++i) CHECK(up[i] - up[i - 1] == doctest::Approx(up[1] - up[0]).epsilon(0.02));
  }
}

TEST_CASE("lowpoly normals: smooth around the cross-section, hard where the profile turns more than 40 degrees") {
  const Built b = build("humanoid", 1000);
  const SkinnedMesh& m = b.as.mesh;
  // For every triangle the corner normals stay within 40° + half a side step of the face normal (no smoothing across creases).
  int hard_split = 0;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const vec3d fn = normalize(cross(m.positions[m.indices[t + 1]] - m.positions[m.indices[t]], m.positions[m.indices[t + 2]] - m.positions[m.indices[t]]));
    for (int k = 0; k < 3; ++k) {
      const double ang = std::acos(std::clamp(dot(fn, m.normals[m.indices[t + static_cast<size_t>(k)]]), -1.0, 1.0)) * 180 / 3.14159265358979;
      CHECK(ang < 40 + 45);
    }
  }
  // Flat caps are hard: a cap vertex normal equals its cap face normal.
  std::map<std::tuple<double, double, double>, std::set<std::tuple<long, long, long>>> normals_at;
  for (size_t v = 0; v < m.positions.size(); ++v)
    normals_at[{m.positions[v].x, m.positions[v].y, m.positions[v].z}].insert(
        {std::lround(m.normals[v].x * 1e4), std::lround(m.normals[v].y * 1e4), std::lround(m.normals[v].z * 1e4)});
  for (const auto& [p, ns] : normals_at) hard_split += ns.size() > 1;
  CHECK(hard_split > 0);
}
