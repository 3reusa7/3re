// Polished styles, step 3: small bevels on corners that stay sharp after rounding.
#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

#include "pixel3d/polygon/raster.hpp"
#include "pixel3d/style/rounding.hpp"

using namespace pixel3d;

namespace {

bool same_pixels(const Polygons2D& a, const Polygons2D& b) {
  auto r = [](Polygons2D p) {
    for (Ring& ring : p)
      for (vec2d& v : ring) v = {v.x + 4, v.y + 4};
    return rasterize_pixel_centres(p, 48, 48);
  };
  return r(a) == r(b);
}

double outside_area(const Polygons2D& result, const Polygons2D& original) {
  return std::abs(area(subtract(result, original)));
}

manifold::Manifold box(double x0, double y0, double z0, double x1, double y1, double z1) {
  return manifold::Manifold::Cube({x1 - x0, y1 - y0, z1 - z0}).Translate({x0, y0, z0});
}

// Pixel-centre raster of a solid's projection along one axis (0 = Z, 1 = X, 2 = Y).
Mask2D projection(const manifold::Manifold& m, int axis) {
  const manifold::mat3x4 rot[3] = {manifold::mat3x4({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}),
                                   manifold::mat3x4({0, 0, -1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 0}),
                                   manifold::mat3x4({1, 0, 0}, {0, 0, 1}, {0, -1, 0}, {0, 0, 0})};
  Polygons2D p;
  for (const auto& sp : m.Transform(rot[axis]).Project()) {
    Ring r;
    for (const auto& v : sp) r.push_back({v.x + 40, v.y + 40});
    p.push_back(r);
  }
  return rasterize_pixel_centres(unite(p, {}), 80, 80);
}

}  // namespace

TEST_CASE("bevel: thin features get a small pixel-safe bevel instead of a sharp corner") {
  CornerOptions o;
  o.bevel = 0.2;
  for (double width : {1.0, 2.0}) {
    CAPTURE(width);
    const Polygons2D strip = rect_polys(0, 0, width, 9);
    CornerReport rep;
    const Polygons2D r = round_corners(strip, o, &rep);
    CHECK(rep.thin == 4);
    CHECK(rep.beveled == 4);
    CHECK(rep.rounded == 0);
    CHECK(rep.restored_pixels == 0);
    CHECK(width * 9 - area(r) == doctest::Approx(4 * (1 - std::numbers::pi / 4) * 0.04).epsilon(0.1));
    CHECK(same_pixels(r, strip));
    CHECK(outside_area(r, strip) < 1e-9);
  }
  // Chamfer bevel (lowpoly): a straight cut of 0.2 px.
  o.exponent = 1;
  CornerReport rep;
  const Polygons2D r = round_corners(rect_polys(0, 0, 2, 9), o, &rep);
  CHECK(rep.beveled == 4);
  CHECK(18 - area(r) == doctest::Approx(4 * 0.5 * 0.04));
  // No bevel by default: crisp corners stay exactly as they are.
  CornerReport none;
  round_corners(rect_polys(0, 0, 2, 9), CornerOptions{}, &none);
  CHECK(none.beveled == 0);
}

TEST_CASE("bevel 3D: thin parts are bevelled, stay closed, inside and pixel-exact") {
  const manifold::Manifold plank = box(0, 0, 0, 2, 9, 1);
  CornerOptions o;
  o.bevel = 0.2;
  PartRoundReport rep;
  const manifold::Manifold r = round_part(plank, o, &rep);
  CHECK_FALSE(rep.fallback);
  CHECK(rep.corners.beveled == 12);
  CHECK(r.Status() == manifold::Manifold::Error::NoError);
  CHECK(r.Genus() == 0);
  CHECK(r.Volume() < plank.Volume());
  CHECK((r - plank).Volume() < 1e-6);
  for (int axis = 0; axis < 3; ++axis) CHECK(projection(r, axis) == projection(plank, axis));
}
