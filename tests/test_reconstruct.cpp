#include <doctest/doctest.h>

#include "pipeline_helpers.hpp"
#include "pixel3d/mesh/topology.hpp"
#include "pixel3d/reconstruct/depth_constraint.hpp"
#include "pixel3d/reconstruct/true_solid.hpp"
#include "pixel3d/reconstruct/voxel_mesher.hpp"

using namespace pixel3d;
using namespace p3dtest;

namespace {

double mesh_volume(const Mesh& m) { return analyze_topology(m).signed_volume; }

void check_clean_solid(const SolidResult& s) {
  CHECK(s.topology.watertight);
  CHECK(s.topology.nonmanifold_edges == 0);
  CHECK(s.topology.nonmanifold_vertices == 0);
  CHECK(s.topology.internal_faces == 0);
  CHECK(s.topology.duplicate_faces == 0);
  CHECK(s.topology.zero_area_faces == 0);
  CHECK(s.topology.inconsistent_winding_edges == 0);
  CHECK(s.topology.signed_volume > 0);
}

}  // namespace

TEST_CASE("reconstruct: A true solid is watertight manifold low-poly") {
  const Corr c = run_fixture("A_synthetic_chair");
  const SolidResult s = assemble_true_solid(c.cands, {}, c.dims, CleanupOptions{}, c.part_of_candidate());
  check_clean_solid(s);
  CHECK(s.topology.shells == 1);
  CHECK(s.topology.triangle_count <= 600);
  CHECK(s.mesh.tri_part.size() == s.mesh.tris.size());
  std::set<int> parts(s.mesh.tri_part.begin(), s.mesh.tri_part.end());
  CHECK(parts.size() == 5);  // four legs + seat/back
  CHECK_FALSE(parts.contains(-1));
}

TEST_CASE("reconstruct: A survives 90 degrees") {
  const Corr c = run_fixture("A_synthetic_chair");
  const SolidResult s = assemble_true_solid(c.cands, {}, c.dims, CleanupOptions{}, c.part_of_candidate());
  vec3d lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
  for (const vec3d& p : s.mesh.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  CHECK(hi.x - lo.x >= 15);
  CHECK(hi.z - lo.z >= 15);  // real depth: seen from the side it is still a chair, not a plane
}

TEST_CASE("reconstruct: B and C true solids are clean") {
  for (const char* name : {"B_hand_chair", "C_asym_planter"}) {
    CAPTURE(name);
    const Corr c = run_fixture(name);
    const SolidResult s = assemble_true_solid(c.cands, {}, c.dims, CleanupOptions{}, c.part_of_candidate());
    check_clean_solid(s);
  }
}

TEST_CASE("reconstruct: removed candidates are not in the solid") {
  const Corr c = run_fixture("C_asym_planter_bottom", {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Bottom});
  const SolidResult s = assemble_true_solid(c.cands, {}, c.dims, CleanupOptions{}, c.part_of_candidate());
  check_clean_solid(s);
  double kept = 0;
  for (const auto& cand : c.cands.cands) kept += is_kept(cand.status) ? cand.volume : 0;
  CHECK(mesh_volume(s.mesh) == doctest::Approx(kept).epsilon(0.01));
}

TEST_CASE("reconstruct: solids touching along an edge stay watertight") {
  // Diagonal contacts are everyday pixel art; Manifold keeps the touching shells'
  // vertices separate and cleanup must not weld them into non-manifold edges.
  const Corr c = run_boxes({Box{0, 0, 0, 2, 2, 3}, Box{2, 2, 0, 4, 4, 3}}, Dims{4, 4, 3});
  const SolidResult s = assemble_true_solid(c.cands, {}, c.dims, CleanupOptions{}, c.part_of_candidate());
  check_clean_solid(s);
  CHECK(mesh_volume(s.mesh) == doctest::Approx(24));
}

TEST_CASE("reconstruct: voxel mode is blocky and watertight") {
  const Corr c = run_fixture("A_synthetic_chair");
  const VoxelGrid g = voxelize_candidates(c.cands, c.masks, c.bands, c.dims);
  const SolidResult s = build_voxel_mode(g, CleanupOptions{});
  check_clean_solid(s);
  CHECK(mesh_volume(s.mesh) == doctest::Approx(g.count()));
  for (size_t t = 0; t < s.mesh.tris.size(); ++t) {
    const vec3d n = normalize(tri_normal_raw(s.mesh, t));
    const int axis_aligned = (std::abs(n.x) > 0.999) + (std::abs(n.y) > 0.999) + (std::abs(n.z) > 0.999);
    CHECK(axis_aligned == 1);
  }
}

TEST_CASE("reconstruct: depth constraint carves C cavity") {
  const Corr c = run_fixture("C_asym_planter");
  const SolidResult before = assemble_true_solid(c.cands, {}, c.dims, CleanupOptions{}, c.part_of_candidate());
  // Cavity x∈[6,20) z∈[6,16) world → aligned top (2..16, 2..12); floor 11 below the rim.
  const DepthConstraint carve{ViewRole::Top, rect_polys(2, 2, 16, 12), 11, 1e9, "cavity"};
  const SolidResult after = assemble_true_solid(c.cands, {carve}, c.dims, CleanupOptions{}, c.part_of_candidate());
  check_clean_solid(after);
  CHECK(mesh_volume(before.mesh) - mesh_volume(after.mesh) == doctest::Approx(14 * 10 * 11).epsilon(0.05));
}

TEST_CASE("reconstruct: depth constraint on voxels matches the solid") {
  const Corr c = run_fixture("C_asym_planter");
  const DepthConstraint carve{ViewRole::Top, rect_polys(2, 2, 16, 12), 11, 1e9, "cavity"};
  const VoxelGrid g = voxelize_candidates(c.cands, c.masks, c.bands, c.dims);
  const VoxelGrid carved = apply_depth_constraints(g, {carve}, c.dims);
  CHECK(g.count() - carved.count() == 14 * 10 * 11);
}
