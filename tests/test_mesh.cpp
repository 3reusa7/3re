#include <doctest/doctest.h>

#include "pixel3d/mesh/cleanup.hpp"
#include "pixel3d/mesh/topology.hpp"

using namespace pixel3d;

namespace {

// Axis-aligned box with outward-facing triangles, appended to `m`.
void add_box(Mesh& m, vec3d lo, vec3d hi, int part = 0) {
  const uint32_t b = static_cast<uint32_t>(m.positions.size());
  for (int i = 0; i < 8; ++i)
    m.positions.push_back({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
  const uint32_t f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                             {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
  for (const auto& t : f) {
    m.tris.push_back({b + t[0], b + t[1], b + t[2]});
    m.tri_part.push_back(part);
  }
}

Mesh box_mesh(vec3d lo, vec3d hi) {
  Mesh m;
  add_box(m, lo, hi);
  return m;
}

}  // namespace

TEST_CASE("mesh: closed cube is watertight with 0 nonmanifold") {
  const Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  const TopologyReport t = analyze_topology(m);
  CHECK(t.watertight);
  CHECK(t.boundary_edges == 0);
  CHECK(t.nonmanifold_edges == 0);
  CHECK(t.nonmanifold_vertices == 0);
  CHECK(t.inconsistent_winding_edges == 0);
  CHECK(t.signed_volume == doctest::Approx(1.0));
  CHECK(t.shells == 1);
  CHECK(t.triangle_count == 12);
  CHECK(t.planar_regions == 6);
}

TEST_CASE("mesh: missing triangle is not watertight") {
  Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  m.tris.pop_back();
  m.tri_part.pop_back();
  const TopologyReport t = analyze_topology(m);
  CHECK_FALSE(t.watertight);
  CHECK(t.boundary_edges == 3);
}

TEST_CASE("mesh: two cubes sharing an edge report nonmanifold edge") {
  Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  add_box(m, {1, 1, 0}, {2, 2, 1});
  const Mesh welded = cleanup_mesh(m, CleanupOptions{}, nullptr);
  const TopologyReport t = analyze_topology(welded);
  CHECK(t.nonmanifold_edges >= 1);
  CHECK_FALSE(t.watertight);
}

TEST_CASE("mesh: cubes touching at a vertex report nonmanifold vertex") {
  Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  add_box(m, {1, 1, 1}, {2, 2, 2});
  const Mesh welded = cleanup_mesh(m, CleanupOptions{}, nullptr);
  const TopologyReport t = analyze_topology(welded);
  CHECK(t.nonmanifold_vertices == 1);
  CHECK(t.nonmanifold_edges == 0);
}

TEST_CASE("mesh: inverted shell is flipped by cleanup") {
  Mesh m = box_mesh({0, 0, 0}, {2, 2, 2});
  for (auto& t : m.tris) std::swap(t[1], t[2]);
  CHECK(analyze_topology(m).signed_volume == doctest::Approx(-8.0));
  CleanupReport rep;
  const Mesh c = cleanup_mesh(m, CleanupOptions{}, &rep);
  CHECK(rep.flipped_shells == 1);
  CHECK(analyze_topology(c).signed_volume == doctest::Approx(8.0));
}

TEST_CASE("mesh: single flipped triangle is reoriented") {
  Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  std::swap(m.tris[3][1], m.tris[3][2]);
  CHECK(analyze_topology(m).inconsistent_winding_edges == 3);
  const Mesh c = cleanup_mesh(m, CleanupOptions{}, nullptr);
  const TopologyReport t = analyze_topology(c);
  CHECK(t.inconsistent_winding_edges == 0);
  CHECK(t.signed_volume == doctest::Approx(1.0));
}

TEST_CASE("mesh: coincident opposite faces counted internal and removed") {
  Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  m.tris.push_back({m.tris[0][0], m.tris[0][2], m.tris[0][1]});  // same face, reversed
  m.tri_part.push_back(0);
  CHECK(analyze_topology(m).internal_faces == 1);
  CleanupReport rep;
  const Mesh c = cleanup_mesh(m, CleanupOptions{}, &rep);
  CHECK(rep.removed_internal == 2);
  CHECK(analyze_topology(c).internal_faces == 0);
}

TEST_CASE("mesh: duplicate faces are counted") {
  Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  m.tris.push_back(m.tris[5]);
  m.tri_part.push_back(0);
  CHECK(analyze_topology(m).duplicate_faces == 1);
  CHECK(analyze_topology(cleanup_mesh(m, CleanupOptions{}, nullptr)).duplicate_faces == 0);
}

TEST_CASE("mesh: zero-area triangles are counted and removed") {
  Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  m.positions.push_back({0.5, 0, 0});
  m.tris.push_back({0, 1, static_cast<uint32_t>(m.positions.size() - 1)});  // collinear
  m.tri_part.push_back(0);
  CHECK(analyze_topology(m).zero_area_faces == 1);
  CleanupReport rep;
  const Mesh c = cleanup_mesh(m, CleanupOptions{}, &rep);
  CHECK(rep.removed_zero_area == 1);
  CHECK(analyze_topology(c).watertight);
}

TEST_CASE("mesh: tiny fragment removed, big shell kept") {
  Mesh m = box_mesh({0, 0, 0}, {3, 3, 3});
  add_box(m, {10, 10, 10}, {10.5, 10.5, 10.5}, 7);  // volume 0.125 < 0.5
  CleanupReport rep;
  const Mesh c = cleanup_mesh(m, CleanupOptions{}, &rep);
  CHECK(rep.removed_fragments == 1);
  REQUIRE(rep.kept_shell_volumes.size() == 1);
  CHECK(rep.kept_shell_volumes[0] == doctest::Approx(27));
  CHECK(analyze_topology(c).shells == 1);
  CHECK(c.tris.size() == 12);
  CHECK(c.tri_part.size() == 12);
}

TEST_CASE("mesh: weld merges coincident vertices") {
  Mesh m = box_mesh({0, 0, 0}, {1, 1, 1});
  m.positions.push_back(m.positions[0]);  // duplicate of vertex 0
  m.tris[0][0] = static_cast<uint32_t>(m.positions.size() - 1);
  CHECK_FALSE(analyze_topology(m).watertight);
  CleanupReport rep;
  const Mesh c = cleanup_mesh(m, CleanupOptions{}, &rep);
  CHECK(rep.welded == 1);
  CHECK(analyze_topology(c).watertight);
  CHECK(c.positions.size() == 8);
}

TEST_CASE("mesh: planar regions of a box are its six faces") {
  Mesh m = box_mesh({0, 0, 0}, {2, 1, 1});
  label_planar_regions(m);
  int max_region = 0;
  for (int r : m.tri_region) max_region = std::max(max_region, r);
  CHECK(max_region + 1 == 6);
}
