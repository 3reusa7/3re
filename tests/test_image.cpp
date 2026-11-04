#include <doctest/doctest.h>

#include <random>

#include "pixel3d/image/image.hpp"
#include "pixel3d/image/mask.hpp"
#include "pixel3d/image/pixel_scale.hpp"

using namespace pixel3d;

namespace {

ImageRGBA random_image(int w, int h, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> d(0, 255);
  const uint8_t alphas[] = {0, 255, 37};
  ImageRGBA img(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      img.at(x, y) = Rgba8{uint8_t(d(rng)), uint8_t(d(rng)), uint8_t(d(rng)), alphas[d(rng) % 3]};
  return img;
}

ImageRGBA upscale(const ImageRGBA& src, int k, int pad_left) {
  ImageRGBA out(src.width() * k + pad_left, src.height() * k);
  for (int y = 0; y < out.height(); ++y)
    for (int x = pad_left; x < out.width(); ++x) out.at(x, y) = src.at((x - pad_left) / k, y / k);
  return out;
}

}  // namespace

TEST_CASE("image: png round trip preserves exact RGBA") {
  const ImageRGBA img = random_image(7, 5, 1);
  const auto bytes = encode_png(img);
  const auto back = decode_png(bytes);
  REQUIRE(back.has_value());
  REQUIRE(back->width() == 7);
  REQUIRE(back->height() == 5);
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 7; ++x) CHECK(back->at(x, y) == img.at(x, y));
}

TEST_CASE("image: load_png missing file gives code io.not_found") {
  const auto r = load_png("definitely/not/here.png");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == "io.not_found");
  CHECK(r.error().severity == Severity::Error);
}

TEST_CASE("image: decode garbage gives image.decode_failed") {
  const std::vector<uint8_t> junk = {1, 2, 3, 4, 5};
  const auto r = decode_png(junk);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == "image.decode_failed");
}

TEST_CASE("image: opaque_mask threshold 128") {
  ImageRGBA img(2, 1);
  img.at(0, 0) = {10, 10, 10, 127};
  img.at(1, 0) = {10, 10, 10, 128};
  const Mask2D m = opaque_mask(img);
  CHECK_FALSE(m.get(0, 0));
  CHECK(m.get(1, 0));
}

TEST_CASE("image: mask_iou and xor") {
  Mask2D a(4, 1), b(4, 1);
  a.set(0, 0, true);
  a.set(1, 0, true);
  b.set(1, 0, true);
  b.set(2, 0, true);
  CHECK(mask_iou(a, b) == doctest::Approx(1.0 / 3.0));
  CHECK(mask_xor_count(a, b) == 2);
  Mask2D c(4, 1);
  c.set(0, 0, true);
  c.set(1, 0, true);
  CHECK(mask_iou(a, c) == 1.0);
  CHECK(mask_iou(Mask2D(3, 3), Mask2D(3, 3)) == 1.0);
}

TEST_CASE("image: mask bbox and count") {
  Mask2D m(5, 4);
  m.set(1, 2, true);
  m.set(3, 1, true);
  CHECK(m.count() == 2);
  const auto box = m.bbox();
  CHECK(box.x0 == 1);
  CHECK(box.y0 == 1);
  CHECK(box.x1 == 4);  // exclusive
  CHECK(box.y1 == 3);
  CHECK(Mask2D(2, 2).bbox().empty());
}

TEST_CASE("image: flip and crop") {
  ImageRGBA img(3, 2);
  img.at(0, 0) = {1, 0, 0, 255};
  const ImageRGBA f = flip_x(img);
  CHECK(f.at(2, 0) == Rgba8{1, 0, 0, 255});
  const ImageRGBA c = crop(f, Mask2D::Box{2, 0, 3, 1});
  CHECK(c.width() == 1);
  CHECK(c.height() == 1);
  CHECK(c.at(0, 0) == Rgba8{1, 0, 0, 255});
}

TEST_CASE("image: detect_pixel_scale finds 4 with offset 2") {
  ImageRGBA src = random_image(8, 6, 7);
  for (int y = 0; y < 6; ++y) src.at(0, y).a = 255;  // keep first column non-transparent
  const ImageRGBA big = upscale(src, 4, 2);
  const ScaleInfo s = detect_pixel_scale(big);
  CHECK(s.k == 4);
  CHECK(s.ox == 2);
  CHECK(s.oy == 0);
  const ImageRGBA small = downscale_nearest(big, s);
  REQUIRE(small.width() == 9);   // 1 partial column (the 2-px pad) + 8
  REQUIRE(small.height() == 6);
  for (int y = 0; y < 6; ++y)
    for (int x = 0; x < 8; ++x) CHECK(small.at(x + 1, y) == src.at(x, y));
}

TEST_CASE("image: detect_pixel_scale is 1 for native art") {
  const ImageRGBA img = random_image(9, 9, 3);
  CHECK(detect_pixel_scale(img).k == 1);
}
