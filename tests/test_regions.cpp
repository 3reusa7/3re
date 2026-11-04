#include <doctest/doctest.h>

#include "pixel3d/image/palette.hpp"
#include "pixel3d/segment/regions.hpp"
#include "test_helpers.hpp"

using namespace pixel3d;

namespace {

MaterialFamilies families(const p3dtest::Fixture& f) {
  std::vector<const ImageRGBA*> imgs;
  for (const auto& v : f.views) imgs.push_back(v.aligned.get());
  return analyze_palette(imgs);
}

}  // namespace

TEST_CASE("regions: shading does not split a part") {
  ImageRGBA img(6, 2);
  const Rgba8 shades[3] = {{92, 58, 34, 255}, {128, 84, 48, 255}, {164, 112, 66, 255}};
  for (int x = 0; x < 6; ++x)
    for (int y = 0; y < 2; ++y) img.at(x, y) = shades[x / 2];
  const ImageRGBA* v[] = {&img};
  const RegionMap rm = compute_regions(img, analyze_palette(v));
  CHECK(rm.regions.size() == 1);
  CHECK(rm.regions[0].pixels.size() == 12);
  CHECK(rm.regions[0].touches_silhouette);
  CHECK_FALSE(rm.regions[0].enclosed);
}

TEST_CASE("regions: outline separates leg end from seat underside") {
  const auto f = p3dtest::load_fixture("C_asym_planter_bottom",
                                       {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Bottom});
  const ImageRGBA& bottom = *f.view(ViewRole::Bottom).aligned;
  const RegionMap rm = compute_regions(bottom, families(f));
  // Front-left leg end: x∈[5,7), z∈[15,17) → aligned bottom (1..2, 1..2); underside sample at x=10, z=10.
  const int leg = rm.label[1 * rm.w + 1];
  const int under = rm.label[7 * rm.w + 6];
  REQUIRE(leg >= 0);
  REQUIRE(under >= 0);
  CHECK(leg != under);
  CHECK(rm.regions[leg].pixels.size() <= 4);
  CHECK(rm.regions[under].pixels.size() > 50);
}

TEST_CASE("regions: C top cavity region is enclosed and darker than rim") {
  const auto f = p3dtest::load_fixture("C_asym_planter");
  const ImageRGBA& top = *f.view(ViewRole::Top).aligned;
  const RegionMap rm = compute_regions(top, families(f));
  const int cavity = rm.label[7 * rm.w + 9];  // x=13, z=11 inside the cavity
  const int rim = rm.label[0 * rm.w + 6];     // rear wall top
  REQUIRE(cavity >= 0);
  REQUIRE(rim >= 0);
  CHECK(cavity != rim);
  CHECK(rm.regions[cavity].enclosed);
  CHECK_FALSE(rm.regions[cavity].touches_silhouette);
  CHECK_FALSE(rm.regions[rim].enclosed);
  CHECK(rm.regions[cavity].mean_luminance < 0.88 * rm.regions[rim].mean_luminance);
  const auto& nb = rm.regions[cavity].neighbours;
  CHECK(std::find(nb.begin(), nb.end(), rim) != nb.end());
}

TEST_CASE("regions: B cushion is its own region") {
  const auto f = p3dtest::load_fixture("B_hand_chair");
  const ImageRGBA& top = *f.view(ViewRole::Top).aligned;
  const RegionMap rm = compute_regions(top, families(f));
  const int cushion = rm.label[6 * rm.w + 5];
  const int back = rm.label[1 * rm.w + 5];
  REQUIRE(cushion >= 0);
  REQUIRE(back >= 0);
  CHECK(cushion != back);
  CHECK(rm.label[0] == -1);  // outline pixel
  CHECK_FALSE(rm.regions[cushion].enclosed);  // reaches the silhouette through the outline ring
}
