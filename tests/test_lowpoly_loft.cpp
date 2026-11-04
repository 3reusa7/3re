#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

#include "pixel3d/lowpoly/loft.hpp"
#include "pixel3d/mesh/topology.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;

namespace {

Mesh as_mesh(const TubeMesh& t) {
  Mesh m;
  m.positions = t.positions;
  m.tris = t.tris;
  m.tri_part.assign(t.tris.size(), 0);
  return m;
}

TubeShape straight_tube(int sides, int rings, double a, double b) {
  TubeShape t;
  t.sides = sides;
  for (int i = 0; i < rings; ++i) {
    LoftRing r;
    r.c = {0, 10.0 * i, 0};
    r.n = {1, 0, 0};
    r.a = a * (1 + 0.1 * i);
    r.b = b;
    r.s = 10.0 * i;
    t.rings.push_back(r);
  }
  t.apex_start = {0, -5, 0};
  t.apex_end = {0, 10.0 * (rings - 1) + 5, 0};
  return t;
}

}  // namespace

TEST_CASE("lowpoly loft: triangle count follows the formula and the shell is closed") {
  for (int sides : {4, 6, 8})
    for (int rings : {2, 3, 7})
      for (Cap c0 : {Cap::Flat, Cap::Apex})
        for (Cap c1 : {Cap::Flat, Cap::Apex}) {
          CAPTURE(sides);
          CAPTURE(rings);
          TubeShape t = straight_tube(sides, rings, 4, 3);
          t.cap_start = c0;
          t.cap_end = c1;
          const TubeMesh m = build_tube(t);
          CHECK(static_cast<int>(m.tris.size()) == tube_triangles(sides, rings, c0, c1));
          CHECK(tube_triangles(sides, rings, c0, c1) ==
                2 * sides * (rings - 1) + cap_triangles(c0, sides) + cap_triangles(c1, sides));
          const TopologyReport rep = analyze_topology(as_mesh(m));
          CHECK(rep.watertight);
          CHECK(rep.inconsistent_winding_edges == 0);
          CHECK(rep.nonmanifold_edges == 0);
          CHECK(rep.shells == 1);
          CHECK(rep.signed_volume > 0);
        }
  CHECK(cap_triangles(Cap::Flat, 6) == 4);
  CHECK(cap_triangles(Cap::Apex, 6) == 6);
}

TEST_CASE("lowpoly loft: width and depth extremes are exact for every side count") {
  for (int sides : {4, 6, 8}) {
    CAPTURE(sides);
    const TubeMesh m = build_tube(straight_tube(sides, 2, 4, 3));
    double xmax = -1e9, zmax = -1e9, zmin = 1e9;
    for (size_t v = 0; v < m.positions.size(); ++v)
      if (m.vertex_ring[v] == 0 && m.vertex_side[v] >= 0) {
        xmax = std::max(xmax, m.positions[v].x);
        zmax = std::max(zmax, m.positions[v].z);
        zmin = std::min(zmin, m.positions[v].z);
      }
    CHECK(xmax == doctest::Approx(4));
    CHECK(zmax == doctest::Approx(3));
    CHECK(zmin == doctest::Approx(-3));
  }
}

TEST_CASE("lowpoly loft: untangle removes folds on a tight bend") {
  // Rings along a quarter circle of radius 6 with half width 7: the inner side folds.
  TubeShape t;
  t.sides = 6;
  for (int i = 0; i <= 8; ++i) {
    const double ang = (std::numbers::pi / 2) * i / 8;
    LoftRing r;
    r.c = {6 * std::cos(ang), 6 * std::sin(ang), 0};
    r.n = {std::cos(ang), std::sin(ang), 0};  // radial = cross direction
    r.a = 7;
    r.b = 2;
    r.s = 6 * ang;
    t.rings.push_back(r);
  }
  auto folds = [&](const std::vector<LoftRing>& rs) {
    int f = 0;
    for (size_t i = 0; i + 1 < rs.size(); ++i) {
      const vec3d d = rs[i + 1].c - rs[i].c;
      for (double sgn : {-1.0, 1.0}) {
        const vec3d p0 = rs[i].c + rs[i].n * (sgn * rs[i].a), p1 = rs[i + 1].c + rs[i + 1].n * (sgn * rs[i + 1].a);
        f += dot(p1 - p0, d) <= 0;
      }
    }
    return f;
  };
  CHECK(folds(t.rings) > 0);
  CHECK(untangle(t.rings, t.sides) > 0);
  CHECK(folds(t.rings) == 0);
  const TopologyReport rep = analyze_topology(as_mesh(build_tube(t)));
  CHECK(rep.watertight);
  CHECK(rep.signed_volume > 0);
}

TEST_CASE("lowpoly loft: stations map to world rings and interpolate") {
  std::vector<Station> st(3);
  for (int i = 0; i < 3; ++i) {
    st[static_cast<size_t>(i)].s = i;
    st[static_cast<size_t>(i)].centre = {10, 50.0 - i};
    st[static_cast<size_t>(i)].n = {1, 0};
    st[static_cast<size_t>(i)].half_w = 2 + i;
    st[static_cast<size_t>(i)].z_c = 7;
    st[static_cast<size_t>(i)].half_d = 1;
  }
  const Station mid = station_at(st, 1.5);
  CHECK(mid.half_w == doctest::Approx(3.5));
  CHECK(mid.centre.y == doctest::Approx(48.5));
  const LoftRing r = ring_from_station(mid, 60);
  CHECK(r.c.x == doctest::Approx(10));
  CHECK(r.c.y == doctest::Approx(11.5));
  CHECK(r.c.z == doctest::Approx(7));
  CHECK(r.n.x == doctest::Approx(1));
  CHECK(r.a == doctest::Approx(3.5));
}
