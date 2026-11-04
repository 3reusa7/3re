#include <doctest/doctest.h>

#include <set>

#include "pixel3d/image/palette.hpp"

using namespace pixel3d;

namespace {

constexpr Rgba8 kOutline{30, 24, 20, 255};
constexpr Rgba8 kWood[4] = {{92, 58, 34, 255}, {128, 84, 48, 255}, {164, 112, 66, 255}, {198, 146, 92, 255}};
constexpr Rgba8 kRed[2] = {{150, 30, 38, 255}, {206, 58, 62, 255}};

void fill(ImageRGBA& img, int x0, int y0, int x1, int y1, Rgba8 c) {
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x) img.at(x, y) = c;
}

// Outlined rectangle: 1px outline ring around a filled interior.
void outlined_block(ImageRGBA& img, int x0, int y0, int x1, int y1, Rgba8 inner) {
  fill(img, x0, y0, x1, y1, kOutline);
  fill(img, x0 + 1, y0 + 1, x1 - 1, y1 - 1, inner);
}

MaterialFamilies families_of(const ImageRGBA& img) {
  const ImageRGBA* views[] = {&img};
  return analyze_palette(views);
}

}  // namespace

TEST_CASE("palette: shades of one hue form one family") {
  ImageRGBA img(8, 2);
  for (int i = 0; i < 4; ++i) fill(img, i * 2, 0, i * 2 + 2, 2, kWood[i]);
  const auto f = families_of(img);
  CHECK(f.entries.size() == 4);
  CHECK(f.family_count == 1);
  for (const auto& c : kWood) CHECK(f.family_of(c) == f.family_of(kWood[0]));
}

TEST_CASE("palette: different hues split") {
  ImageRGBA img(8, 4);
  fill(img, 0, 0, 4, 4, kWood[1]);
  fill(img, 4, 0, 6, 4, kRed[0]);
  fill(img, 6, 0, 8, 4, kRed[1]);
  const auto f = families_of(img);
  CHECK(f.family_count == 2);
  CHECK(f.family_of(kRed[0]) == f.family_of(kRed[1]));
  CHECK(f.family_of(kRed[0]) != f.family_of(kWood[1]));
}

TEST_CASE("palette: near-black low-chroma becomes outline family") {
  ImageRGBA img(20, 8);
  outlined_block(img, 0, 0, 10, 8, kWood[2]);
  outlined_block(img, 9, 0, 20, 8, kRed[1]);  // shares one outline column
  const auto f = families_of(img);
  REQUIRE(f.outline_family >= 0);
  CHECK(f.family_of(kOutline) == f.outline_family);
  CHECK(f.family_of(kWood[2]) != f.outline_family);
  CHECK(f.family_of(kRed[1]) != f.outline_family);
}

TEST_CASE("palette: greys without outline role are their own family") {
  ImageRGBA img(10, 10);
  fill(img, 0, 0, 10, 10, Rgba8{35, 35, 35, 255});  // large dark grey area
  fill(img, 0, 0, 3, 3, kWood[1]);
  const auto f = families_of(img);
  CHECK(f.outline_family == -1);
  CHECK(f.family_of(Rgba8{35, 35, 35, 255}) >= 0);
  CHECK(f.family_of(Rgba8{35, 35, 35, 255}) != f.family_of(kWood[1]));
}

TEST_CASE("palette: transparent and unknown colours have no family") {
  ImageRGBA img(2, 1);
  img.at(0, 0) = kWood[0];
  const auto f = families_of(img);
  CHECK(f.family_of(Rgba8{0, 0, 0, 0}) == -1);
  CHECK(f.family_of(Rgba8{1, 2, 3, 255}) == -1);
}

TEST_CASE("palette: families are shared across views") {
  ImageRGBA a(2, 2), b(2, 2);
  fill(a, 0, 0, 2, 2, kWood[0]);
  fill(b, 0, 0, 2, 2, kWood[3]);
  const ImageRGBA* views[] = {&a, &b};
  const auto f = analyze_palette(views);
  CHECK(f.family_of(kWood[0]) == f.family_of(kWood[3]));
}

TEST_CASE("palette: relative luminance endpoints") {
  CHECK(relative_luminance(Rgba8{0, 0, 0, 255}) == doctest::Approx(0.0));
  CHECK(relative_luminance(Rgba8{255, 255, 255, 255}) == doctest::Approx(1.0));
  CHECK(relative_luminance(kWood[3]) > relative_luminance(kWood[0]));
}
