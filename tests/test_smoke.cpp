#include <doctest/doctest.h>

#include <manifold/manifold.h>

#include "pixel3d/common/math.hpp"

TEST_CASE("smoke: manifold cube intersection volume") {
  auto a = manifold::Manifold::Cube({2, 2, 2});
  auto b = manifold::Manifold::Cube({2, 2, 2}).Translate({1, 1, 1});
  auto c = a ^ b;
  CHECK(c.Volume() == doctest::Approx(1.0));
  CHECK(c.Status() == manifold::Manifold::Error::NoError);
}

TEST_CASE("smoke: vec3d cross") {
  CHECK(pixel3d::cross(pixel3d::vec3d{1, 0, 0}, pixel3d::vec3d{0, 1, 0}).z == 1.0);
}
