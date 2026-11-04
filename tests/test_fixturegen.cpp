#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include "fixturegen/gt_model.hpp"
#include "fixturegen/gt_render.hpp"
#include "fixturegen/hand_maps.hpp"
#include "pixel3d/image/mask.hpp"
#include "pixel3d/image/palette.hpp"

using namespace pixel3d;
using namespace p3dfix;

namespace {

int runs_in_row(const ImageRGBA& img, int v) {
  int runs = 0;
  bool in = false;
  for (int u = 0; u < img.width(); ++u) {
    const bool o = img.at(u, v).a >= 128;
    if (o && !in) ++runs;
    in = o;
  }
  return runs;
}

int max_runs_in_rows(const ImageRGBA& img, int v0, int v1) {
  int m = 0;
  for (int v = v0; v < v1; ++v) m = std::max(m, runs_in_row(img, v));
  return m;
}

std::string read_text(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

TEST_CASE("fixturegen: A front silhouette has two leg strands below the seat") {
  const GroundTruth a = make_fixture_A();
  const ImageRGBA front = render_gt_view(a, ViewRole::Front);
  CHECK(runs_in_row(front, a.dims.H - 1 - 5) == 2);
  CHECK(runs_in_row(front, a.dims.H - 1 - 15) == 1);  // seat
}

TEST_CASE("fixturegen: A side shows the reclined back as a slope") {
  const GroundTruth a = make_fixture_A();
  const ImageRGBA side = render_gt_view(a, ViewRole::Side);
  auto first_opaque = [&](int v) {
    for (int u = 0; u < side.width(); ++u)
      if (side.at(u, v).a >= 128) return u;
    return -1;
  };
  // Back leans towards -Z (image left) as it rises: 1 px per 4 rows.
  CHECK(first_opaque(a.dims.H - 1 - 18) - first_opaque(a.dims.H - 1 - 30) == 3);
}

TEST_CASE("fixturegen: C front has three leg strands and side has two") {
  const GroundTruth c = make_fixture_C();
  const ImageRGBA front = render_gt_view(c, ViewRole::Front);
  const ImageRGBA side = render_gt_view(c, ViewRole::Side);
  CHECK(runs_in_row(front, c.dims.H - 1 - 3) == 3);
  CHECK(runs_in_row(side, c.dims.H - 1 - 3) == 2);
}

TEST_CASE("fixturegen: C cavity interior is darker than rim in top") {
  const GroundTruth c = make_fixture_C();
  const ImageRGBA top = render_gt_view(c, ViewRole::Top);
  double cavity = 0, rim = 0;
  int nc = 0, nr = 0;
  for (int z = 8; z < 14; ++z)
    for (int x = 8; x < 18; ++x) cavity += relative_luminance(top.at(x, z)), ++nc;
  for (int x = 8; x < 18; ++x) rim += relative_luminance(top.at(x, 4)), ++nr;  // front... z=4 is the rear wall top
  CHECK(cavity / nc < 0.88 * (rim / nr));
}

TEST_CASE("fixturegen: C handle is visible only from the top and front") {
  const GroundTruth c = make_fixture_C();
  const ImageRGBA top = render_gt_view(c, ViewRole::Top);
  CHECK(top.at(23, 11).a == 255);  // handle footprint x∈[22,25), z∈[9,13)
  CHECK(top.at(23, 6).a == 0);
}

TEST_CASE("fixturegen: ground truth inside and volume") {
  const GroundTruth c = make_fixture_C();
  CHECK(c.inside({13, 15, 11}) == false);  // cavity
  CHECK(c.inside({5, 15, 11}) == true);    // left wall
  const GroundTruth a = make_fixture_A();
  // Legs 4×(2×2×14) + seat 18×3×16 + back 16×16×2 (sheared slab keeps its volume).
  CHECK(a.volume(4) == doctest::Approx(4 * 56 + 864 + 512).epsilon(0.01));
}

TEST_CASE("fixturegen: outlines skip features thinner than three pixels") {
  const GroundTruth a = make_fixture_A();
  const ImageRGBA front = render_gt_view(a, ViewRole::Front);
  const int v = a.dims.H - 1 - 5;  // leg row
  for (int u = 0; u < front.width(); ++u)
    if (front.at(u, v).a >= 128) CHECK_FALSE(front.at(u, v) == a.outline);
}

TEST_CASE("fixturegen: text map parses palette and transparency") {
  const auto img = image_from_text_map("palette:\nO=#2a1d17ff\nb=#80542f\n---\n.Ob\nbO.\n");
  REQUIRE(img.has_value());
  CHECK(img->width() == 3);
  CHECK(img->height() == 2);
  CHECK(img->at(0, 0).a == 0);
  CHECK(img->at(1, 0) == Rgba8{0x2a, 0x1d, 0x17, 0xff});
  CHECK(img->at(2, 0) == Rgba8{0x80, 0x54, 0x2f, 0xff});
}

TEST_CASE("fixturegen: ragged text map is rejected") {
  const auto img = image_from_text_map("palette:\nO=#000000\n---\nOO\nO\n");
  REQUIRE_FALSE(img.has_value());
  CHECK(img.error().code == "fixture.ragged_rows");
}

TEST_CASE("fixturegen: B views have consistent extents") {
  const std::string dir = std::string(PIXEL3D_FIXTURES_DIR) + "/B_hand_chair/";
  const auto front = image_from_text_map(read_text(dir + "front.txt"));
  const auto side = image_from_text_map(read_text(dir + "side.txt"));
  const auto top = image_from_text_map(read_text(dir + "top.txt"));
  REQUIRE(front.has_value());
  REQUIRE(side.has_value());
  REQUIRE(top.has_value());
  CHECK(front->width() == 16);
  CHECK(front->height() == 28);
  CHECK(side->width() == 14);
  CHECK(side->height() == 28);
  CHECK(top->width() == 16);
  CHECK(top->height() == 14);
  // Slatted back: gap rows have two runs, slat rows one.
  CHECK(runs_in_row(*front, 2) == 2);
  CHECK(runs_in_row(*front, 4) == 1);
  CHECK(max_runs_in_rows(*front, 16, 28) == 2);
}
