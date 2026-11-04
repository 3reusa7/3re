// Polished styles, step 2: rounded volumes inside the hull, silhouettes pixel-exact.
#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <random>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/polygon/raster.hpp"
#include "pixel3d/polygon/simplify.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/style/rounding.hpp"

using namespace pixel3d;

namespace {

// Pixel-centre raster of `p` on a grid with origin (ox, oy) (integers).
Mask2D raster(const Polygons2D& p, int ox, int oy, int w, int h) {
  Polygons2D q = p;
  for (Ring& r : q)
    for (vec2d& v : r) v = {v.x - ox, v.y - oy};
  return rasterize_pixel_centres(q, w, h);
}

bool same_pixels(const Polygons2D& a, const Polygons2D& b) {
  return raster(a, -4, -4, 48, 48) == raster(b, -4, -4, 48, 48);
}

double outside_area(const Polygons2D& result, const Polygons2D& original) {
  return std::abs(area(subtract(result, original)));
}

Polygons2D from_rows(const std::vector<std::string>& rows) {
  Mask2D m(static_cast<int>(rows[0].size()), static_cast<int>(rows.size()));
  for (size_t y = 0; y < rows.size(); ++y)
    for (size_t x = 0; x < rows[y].size(); ++x) m.set(static_cast<int>(x), static_cast<int>(y), rows[y][x] == '#');
  return mask_to_polygons(m);
}

}  // namespace

TEST_CASE("rounding 2D: a square gets four circular corners of the requested radius") {
  const Polygons2D sq = rect_polys(0, 0, 10, 10);
  CornerOptions o;
  o.radius = 1.5;
  CornerReport rep;
  const Polygons2D r = round_corners(sq, o, &rep);
  CHECK(rep.corners == 4);
  CHECK(rep.rounded == 4);
  CHECK(rep.clamped == 0);
  CHECK(rep.restored_pixels == 0);
  CHECK(rep.max_size == doctest::Approx(1.5));
  // Area lost = 4 (1 − π/4) r² (minus a little for the polygonal arc).
  const double lost = 100 - area(r), exact = 4 * (1 - std::numbers::pi / 4) * 1.5 * 1.5;
  CHECK(lost == doctest::Approx(exact).epsilon(0.08));
  CHECK(outside_area(r, sq) < 1e-9);
  CHECK(same_pixels(r, sq));
}

TEST_CASE("rounding 2D: a radius the pixels cannot take is clamped, never a lost centre") {
  const Polygons2D sq = rect_polys(0, 0, 10, 10);
  for (double exponent : {1.0, 2.0, 4.0}) {
    CAPTURE(exponent);
    CornerOptions o;
    o.radius = 4;
    o.exponent = exponent;
    CornerReport rep;
    const Polygons2D r = round_corners(sq, o, &rep);
    CHECK(rep.rounded == 4);
    CHECK(rep.clamped == 4);
    CHECK(rep.restored_pixels == 0);
    // Circular: the corner pixel centre (0.5, 0.5) allows r < 1.707; chamfer: t < 1.
    if (exponent == 2.0) CHECK(rep.max_size < 1.71);
    if (exponent == 1.0) CHECK(rep.max_size < 1.0);
    CHECK(rep.max_size > 0.5);
    CHECK(same_pixels(r, sq));
    CHECK(outside_area(r, sq) < 1e-9);
  }
}

TEST_CASE("rounding 2D: thin features stay sharp") {
  const Polygons2D strip = rect_polys(0, 0, 2, 9);
  CornerReport rep;
  const Polygons2D r = round_corners(strip, CornerOptions{}, &rep);
  CHECK(rep.corners == 4);
  CHECK(rep.thin == 4);
  CHECK(rep.rounded == 0);
  CHECK(area(r) == doctest::Approx(18));
}

TEST_CASE("rounding 2D: large drawn shapes round their steps; a 3 px bar is not thin") {
  const Polygons2D blob = from_rows({"...####...", ".########.", ".########.", "##########", "##########",
                                     "##########", ".########.", "...####..."});
  CornerReport rep;
  const Polygons2D r = round_corners(blob, CornerOptions{}, &rep);
  CHECK(rep.thin == 0);
  CHECK(rep.rounded == rep.corners);
  CHECK(rep.restored_pixels == 0);
  CHECK(same_pixels(r, blob));
  CornerReport bar;
  round_corners(rect_polys(0, 0, 3, 9), CornerOptions{}, &bar);
  CHECK(bar.thin == 0);
  CHECK(bar.rounded == 4);
}

TEST_CASE("rounding 2D: concave corners and holes are untouched") {
  // L shape: 5 convex corners, 1 concave.
  const Polygons2D l = from_rows({"####....", "####....", "####....", "########", "########", "########"});
  CornerReport rep;
  const Polygons2D r = round_corners(l, CornerOptions{}, &rep);
  CHECK(rep.corners == 5);
  CHECK(rep.restored_pixels == 0);
  CHECK(same_pixels(r, l));
  CHECK(outside_area(r, l) < 1e-9);
  // A ring with a hole: the hole's corners are concave seen from the solid.
  const Polygons2D ring = subtract(rect_polys(0, 0, 9, 9), rect_polys(3, 3, 6, 6));
  CornerReport rr;
  const Polygons2D r2 = round_corners(ring, CornerOptions{}, &rr);
  CHECK(rr.corners == 4);
  CHECK(same_pixels(r2, ring));
}

TEST_CASE("rounding 2D: random pixel shapes keep every pixel centre without restoring any") {
  std::mt19937 rng(1234);
  int restored = 0, rounded = 0;
  for (int trial = 0; trial < 120; ++trial) {
    Mask2D m(14, 14);
    // Union of random rectangles: corners of every kind, steps, thin bits.
    for (int k = 0; k < 4; ++k) {
      const int x0 = rng() % 10, y0 = rng() % 10, w = 1 + rng() % 6, h = 1 + rng() % 6;
      for (int y = y0; y < std::min(14, y0 + h); ++y)
        for (int x = x0; x < std::min(14, x0 + w); ++x) m.set(x, y, true);
    }
    const Polygons2D exact = mask_to_polygons(m);
    const Polygons2D simple = simplify_feature_preserving(exact, m, SimplifyOptions{});
    for (const Polygons2D* p : {&exact, &simple})
      for (double radius : {0.6, 1.5, 3.0})
        for (double exponent : {1.0, 2.0, 4.0}) {
          CornerOptions o;
          o.radius = radius;
          o.exponent = exponent;
          CornerReport rep;
          const Polygons2D r = round_corners(*p, o, &rep);
          CAPTURE(trial);
          CAPTURE(radius);
          CAPTURE(exponent);
          CHECK(rasterize_pixel_centres(r, 14, 14) == m);
          CHECK(outside_area(r, *p) < 1e-7);
          restored += rep.restored_pixels;
          rounded += rep.rounded;
        }
  }
  CHECK(restored == 0);
  CHECK(rounded > 1000);
}

namespace {

manifold::Manifold box(double x0, double y0, double z0, double x1, double y1, double z1) {
  return manifold::Manifold::Cube({x1 - x0, y1 - y0, z1 - z0}).Translate({x0, y0, z0});
}

// Pixel-centre raster of a solid's projection along one axis (0 = Z/Front, 1 = X/Side, 2 = Y/Top).
Mask2D projection(const manifold::Manifold& m, int axis) {
  const manifold::mat3x4 rot[3] = {
      manifold::mat3x4({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}),
      manifold::mat3x4({0, 0, -1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 0}),   // (x,y,z) → (z, y, −x)
      manifold::mat3x4({1, 0, 0}, {0, 0, 1}, {0, -1, 0}, {0, 0, 0})};  // (x,y,z) → (x, −z, y)
  Polygons2D p;
  for (const auto& sp : m.Transform(rot[axis]).Project()) {
    Ring r;
    for (const auto& v : sp) r.push_back({v.x, v.y});
    p.push_back(r);
  }
  return raster(unite(p, {}), -40, -40, 80, 80);
}

}  // namespace

TEST_CASE("rounding 3D: a box part is rounded inside itself, closed, with exact projections") {
  const manifold::Manifold b = box(0, 0, 0, 10, 12, 8);
  PartRoundReport rep;
  const manifold::Manifold r = round_part(b, CornerOptions{}, &rep);
  CHECK_FALSE(rep.fallback);
  CHECK(r.Status() == manifold::Manifold::Error::NoError);
  CHECK(r.Genus() == 0);
  CHECK(r.Volume() < b.Volume() - 5);
  CHECK((r - b).Volume() < 1e-6);
  CHECK(rep.corners.rounded == 12);  // four corners in each of the three projections
  for (int axis = 0; axis < 3; ++axis) {
    CAPTURE(axis);
    CHECK(projection(r, axis) == projection(b, axis));
  }
}

TEST_CASE("rounding 3D: thin parts and empty parts pass through unchanged") {
  const manifold::Manifold plank = box(0, 0, 0, 2, 9, 1);
  PartRoundReport rep;
  const manifold::Manifold r = round_part(plank, CornerOptions{}, &rep);
  CHECK(r.Volume() == doctest::Approx(plank.Volume()));
  CHECK(rep.corners.rounded == 0);
  CHECK(round_part(manifold::Manifold(), CornerOptions{}).IsEmpty());
}

namespace {

std::string fixture(const char* name, const char* view) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + view + ".png";
}

}  // namespace

TEST_CASE("rounding 3D: every fixture keeps its silhouettes, closed and manifold") {
  for (const char* fx : {"A_synthetic_chair", "B_hand_chair", "C_asym_planter", "D_character"}) {
    const std::string name = fx;
    CAPTURE(name);
    Session s;
    REQUIRE(s.new_project(ProjectSettings{}).success);
    REQUIRE(s.import_views({{ViewRole::Front, fixture(fx, "front"), {}},
                            {ViewRole::Side, fixture(fx, "side"), {}},
                            {ViewRole::Top, fixture(fx, "top"), {}}})
                .success);
    REQUIRE(s.validate(ValidateOptions{}).success);
    const Document& d = s.document();
    for (double exponent : {1.0, 2.0}) {
      CAPTURE(exponent);
      CornerOptions o;
      o.exponent = exponent;
      std::vector<PartRoundReport> reps;
      const PartGeometry g = round_parts(*d.edited_parts, o, &reps);
      int rounded = 0, restored = 0;
      for (const auto& rp : reps) {
        CAPTURE(rp.part);
        CAPTURE(rp.reason);
        CHECK_FALSE(rp.fallback);
        rounded += rp.corners.rounded;
        restored += rp.corners.restored_pixels;
      }
      CHECK(rounded > 0);
      // B's legs (parts 0–3) are 2 × 2 px sticks: thin, so they stay exactly as drawn.
      if (name == "B_hand_chair")
        for (const auto& rp : reps)
          if (rp.part <= 3) CHECK(rp.corners.rounded == 0);
      CHECK(restored == 0);
      const SolidResult solid = assemble_parts(g, d.cleanup_options);
      CHECK(solid.topology.watertight);
      CHECK(solid.topology.nonmanifold_edges == 0);
      CHECK(solid.topology.nonmanifold_vertices == 0);
      CHECK(solid.topology.inconsistent_winding_edges == 0);
      CHECK(solid.topology.signed_volume > 0);
      CHECK(solid.topology.signed_volume <= d.solid->topology.signed_volume + 1e-6);
      // Inside the crisp solid, and the same pixel-centre silhouette in every view.
      CHECK((g.all() - d.edited_parts->all()).Volume() < 1e-6);
      for (ViewRole role : {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Back, ViewRole::Right}) {
        CAPTURE(to_string(role));
        const Camera cam = camera_for_view(role, d.align->dims, 1);
        CHECK(render_mesh_flat(solid.mesh, cam).coverage == render_mesh_flat(d.solid->mesh, cam).coverage);
      }
    }
  }
}

TEST_CASE("rounding 3D: parts stacked into one shape get no groove at their joint") {
  // A post split into two parts (as correspondence bands do), and a wider block
  // sitting on a narrower one (a head on a torso): only the second keeps its step.
  PartGeometry g;
  g.parts[0] = box(0, 0, 0, 4, 5, 4);
  g.parts[1] = box(0, 5, 0, 4, 10, 4);
  g.parts[2] = box(10, 0, 0, 14, 5, 4);
  g.parts[3] = box(9, 5, 0, 15, 10, 5);
  std::vector<PartRoundReport> reps;
  const PartGeometry r = round_parts(g, CornerOptions{}, &reps);
  const manifold::Manifold post = r.parts.at(0) + r.parts.at(1);
  // Just beside the joint on the post's outer edges: still solid (no notch).
  for (const manifold::vec3& p : {manifold::vec3{0.05, 5.0, 2}, manifold::vec3{3.95, 5.0, 2}, manifold::vec3{2, 5.0, 0.05}}) {
    CAPTURE(p.x);
    CAPTURE(p.z);
    CHECK((post ^ manifold::Manifold::Cube({0.02, 0.02, 0.02}, true).Translate(p)).Volume() > 7e-6);
  }
  // The post's free ends are still rounded.
  CHECK((post ^ manifold::Manifold::Cube({0.02, 0.02, 0.02}, true).Translate({0.05, 9.95, 2})).IsEmpty());
  int joined = 0;
  for (const auto& rp : reps) joined += rp.corners.joined;
  CHECK(joined > 0);
  // The wider block's corners over the narrower one are free corners: rounded.
  CHECK((r.parts.at(3) ^ manifold::Manifold::Cube({0.02, 0.02, 0.02}, true).Translate({9.05, 5.05, 2})).IsEmpty());
}
