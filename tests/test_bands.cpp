#include <doctest/doctest.h>

#include "pixel3d/segment/bands.hpp"
#include "test_helpers.hpp"

using namespace pixel3d;

namespace {

const Band* band_at(const BandSet& bs, int y) {
  for (const Band& b : bs.bands)
    if (y >= b.y0 && y < b.y1) return &b;
  return nullptr;
}

}  // namespace

TEST_CASE("bands: chair A has back+seat band and leg band") {
  const auto f = p3dtest::load_fixture("A_synthetic_chair");
  const int H = f.align.dims.H;
  const BandSet bs = compute_bands(f.y_masks(), H);
  const Band* legs = band_at(bs, 5);
  REQUIRE(legs);
  CHECK(legs->y0 == 0);
  CHECK(legs->y1 == 14);
  CHECK(bs.view(legs->id, ViewRole::Front)->strands.size() == 2);
  CHECK(bs.view(legs->id, ViewRole::Side)->strands.size() == 2);
  const Band* seat = band_at(bs, 15);
  REQUIRE(seat);
  CHECK(seat->y0 == 14);
  CHECK(seat == band_at(bs, 30));  // seat and back rows are one continuous band
  CHECK(bs.view(seat->id, ViewRole::Front)->strands.size() == 1);
  CHECK(bs.view(seat->id, ViewRole::Side)->strands.size() == 1);
  // A strand holds exactly one run per band row.
  CHECK(bs.view(legs->id, ViewRole::Front)->strands[0].runs.size() == 14);
}

TEST_CASE("bands: seat band atoms split at leg boundaries") {
  const auto f = p3dtest::load_fixture("A_synthetic_chair");
  const BandSet bs = compute_bands(f.y_masks(), f.align.dims.H);
  const Band* seat = band_at(bs, 15);
  REQUIRE(seat);
  const BandView* fv = bs.view(seat->id, ViewRole::Front);
  REQUIRE(fv->atoms.size() == 5);  // edge, leg L, middle, leg R, edge
  CHECK(fv->atoms[0].a == 0);
  CHECK(fv->atoms[1].a == 1);
  CHECK(fv->atoms[1].b == 3);
  CHECK(fv->atoms[4].b == 18);
  for (const Atom& a : fv->atoms) CHECK(a.split);
  // The leg band itself is not split: its strands are original atoms.
  const BandView* legs = bs.view(band_at(bs, 5)->id, ViewRole::Front);
  REQUIRE(legs->atoms.size() == 2);
  CHECK_FALSE(legs->atoms[0].split);
}

TEST_CASE("bands: slatted back alternates bands") {
  const auto f = p3dtest::load_fixture("B_hand_chair");
  const BandSet bs = compute_bands(f.y_masks(), f.align.dims.H);
  // B rows (world Y): gap rows 24-25 have two posts, slat rows 22-23 one run.
  const Band* gap = band_at(bs, 24);
  const Band* slat = band_at(bs, 22);
  REQUIRE(gap);
  REQUIRE(slat);
  CHECK(gap != slat);
  CHECK(bs.view(gap->id, ViewRole::Front)->strands.size() == 2);
  CHECK(bs.view(gap->id, ViewRole::Side)->strands.size() == 1);
  const BandView* sv = bs.view(slat->id, ViewRole::Front);
  CHECK(sv->strands.size() == 1);
  CHECK(sv->atoms.size() == 3);  // post L, slat middle, post R
}

TEST_CASE("bands: C legs band has 3 front and 2 side strands") {
  const auto f = p3dtest::load_fixture("C_asym_planter");
  const BandSet bs = compute_bands(f.y_masks(), f.align.dims.H);
  const Band* legs = band_at(bs, 3);
  REQUIRE(legs);
  CHECK(bs.view(legs->id, ViewRole::Front)->strands.size() == 3);
  CHECK(bs.view(legs->id, ViewRole::Side)->strands.size() == 2);
}

TEST_CASE("bands: diagonal one-pixel line stays one strand") {
  Mask2D front(6, 6), side(3, 6);
  for (int i = 0; i < 6; ++i) front.set(i, 5 - i, true);  // 8-connected diagonal
  for (int v = 0; v < 6; ++v) side.set(1, v, true);
  const BandSet bs = compute_bands({{ViewRole::Front, front}, {ViewRole::Side, side}}, 6);
  REQUIRE(bs.bands.size() == 1);
  CHECK(bs.view(0, ViewRole::Front)->strands.size() == 1);
}

TEST_CASE("bands: rows empty in one view break the band") {
  Mask2D front(3, 4), side(3, 4);
  for (int v = 0; v < 4; ++v) side.set(1, v, true);
  for (int v : {0, 1, 3}) front.set(1, v, true);  // world row Y=1 (v=2) missing in Front
  const BandSet bs = compute_bands({{ViewRole::Front, front}, {ViewRole::Side, side}}, 4);
  CHECK(bs.bands.size() == 3);
  const Band* b = band_at(bs, 1);
  REQUIRE(b);
  CHECK(bs.view(b->id, ViewRole::Front)->strands.empty());
}
