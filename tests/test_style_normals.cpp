// Polished styles, step 4: smooth normals by angle, split at hard edges and part borders.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <numbers>

#include "pixel3d/style/normals.hpp"

using namespace pixel3d;

namespace {

// Flat-shaded mesh (three vertices per triangle) from a closed Manifold-free
// description: a prism whose cross-section is a regular n-gon (axis Y).
TexturedMesh prism(int n, double radius, double height, int part_of_cap = 0) {
  TexturedMesh m;
  auto add = [&](vec3d a, vec3d b, vec3d c, int part) {
    const vec3d nrm = normalize(cross(b - a, c - a));
    for (const vec3d& p : {a, b, c}) {
      m.indices.push_back(static_cast<uint32_t>(m.positions.size()));
      m.positions.push_back(p);
      m.normals.push_back(nrm);
      m.uvs.push_back({0, 0});
    }
    m.tri_part.push_back(part);
  };
  auto ring = [&](int i, double y) {
    const double a = 2 * std::numbers::pi * (i % n) / n;  // the seam reuses vertex 0 exactly
    return vec3d{radius * std::cos(a), y, -radius * std::sin(a)};
  };
  for (int i = 0; i < n; ++i) {
    const vec3d a0 = ring(i, 0), a1 = ring(i + 1, 0), b0 = ring(i, height), b1 = ring(i + 1, height);
    add(a0, a1, b1, 0);
    add(a0, b1, b0, 0);
    add(vec3d{0, height, 0}, b0, b1, part_of_cap);
    add(vec3d{0, 0, 0}, a1, a0, part_of_cap);
  }
  m.texture = std::make_shared<ImageRGBA>(1, 1, Rgba8{200, 200, 200, 255});
  return m;
}

}  // namespace

TEST_CASE("normals: a fine prism side is smooth, its caps stay hard") {
  const TexturedMesh m = prism(24, 5, 4);  // 15° between side faces
  NormalReport rep;
  const TexturedMesh s = smooth_normals(m, NormalOptions{}, &rep);
  REQUIRE(s.indices.size() == m.indices.size());
  CHECK(rep.hard_edges == 48);  // the two cap rims
  for (size_t i = 0; i < s.indices.size(); ++i) {
    const vec3d p = s.positions[s.indices[i]], n = s.normals[s.indices[i]];
    CHECK(length(n) == doctest::Approx(1.0));
    CHECK(p == m.positions[m.indices[i]]);
    const size_t tri = i / 3;
    if (tri % 4 < 2) {
      // Side corner: the radial direction, not the face normal.
      const vec3d radial = normalize(vec3d{p.x, 0, p.z});
      CHECK(dot(n, radial) == doctest::Approx(1.0).epsilon(1e-9));
    } else {
      // Cap corner: still exactly the cap's flat normal.
      CHECK(std::abs(n.y) == doctest::Approx(1.0));
    }
  }
}

TEST_CASE("normals: a coarse prism (60° steps) stays faceted at 40°") {
  const TexturedMesh m = prism(6, 5, 4);
  NormalReport rep;
  const TexturedMesh s = smooth_normals(m, NormalOptions{}, &rep);
  CHECK(rep.hard_edges == 6 + 12);
  for (size_t i = 0; i < s.indices.size(); ++i) {
    const size_t tri = i / 3;
    const uint32_t a = m.indices[3 * tri];
    CHECK(dot(s.normals[s.indices[i]], m.normals[a]) == doctest::Approx(1.0));
  }
  // A wider threshold smooths the sides as well.
  NormalOptions wide;
  wide.smooth_angle_deg = 70;
  NormalReport r2;
  smooth_normals(m, wide, &r2);
  CHECK(r2.hard_edges == 12);
}

TEST_CASE("normals: parts never share a normal, even when coplanar-ish") {
  const TexturedMesh m = prism(24, 5, 4, /*part_of_cap=*/1);
  NormalOptions o;
  o.smooth_angle_deg = 179;  // only the part border can keep the rim sharp
  NormalReport rep;
  const TexturedMesh s = smooth_normals(m, o, &rep);
  CHECK(rep.part_edges == 48);
  for (size_t i = 0; i < s.indices.size(); ++i)
    if ((i / 3) % 4 >= 2) CHECK(std::abs(s.normals[s.indices[i]].y) == doctest::Approx(1.0));
}

TEST_CASE("normals: vertices are shared again where normals agree") {
  const TexturedMesh m = prism(24, 5, 4);
  const TexturedMesh s = smooth_normals(m);
  // Side: 48 rim vertices; caps: 2 × (24 rim + 1 centre). Flat input had 3 per triangle.
  CHECK(s.positions.size() == 48 + 50);
  CHECK(s.uvs.size() == s.positions.size());
  CHECK(s.tri_part == m.tri_part);
  CHECK(s.texture == m.texture);
}
