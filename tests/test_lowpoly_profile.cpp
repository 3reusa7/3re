#include <doctest/doctest.h>

#include <cmath>

#include "pixel3d/lowpoly/profile.hpp"
#include "pixel3d/lowpoly/spine.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;

namespace {

// Pixels whose centre lies inside the ellipse (cx, cy, rx, ry).
Mask2D ellipse_mask(int w, int h, double cx, double cy, double rx, double ry) {
  Mask2D m(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double qx = (x + 0.5 - cx) / rx, qy = (y + 0.5 - cy) / ry;
      m.set(x, y, qx * qx + qy * qy <= 1.0);
    }
  return m;
}

// A capsule from a to b with radius r.
Mask2D capsule_mask(int w, int h, vec2d a, vec2d b, double r) {
  Mask2D m(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const vec2d p{x + 0.5, y + 0.5}, ab = b - a;
      const double t = std::clamp(dot(p - a, ab) / dot(ab, ab), 0.0, 1.0);
      m.set(x, y, length(p - (a + ab * t)) <= r);
    }
  return m;
}

}  // namespace

TEST_CASE("lowpoly profile: distance transform of a disk peaks at its radius") {
  const Mask2D m = ellipse_mask(41, 41, 20.5, 20.5, 15, 15);
  const DistanceField dt = distance_transform(m);
  CHECK(dt.at(20, 20) == doctest::Approx(15).epsilon(0.05));
  CHECK(dt.at(0, 0) == 0);
  CHECK(dt.sample({20.5, 20.5}) == doctest::Approx(15).epsilon(0.05));
}

TEST_CASE("lowpoly profile: straight spine through an ellipse measures its width") {
  const Mask2D m = ellipse_mask(60, 80, 30, 40, 20, 30);
  const Spine sp = straight_spine(m, {0, -1});
  REQUIRE(sp.pts.size() > 10);
  const Polygons2D outline = part_outline(m);
  const auto st = measure_widths(sp, outline, /*outer_extent=*/true);
  REQUIRE(st.size() == sp.pts.size());
  int checked = 0;
  for (const auto& s : st) {
    const double dy = (s.centre.y - 40) / 30;
    if (std::abs(dy) > 0.9) continue;  // the pixel staircase dominates near the poles
    CHECK(s.half_w == doctest::Approx(20 * std::sqrt(1 - dy * dy)).epsilon(0.06));
    CHECK(s.centre.x == doctest::Approx(30).epsilon(0.02));
    ++checked;
  }
  CHECK(checked > 50);
  // The spine runs upwards: the normal points to the image right (world +X).
  CHECK(sp.normals.front().x == doctest::Approx(1));
}

TEST_CASE("lowpoly profile: medial spine follows a diagonal capsule") {
  const vec2d a{12, 50}, b{50, 12};
  const Mask2D m = capsule_mask(64, 64, a, b, 5);
  const Spine sp = medial_spine(m);
  REQUIRE(sp.pts.size() > 20);
  // The ends reach the capsule's rounded tips (within a pixel).
  const double reach = length(a - b) + 2 * 5;
  CHECK(sp.length() == doctest::Approx(reach).epsilon(0.08));
  const vec2d axis = (b - a) * (1 / length(b - a));
  const auto st = measure_widths(sp, part_outline(m), /*outer_extent=*/false);
  int mid = 0;
  for (size_t i = 0; i < st.size(); ++i) {
    const double t = dot(st[i].centre - a, axis) / length(b - a);
    if (t < 0.2 || t > 0.8) continue;
    CHECK(std::abs(dot(sp.normals[i], axis)) < 0.15);
    CHECK(st[i].half_w == doctest::Approx(5).epsilon(0.15));
    ++mid;
  }
  CHECK(mid > 20);
}

TEST_CASE("lowpoly profile: depth from a side mask is measured, inflation is clamped") {
  const Mask2D front = ellipse_mask(40, 60, 20, 30, 12, 25);
  const Spine sp = straight_spine(front, {0, -1});
  auto st = measure_widths(sp, part_outline(front), true);
  Mask2D side(30, 60);
  for (int y = 5; y < 55; ++y)
    for (int x = 7; x < 21; ++x) side.set(x, y, true);
  DepthOptions o;
  o.box_depth = 30;
  o.side = &side;
  const std::string src = measure_depth(st, sp, distance_transform(front), o);
  CHECK(src == "side");
  for (const auto& s : st) {
    CHECK(s.measured);
    CHECK(s.z_c == doctest::Approx(14));
    CHECK(s.half_d == doctest::Approx(7));
  }

  DepthOptions inf;
  inf.box_depth = 30;
  inf.roundness = 1.0;
  inf.depth_limit = 10;  // the user's depth: half depth ≤ 5
  auto st2 = measure_widths(sp, part_outline(front), true);
  CHECK(measure_depth(st2, sp, distance_transform(front), inf) == "inflation");
  double max_hd = 0;
  for (const auto& s : st2) {
    CHECK_FALSE(s.measured);
    CHECK(s.z_c == doctest::Approx(15));
    max_hd = std::max(max_hd, s.half_d);
  }
  CHECK(max_hd == doctest::Approx(5));
}

TEST_CASE("lowpoly profile: a neutral depth cue changes nothing, a thin cue thins") {
  const Mask2D front = ellipse_mask(40, 60, 20, 30, 12, 25);
  const Spine sp = straight_spine(front, {0, -1});
  const DistanceField dt = distance_transform(front);
  DepthOptions o;
  o.box_depth = 60;
  o.roundness = 0.8;
  auto plain = measure_widths(sp, part_outline(front), true);
  measure_depth(plain, sp, dt, o);

  DepthCue cue;
  cue.w = 40;
  cue.h = 60;
  cue.values.assign(40 * 60, 0.7f);  // uniform: no relative shape information
  o.cue = &cue;
  auto neutral = measure_widths(sp, part_outline(front), true);
  measure_depth(neutral, sp, dt, o);
  for (size_t i = 0; i < plain.size(); ++i) CHECK(neutral[i].half_d == doctest::Approx(plain[i].half_d));

  for (int y = 0; y < 30; ++y)  // upper half half as thick as the lower half
    for (int x = 0; x < 40; ++x) cue.values[static_cast<size_t>(y) * 40 + x] = 0.35f;
  auto shaped = measure_widths(sp, part_outline(front), true);
  measure_depth(shaped, sp, dt, o);
  double upper = 0, lower = 0;
  for (size_t i = 0; i < shaped.size(); ++i) {
    if (shaped[i].centre.y < 25) upper = std::max(upper, shaped[i].half_d / plain[i].half_d);
    if (shaped[i].centre.y > 35) lower = std::max(lower, shaped[i].half_d / plain[i].half_d);
  }
  CHECK(upper < lower);
}

TEST_CASE("lowpoly profile: the station reducer keeps fixed stations and refines the worst deviation first") {
  const Mask2D m = capsule_mask(40, 90, {20, 10}, {20, 80}, 6);
  Mask2D bulged = m;  // a bulge (a knee) halfway
  for (int y = 40; y < 50; ++y)
    for (int x = 11; x < 29; ++x) bulged.set(x, y, true);
  const Spine sp = straight_spine(bulged, {0, -1});
  auto st = measure_widths(sp, part_outline(bulged), true);
  DepthOptions o;
  o.box_depth = 20;
  measure_depth(st, sp, distance_transform(bulged), o);
  const int fixed = static_cast<int>(st.size() / 3);
  const Reduction r = reduce_stations(st, {fixed}, {});
  REQUIRE(!r.errors.empty());
  // Greedy insertion is not strictly monotone (a new chord can deviate more
  // elsewhere in its span), but it starts at the worst and ends exact.
  for (double e : r.errors) CHECK(e <= r.errors.front() + 1e-12);
  CHECK(r.errors.back() == doctest::Approx(0).epsilon(1e-9));
  CHECK(std::find(r.initial.begin(), r.initial.end(), fixed) != r.initial.end());
  // The bulge corners (the knee) are among the first refinements, after the rounded ends.
  REQUIRE(r.order.size() >= 8);
  int bulge = 0;
  for (int k = 0; k < 8; ++k) {
    const double y = st[static_cast<size_t>(r.order[static_cast<size_t>(k)])].centre.y;
    bulge += (y > 38 && y < 52);
  }
  CHECK(bulge >= 2);
}

TEST_CASE("lowpoly profile: forbidden stations are never selected") {
  const Mask2D m = ellipse_mask(40, 80, 20, 40, 12, 35);
  const Spine sp = straight_spine(m, {0, -1});
  auto st = measure_widths(sp, part_outline(m), true);
  std::vector<bool> forbidden(st.size(), false);
  for (size_t i = st.size() / 2 - 5; i < st.size() / 2 + 5; ++i) forbidden[i] = true;
  const Reduction r = reduce_stations(st, {}, forbidden);
  for (int i : r.order) CHECK_FALSE(forbidden[static_cast<size_t>(i)]);
}
