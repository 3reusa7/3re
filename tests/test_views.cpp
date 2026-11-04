#include <doctest/doctest.h>

#include <random>

#include "pixel3d/views/view_role.hpp"
#include "test_helpers.hpp"

using namespace pixel3d;

namespace {
constexpr ViewRole kAll[] = {ViewRole::Front, ViewRole::Back, ViewRole::Side,
                             ViewRole::Right, ViewRole::Top,  ViewRole::Bottom};
}

TEST_CASE("views: mapping round trips for all six roles") {
  const Dims d{5, 7, 3};
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> ux(0, 5), uy(0, 7), uz(0, 3);
  for (ViewRole r : kAll)
    for (int i = 0; i < 50; ++i) {
      const vec3d p{ux(rng), uy(rng), uz(rng)};
      const vec3d uvd = view_from_world(r, d, p);
      const vec3d q = world_from_view(r, d, uvd.x, uvd.y, uvd.z);
      CHECK(q.x == doctest::Approx(p.x).epsilon(1e-12));
      CHECK(q.y == doctest::Approx(p.y).epsilon(1e-12));
      CHECK(q.z == doctest::Approx(p.z).epsilon(1e-12));
    }
}

TEST_CASE("views: front pixel centre maps to expected world point") {
  const vec3d p = world_from_view(ViewRole::Front, Dims{4, 6, 3}, 0.5, 0.5, 0.0);
  CHECK(p.x == 0.5);
  CHECK(p.y == 5.5);
  CHECK(p.z == 3.0);
}

TEST_CASE("views: top image down is +Z") {
  const vec3d p = world_from_view(ViewRole::Top, Dims{4, 6, 3}, 0.5, 2.5, 0.0);
  CHECK(p.z == 2.5);
  CHECK(p.y == 6.0);
}

TEST_CASE("views: side image right is +Z and depth runs along +X") {
  const vec3d p = world_from_view(ViewRole::Side, Dims{4, 6, 3}, 2.5, 0.5, 1.0);
  CHECK(p.z == 2.5);
  CHECK(p.x == 1.0);
  CHECK(view_dir(ViewRole::Side).x == 1.0);
}

TEST_CASE("views: image sizes per role") {
  const Dims d{4, 6, 3};
  CHECK(view_image_size(ViewRole::Front, d).w == 4);
  CHECK(view_image_size(ViewRole::Side, d).w == 3);
  CHECK(view_image_size(ViewRole::Top, d).h == 3);
  CHECK(view_image_size(ViewRole::Right, d).h == 6);
}

TEST_CASE("views: helper projection agrees with world_from_view at pixel centres") {
  const Dims d{5, 7, 3};
  for (ViewRole r : kAll) {
    const auto [u, v] = p3dtest::project_cell(r, d, 1, 2, 0);
    const vec3d c{1.5, 2.5, 0.5};
    const vec3d uvd = view_from_world(r, d, c);
    CHECK(uvd.x == doctest::Approx(u + 0.5));
    CHECK(uvd.y == doctest::Approx(v + 0.5));
    CHECK(uvd.z == doctest::Approx(p3dtest::cell_depth(r, d, 1, 2, 0)));
  }
}

TEST_CASE("views: role names round trip") {
  for (ViewRole r : kAll) CHECK(view_role_from_string(to_string(r)) == r);
  CHECK_FALSE(view_role_from_string("diagonal").has_value());
}
