// Polished styles, step 5: mirror symmetry only from pixel-exact drawings.
#include <doctest/doctest.h>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/style/rounding.hpp"
#include "pixel3d/style/symmetry.hpp"

using namespace pixel3d;

namespace {

std::string fixture(const char* name, const char* view) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + view + ".png";
}

std::unique_ptr<Session> open(const char* fx) {
  auto s = std::make_unique<Session>();
  REQUIRE(s->new_project(ProjectSettings{}).success);
  REQUIRE(s->import_views({{ViewRole::Front, fixture(fx, "front"), {}},
                           {ViewRole::Side, fixture(fx, "side"), {}},
                           {ViewRole::Top, fixture(fx, "top"), {}}})
              .success);
  REQUIRE(s->validate(ValidateOptions{}).success);
  return s;
}

std::map<ViewRole, Mask2D> masks(const Document& d) {
  std::map<ViewRole, Mask2D> m;
  for (const SourceView& v : d.views) m.emplace(v.role, opaque_mask(*v.aligned, d.settings.alpha_threshold));
  return m;
}

manifold::Manifold box(double x0, double y0, double z0, double x1, double y1, double z1) {
  return manifold::Manifold::Cube({x1 - x0, y1 - y0, z1 - z0}).Translate({x0, y0, z0});
}

}  // namespace

TEST_CASE("symmetry: detected only when the drawings are exact mirror images") {
  const Dims d{6, 3, 2};
  Mask2D front(6, 3), top(6, 2), side(2, 3);
  for (int y = 0; y < 3; ++y) front.set(1, y, true), front.set(4, y, true);
  for (int z = 0; z < 2; ++z) top.set(1, z, true), top.set(4, z, true);
  side.set(0, 0, true);  // the side view says nothing about X
  std::map<ViewRole, Mask2D> m{{ViewRole::Front, front}, {ViewRole::Top, top}, {ViewRole::Side, side}};
  SymmetryResult r = detect_mirror_symmetry(m, d);
  CHECK(r.symmetric);
  CHECK(r.axis_x == doctest::Approx(3.0));
  CHECK(r.checked == std::vector<ViewRole>{ViewRole::Front, ViewRole::Top});
  // One pixel off: not symmetric, and the reason names the drawing and pixel.
  m[ViewRole::Top].set(0, 1, true);
  r = detect_mirror_symmetry(m, d);
  CHECK_FALSE(r.symmetric);
  CHECK(r.reason.find("top") != std::string::npos);
  CHECK(r.reason.find("(0, 1)") != std::string::npos);
}

TEST_CASE("symmetry: fixtures") {
  {
    auto s = open("D_character");
    CHECK(detect_mirror_symmetry(masks(s->document()), s->document().align->dims).symmetric);
  }
  {
    auto s = open("C_asym_planter");
    CHECK_FALSE(detect_mirror_symmetry(masks(s->document()), s->document().align->dims).symmetric);
  }
}

TEST_CASE("symmetry: pairs mirror their counterpart, crossing parts mirror their left half") {
  PartGeometry g;
  g.parts[0] = box(0, 0, 0, 2, 4, 2);                          // left
  g.parts[1] = box(8, 0, 0, 10, 4, 2.0000001);                 // right, almost the mirror of 0
  g.parts[2] = box(3, 0, 0, 7, 5, 3) - box(6, 4, 0, 7, 5, 3);  // crosses the plane, notch on the right
  g.parts[3] = box(8, 6, 0, 9, 7, 1);                          // right, no counterpart
  MirrorReport rep;
  const PartGeometry m = mirror_parts(g, 5.0, &rep);
  CHECK(rep.mirrored_pairs == 1);
  CHECK(rep.halved == 1);
  CHECK(rep.skipped == std::vector<int>{3});
  REQUIRE(m.parts.size() == 4);
  // Part 1 is now exactly the mirror image of part 0, with its own id.
  const manifold::Manifold mirror0 = g.parts[0].Mirror({1, 0, 0}).Translate({10, 0, 0});
  CHECK((m.parts.at(1) - mirror0).Volume() < 1e-9);
  CHECK((mirror0 - m.parts.at(1)).Volume() < 1e-9);
  // Part 2 is symmetric: its right side is the left side mirrored (no notch).
  const manifold::Manifold p2 = m.parts.at(2);
  const manifold::Manifold p2m = p2.Mirror({1, 0, 0}).Translate({10, 0, 0});
  CHECK((p2 - p2m).Volume() < 1e-9);
  CHECK(p2.Volume() == doctest::Approx(4 * 5 * 3));
  CHECK(p2.Status() == manifold::Manifold::Error::NoError);
  CHECK((m.parts.at(3) - g.parts[3]).Volume() < 1e-12);
}

TEST_CASE("symmetry: the rounded character mirrored keeps its silhouettes and stays closed") {
  auto s = open("D_character");
  const Document& d = s->document();
  const SymmetryResult sym = detect_mirror_symmetry(masks(d), d.align->dims);
  REQUIRE(sym.symmetric);
  const PartGeometry rounded = round_parts(*d.edited_parts, CornerOptions{});
  MirrorReport rep;
  const PartGeometry m = mirror_parts(rounded, sym.axis_x, &rep);
  CHECK(rep.skipped.empty());
  CHECK(rep.mirrored_pairs + rep.halved == static_cast<int>(m.parts.size()) - rep.mirrored_pairs);
  const SolidResult solid = assemble_parts(m, d.cleanup_options);
  CHECK(solid.topology.watertight);
  CHECK(solid.topology.nonmanifold_edges == 0);
  CHECK(solid.topology.inconsistent_winding_edges == 0);
  CHECK((m.all() - d.edited_parts->all()).Volume() < 1e-6);
  for (ViewRole role : {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Back, ViewRole::Right}) {
    CAPTURE(to_string(role));
    const Camera cam = camera_for_view(role, d.align->dims, 1);
    CHECK(render_mesh_flat(solid.mesh, cam).coverage == render_mesh_flat(d.solid->mesh, cam).coverage);
  }
}
