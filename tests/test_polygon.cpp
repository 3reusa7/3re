#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <random>

#include "pixel3d/image/image.hpp"
#include "pixel3d/polygon/contour.hpp"
#include "pixel3d/polygon/raster.hpp"
#include "pixel3d/polygon/simplify.hpp"

using namespace pixel3d;

namespace {

Mask2D mask_from(int w, int h, const std::function<bool(int, int)>& f) {
  Mask2D m(w, h);
  for (int v = 0; v < h; ++v)
    for (int u = 0; u < w; ++u) m.set(u, v, f(u, v));
  return m;
}

size_t vertex_count(const Polygons2D& p) {
  size_t n = 0;
  for (const auto& r : p) n += r.size();
  return n;
}

Polygons2D simplify(const Mask2D& m, SimplifyStats* st = nullptr) {
  return simplify_feature_preserving(mask_to_polygons(m), m, SimplifyOptions{}, st);
}

// True if some edge of the polygons has |dy/dx| == ratio exactly and length > min_len.
bool has_edge_with_slope(const Polygons2D& p, double ratio, double min_len) {
  for (const auto& r : p)
    for (size_t i = 0; i < r.size(); ++i) {
      const vec2d d = r[(i + 1) % r.size()] - r[i];
      if (d.x != 0 && std::abs(std::abs(d.y / d.x) - ratio) < 1e-12 && length(d) > min_len) return true;
    }
  return false;
}

}  // namespace

TEST_CASE("polygon: rectangle stays 4 vertices") {
  const Mask2D m = mask_from(7, 5, [](int u, int v) { return u >= 1 && u < 6 && v >= 1 && v < 4; });
  const Polygons2D p = mask_to_polygons(m);
  REQUIRE(p.size() == 1);
  CHECK(p[0].size() == 4);
  CHECK(area(p) == doctest::Approx(15));
  CHECK(rasterize_pixel_centres(p, 7, 5) == m);
  CHECK(vertex_count(simplify(m)) == 4);
}

TEST_CASE("polygon: 45 degree staircase becomes one slope") {
  const Mask2D m = mask_from(8, 8, [](int u, int v) { return u <= v; });
  SimplifyStats st;
  const Polygons2D s = simplify(m, &st);
  CHECK(vertex_count(s) <= 5);  // the slope ends on a unit-step midpoint at the corner pixel
  CHECK(st.staircases_replaced >= 1);
  CHECK(has_edge_with_slope(s, 1.0, 5.0));
  CHECK(rasterize_pixel_centres(s, 8, 8) == m);
}

TEST_CASE("polygon: 1:2 staircase snaps to exact slope 0.5") {
  const Mask2D m = mask_from(12, 7, [](int u, int v) { return 2 * v >= u; });
  const Polygons2D s = simplify(m);
  CHECK(has_edge_with_slope(s, 0.5, 4.0));
  CHECK(rasterize_pixel_centres(s, 12, 7) == m);
}

TEST_CASE("polygon: L corner is preserved") {
  const Mask2D m = mask_from(10, 10, [](int u, int v) { return u < 3 || v >= 7; });
  const Polygons2D s = simplify(m);
  REQUIRE(s.size() == 1);
  CHECK(s[0].size() == 6);
  CHECK(rasterize_pixel_centres(s, 10, 10) == m);
}

TEST_CASE("polygon: single step and single notch are preserved") {
  const Mask2D step = mask_from(10, 6, [](int u, int v) { return v >= (u < 5 ? 2 : 3); });
  CHECK(vertex_count(simplify(step)) == vertex_count(mask_to_polygons(step)));
  const Mask2D notch = mask_from(9, 4, [](int u, int v) { return !(u == 4 && v == 0); });
  CHECK(vertex_count(simplify(notch)) == vertex_count(mask_to_polygons(notch)));
}

TEST_CASE("polygon: pixel circle radius 8 becomes polyline not staircase") {
  const Mask2D m = mask_from(17, 17, [](int u, int v) {
    const double dx = u + 0.5 - 8.5, dy = v + 0.5 - 8.5;
    return dx * dx + dy * dy <= 8.0 * 8.0;
  });
  SimplifyStats st;
  const Polygons2D exact = mask_to_polygons(m);
  const Polygons2D s = simplify_feature_preserving(exact, m, SimplifyOptions{}, &st);
  // Only the four 1-px extreme flats may remain as unit steps; everything else is sloped.
  int unit_steps = 0;
  for (const auto& r : s)
    for (size_t i = 0; i < r.size(); ++i) {
      const vec2d d = r[(i + 1) % r.size()] - r[i];
      if ((d.x == 0 || d.y == 0) && std::abs(length(d) - 1) < 1e-9) ++unit_steps;
    }
  CHECK(unit_steps <= 4);
  CHECK(vertex_count(s) <= vertex_count(exact) * 3 / 4);
  CHECK(rasterize_pixel_centres(s, 17, 17) == m);
  CHECK(st.max_deviation <= 0.5 + 1e-9);
}

TEST_CASE("polygon: holes survive") {
  const Mask2D m = mask_from(6, 6, [](int u, int v) { return !(u >= 2 && u < 4 && v >= 2 && v < 4); });
  const Polygons2D p = mask_to_polygons(m);
  CHECK(p.size() == 2);
  CHECK(area(p) == doctest::Approx(32));
  CHECK(rasterize_pixel_centres(simplify(m), 6, 6) == m);
}

TEST_CASE("polygon: property - random blobs rasterize back exactly") {
  std::mt19937 rng(1234);
  std::uniform_int_distribution<int> coin(0, 99);
  int replaced = 0;
  for (int t = 0; t < 200; ++t) {
    // Smoothish random blobs: threshold a few random discs so staircases occur.
    std::vector<std::array<double, 3>> discs;
    for (int k = 0; k < 3; ++k)
      discs.push_back({1.0 + coin(rng) % 10, 1.0 + coin(rng) % 10, 1.5 + (coin(rng) % 40) / 10.0});
    const Mask2D m = mask_from(12, 12, [&](int u, int v) {
      for (auto [cx, cy, r] : discs)
        if ((u + 0.5 - cx) * (u + 0.5 - cx) + (v + 0.5 - cy) * (v + 0.5 - cy) <= r * r) return true;
      return coin(rng) < 3;  // sprinkle noise pixels
    });
    SimplifyStats st;
    const Polygons2D s = simplify_feature_preserving(mask_to_polygons(m), m, SimplifyOptions{}, &st);
    replaced += st.staircases_replaced;
    REQUIRE(rasterize_pixel_centres(s, 12, 12) == m);
    CHECK(st.max_deviation <= 0.5 + 1e-9);
  }
  CHECK(replaced > 50);  // the property is exercised, not vacuous
}

TEST_CASE("polygon: clip intersects polygons") {
  const Polygons2D a = rect_polys(0, 0, 4, 4), b = rect_polys(2, 1, 6, 3);
  CHECK(area(clip(a, b)) == doctest::Approx(4));
}

TEST_CASE("polygon: fixture A side back becomes one 1:4 slope on each face") {
  const auto img = load_png(std::string(PIXEL3D_FIXTURES_DIR) + "/A_synthetic_chair/side.png");
  REQUIRE(img.has_value());
  const Mask2D m = opaque_mask(*img);
  SimplifyStats st;
  const Polygons2D s = simplify_feature_preserving(mask_to_polygons(m), m, SimplifyOptions{}, &st);
  CHECK(rasterize_pixel_centres(s, m.width(), m.height()) == m);
  int slopes = 0;
  for (const auto& r : s)
    for (size_t i = 0; i < r.size(); ++i) {
      const vec2d d = r[(i + 1) % r.size()] - r[i];
      if (d.x != 0 && std::abs(std::abs(d.y / d.x) - 4.0) < 1e-12 && length(d) > 10) ++slopes;
    }
  CHECK(slopes == 2);  // front and rear face of the reclined back
}
