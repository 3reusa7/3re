// Input normalisation: real-world images (opaque or white backgrounds, JPEG
// noise, anti-aliased oversize art, views at different scales, other file
// formats) become native pixel art that aligns like a clean drawing, every
// change is reported, clean pixel art is untouched, and inputs that cannot be
// handled are refused with a specific error.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include <stb_image_write.h>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/image/normalize.hpp"
#include "pixel3d/views/align.hpp"
#include "test_helpers.hpp"

using namespace pixel3d;
using p3dtest::Box;
namespace fs = std::filesystem;

namespace {

// ---- a small asymmetric "character" ------------------------------------------

const Rgba8 kBoot{70, 40, 20, 255}, kPants{50, 50, 60, 255}, kCoat{120, 70, 40, 255}, kSkin{240, 190, 150, 255},
    kHood{40, 40, 45, 255}, kScarf{30, 50, 170, 255};

std::vector<Box> character(int depth_scale = 1) {
  const int s = depth_scale;
  return {Box{2, 0, 2 * s, 5, 8, 6 * s, kBoot},   Box{7, 0, 2 * s, 10, 8, 6 * s, kBoot},
          Box{2, 8, 2 * s, 10, 10, 6 * s, kPants}, Box{1, 10, 1 * s, 11, 20, 7 * s, kCoat},
          Box{0, 11, 2 * s, 1, 18, 6 * s, kCoat},  Box{11, 11, 2 * s, 12, 18, 6 * s, kCoat},
          Box{3, 20, 2 * s, 9, 26, 7 * s, kSkin},  Box{2, 26, 1 * s, 10, 30, 8 * s, kHood},
          Box{3, 18, 7 * s, 9, 20, 8 * s, kScarf}, Box{10, 12, 6 * s, 12, 15, 9 * s, kCoat}};
}
const Dims kCharDims{12, 30, 8};  // the opaque extent: every part starts at z >= 1

std::map<ViewRole, ImageRGBA> character_views(int depth_scale = 1) {
  return p3dtest::render_views(character(depth_scale), Dims{12, 30, 9 * depth_scale});
}

// ---- degraders ---------------------------------------------------------------

ImageRGBA upscale(const ImageRGBA& img, int k, int margin) {
  return p3dtest::upscale_padded(img, k, margin, margin, margin, margin);
}

// Box-filter resample by a real factor f (anti-aliased edges, blended colours,
// partial alpha), with a transparent margin.
ImageRGBA antialiased(const ImageRGBA& src, double f, int margin) {
  const int w = static_cast<int>(std::lround(src.width() * f)), h = static_cast<int>(std::lround(src.height() * f));
  ImageRGBA out(w + 2 * margin, h + 2 * margin);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      double r = 0, g = 0, b = 0, a = 0, area = 0;
      const double x0 = x / f, x1 = (x + 1) / f, y0 = y / f, y1 = (y + 1) / f;
      for (int sy = static_cast<int>(y0); sy < std::min(src.height(), static_cast<int>(std::ceil(y1))); ++sy)
        for (int sx = static_cast<int>(x0); sx < std::min(src.width(), static_cast<int>(std::ceil(x1))); ++sx) {
          const double wx = std::min<double>(x1, sx + 1) - std::max<double>(x0, sx);
          const double wy = std::min<double>(y1, sy + 1) - std::max<double>(y0, sy);
          const double wgt = std::max(0.0, wx) * std::max(0.0, wy);
          const Rgba8 p = src.at(sx, sy);
          const double pa = p.a / 255.0 * wgt;
          r += p.r * pa, g += p.g * pa, b += p.b * pa, a += pa, area += wgt;
        }
      if (a > 0)
        out.at(x + margin, y + margin) =
            Rgba8{static_cast<uint8_t>(std::lround(r / a)), static_cast<uint8_t>(std::lround(g / a)),
                  static_cast<uint8_t>(std::lround(b / a)), static_cast<uint8_t>(std::lround(255.0 * a / area))};
    }
  return out;
}

// Alpha-composites onto an opaque background colour.
ImageRGBA on_background(const ImageRGBA& img, Rgba8 bg) {
  ImageRGBA out(img.width(), img.height(), bg);
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x) {
      const Rgba8 p = img.at(x, y);
      const double a = p.a / 255.0;
      auto mix = [&](uint8_t c, uint8_t b) { return static_cast<uint8_t>(std::lround(c * a + b * (1 - a))); };
      out.at(x, y) = Rgba8{mix(p.r, bg.r), mix(p.g, bg.g), mix(p.b, bg.b), 255};
    }
  return out;
}

// A 1-px checker dither of ±delta on opaque pixels: no block grid survives.
ImageRGBA dithered(const ImageRGBA& img, int delta) {
  ImageRGBA out = img;
  for (int y = 0; y < out.height(); ++y)
    for (int x = 0; x < out.width(); ++x) {
      Rgba8& p = out.at(x, y);
      if (p.a == 0) continue;
      const int d = (x + y) % 2 ? delta : 0;
      auto add = [&](uint8_t c) { return static_cast<uint8_t>(std::clamp(c + d, 0, 255)); };
      p = Rgba8{add(p.r), add(p.g), add(p.b), p.a};
    }
  return out;
}

// Deterministic ±amp noise on every channel of opaque pixels (JPEG-like).
ImageRGBA noisy(const ImageRGBA& img, int amp, uint32_t seed) {
  ImageRGBA out = img;
  uint32_t s = seed;
  auto next = [&] {
    s = s * 1664525u + 1013904223u;
    return static_cast<int>((s >> 16) % static_cast<uint32_t>(2 * amp + 1)) - amp;
  };
  for (int y = 0; y < out.height(); ++y)
    for (int x = 0; x < out.width(); ++x) {
      Rgba8& p = out.at(x, y);
      if (p.a == 0) continue;
      auto add = [&](uint8_t c) { return static_cast<uint8_t>(std::clamp(c + next(), 0, 255)); };
      p = Rgba8{add(p.r), add(p.g), add(p.b), p.a};
    }
  return out;
}

std::vector<SourceView> make_views(const std::map<ViewRole, ImageRGBA>& imgs) {
  std::vector<SourceView> v;
  for (const auto& [role, img] : imgs) {
    SourceView s;
    s.role = role;
    s.original = std::make_shared<const ImageRGBA>(img);
    v.push_back(std::move(s));
  }
  return v;
}

const SourceView& find(const std::vector<SourceView>& v, ViewRole r) {
  for (const auto& s : v)
    if (s.role == r) return s;
  FAIL("view missing");
  return v.front();
}

struct NormRun {
  std::vector<SourceView> views;
  Expected<NormalizeResult> norm = NormalizeResult{};
  Expected<AlignResult> align = AlignResult{};
};

// The align stage as the pipeline runs it: normalise, then align.
NormRun normalise_and_align(const std::map<ViewRole, ImageRGBA>& imgs, NormalizeSettings settings = {}) {
  NormRun r;
  r.views = make_views(imgs);
  NormalizeOptions no;
  no.settings = settings;
  r.norm = normalize_views(r.views, no);
  if (r.norm) r.align = auto_align(r.views, AlignOptions{});
  return r;
}

// A copy of a message's details: missing keys read as null instead of asserting.
nlohmann::json W(const Message& m) { return m.where; }

bool has_code(const std::vector<Message>& ms, const std::string& code) {
  return std::any_of(ms.begin(), ms.end(), [&](const Message& m) { return m.code == code; });
}

int channel_distance(Rgba8 a, Rgba8 b) {
  return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

// Same silhouette as the clean views; every colour within `tol` of the clean one.
void check_matches_clean(const NormRun& r, int tol) {
  if (!r.norm) FAIL_CHECK(r.norm.error().code << ": " << r.norm.error().text);
  REQUIRE(r.norm.has_value());
  if (!r.align) FAIL_CHECK(r.align.error().code << ": " << r.align.error().text);
  REQUIRE(r.align.has_value());
  const NormRun clean = normalise_and_align(character_views());
  REQUIRE(clean.align.has_value());
  CHECK(r.align->dims == clean.align->dims);
  for (ViewRole role : {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
    CAPTURE(to_string(role));
    const ImageRGBA& got = *find(r.views, role).aligned;
    const ImageRGBA& want = *find(clean.views, role).aligned;
    REQUIRE(got.width() == want.width());
    REQUIRE(got.height() == want.height());
    CHECK(opaque_mask(got) == opaque_mask(want));
    int worst = 0;
    for (int y = 0; y < got.height(); ++y)
      for (int x = 0; x < got.width(); ++x)
        if (want.at(x, y).a) worst = std::max(worst, channel_distance(got.at(x, y), want.at(x, y)));
    CHECK(worst <= tol);
  }
}

std::vector<uint8_t> encode_with(int (*writer)(stbi_write_func*, void*, int, int, int, const void*), const ImageRGBA& img) {
  std::vector<uint8_t> out;
  auto sink = [](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
  };
  writer(sink, &out, img.width(), img.height(), 4, img.pixels().data());
  return out;
}

std::vector<uint8_t> encode_jpeg(const ImageRGBA& img, int quality) {
  std::vector<uint8_t> out;
  auto sink = [](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
  };
  stbi_write_jpg_to_func(sink, &out, img.width(), img.height(), 4, img.pixels().data(), quality);
  return out;
}

// Animated GIF, 3×2: frame 1 = red, green, blue / white, black, transparent; frame 2 = yellow.
const std::vector<uint8_t> kGif = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x03, 0x00, 0x02, 0x00, 0x82, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00,
    0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53, 0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00,
    0x00, 0x00, 0x21, 0xf9, 0x04, 0x09, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02,
    0x00, 0x00, 0x08, 0x09, 0x00, 0x03, 0x0c, 0x20, 0x20, 0xa0, 0x00, 0x80, 0x80, 0x00, 0x21, 0xf9, 0x04, 0x09,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x81, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x06, 0x00, 0x01, 0x08, 0x1c, 0x18, 0x10, 0x00, 0x3b};

const Rgba8 kWhite{255, 255, 255, 255};

}  // namespace

// ---- clean pixel art is untouched ---------------------------------------------

TEST_CASE("normalize: clean pixel art is untouched (fixtures, k× upscales)") {
  for (const char* name : {"A_synthetic_chair", "B_hand_chair", "C_asym_planter"}) {
    CAPTURE(name);
    for (int k : {1, 4}) {
      std::map<ViewRole, ImageRGBA> imgs;
      for (ViewRole r : {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
        auto img = load_png(std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + std::string(to_string(r)) + ".png");
        REQUIRE(img.has_value());
        imgs.emplace(r, upscale(*img, k, k == 1 ? 0 : 3));
      }
      NormRun r = normalise_and_align(imgs);
      REQUIRE(r.norm.has_value());
      CHECK_FALSE(r.norm->applied);
      CHECK(r.norm->messages.empty());
      for (const SourceView& v : r.views) CHECK(v.working == nullptr);
      // Exactly the alignment without normalisation.
      auto plain = make_views(imgs);
      REQUIRE(auto_align(plain, AlignOptions{}).has_value());
      for (ViewRole role : {ViewRole::Front, ViewRole::Side, ViewRole::Top})
        CHECK(*find(r.views, role).aligned == *find(plain, role).aligned);
    }
  }
}

TEST_CASE("normalize: a fully opaque solid sprite and an outlined tile are not 'backgrounds'") {
  ImageRGBA solid(8, 8, Rgba8{200, 50, 50, 255});
  CHECK_FALSE(remove_background(solid, 24, 128).removed);
  ImageRGBA tile(20, 20, Rgba8{20, 20, 20, 255});  // a 2-px outline around a lighter fill
  for (int y = 2; y < 18; ++y)
    for (int x = 2; x < 18; ++x) tile.at(x, y) = Rgba8{180, 140, 90, 255};
  CHECK_FALSE(remove_background(tile, 24, 128).removed);
}

// ---- backgrounds -----------------------------------------------------------------

TEST_CASE("normalize: an opaque white background is removed by flood fill") {
  std::map<ViewRole, ImageRGBA> imgs;
  for (const auto& [role, img] : character_views()) imgs.emplace(role, on_background(upscale(img, 6, 20), kWhite));
  const NormRun r = normalise_and_align(imgs);
  check_matches_clean(r, 0);
  REQUIRE(r.norm.has_value());
  CHECK(r.norm->applied);
  for (const SourceView& v : r.views) {
    CHECK(v.norm.applied);
    CHECK(v.norm.background_removed);
    CHECK(v.norm.background == kWhite);
    CHECK(v.norm.background_share > 0.3);
  }
  // Reported per view, with the numbers.
  int reported = 0;
  for (const Message& m : r.norm->messages)
    if (m.code == "input.background_removed") {
      ++reported;
      CHECK(m.where.contains("view"));
      CHECK(m.where.contains("removed_share"));
      CHECK(m.where.contains("colour"));
    }
  CHECK(reported == 3);
}

TEST_CASE("normalize: background-coloured pixels not connected to the border stay opaque") {
  ImageRGBA img(24, 24, kWhite);
  for (int y = 4; y < 20; ++y)
    for (int x = 4; x < 20; ++x) img.at(x, y) = Rgba8{30, 30, 30, 255};
  for (int y = 9; y < 15; ++y)
    for (int x = 9; x < 15; ++x) img.at(x, y) = kWhite;  // a white window inside a dark frame
  const BackgroundRemoval b = remove_background(img, 24, 128);
  REQUIRE(b.removed);
  CHECK(b.image.at(0, 0).a == 0);
  CHECK(b.image.at(12, 12) == kWhite);  // interior white is part of the subject
  CHECK(b.image.at(5, 5).a == 255);
  CHECK(b.removed_px == 24 * 24 - 16 * 16);
  CHECK_FALSE(b.touches_edge);
}

TEST_CASE("normalize: a fake-transparency checkerboard is a background") {
  ImageRGBA img(40, 40);
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 40; ++x) img.at(x, y) = ((x / 4 + y / 4) % 2) ? Rgba8{204, 204, 204, 255} : kWhite;
  for (int y = 10; y < 30; ++y)
    for (int x = 12; x < 28; ++x) img.at(x, y) = kCoat;
  const BackgroundRemoval b = remove_background(img, 24, 128);
  REQUIRE(b.removed);
  CHECK(b.checker);
  CHECK(opaque_mask(b.image).count() == 20 * 16);
}

TEST_CASE("normalize: a subject touching the image edge warns about a possible fill leak") {
  std::map<ViewRole, ImageRGBA> imgs;
  for (const auto& [role, img] : character_views()) imgs.emplace(role, on_background(upscale(img, 6, 0), kWhite));
  const NormRun r = normalise_and_align(imgs);
  REQUIRE(r.norm.has_value());
  bool warned = false;
  for (const Message& m : r.norm->messages)
    if (m.code == "input.background_leak_possible") {
      warned = true;
      CHECK(m.severity == Severity::Warning);
      CHECK(m.where.contains("removed_share"));
      CHECK(m.where.contains("touches_edge"));
    }
  CHECK(warned);
}

// ---- noise, anti-aliasing, scale ----------------------------------------------------

TEST_CASE("normalize: JPEG-like noise on edges and background is cleaned up") {
  std::map<ViewRole, ImageRGBA> imgs;
  uint32_t seed = 7;
  for (const auto& [role, img] : character_views())
    imgs.emplace(role, noisy(on_background(antialiased(img, 8, 16), kWhite), 6, seed++));
  const NormRun r = normalise_and_align(imgs);
  check_matches_clean(r, 12);
  REQUIRE(r.norm.has_value());
  CHECK(has_code(r.norm->messages, "input.colours_merged"));
  for (const SourceView& v : r.views) {
    CHECK(v.norm.colours_before > 200);
    CHECK(v.norm.colours_after <= 24);
  }
}

TEST_CASE("normalize: anti-aliased art about 1200 px tall becomes native size, no invented colours") {
  std::map<ViewRole, ImageRGBA> imgs;
  for (const auto& [role, img] : character_views()) imgs.emplace(role, antialiased(img, 39.3, 25));
  CHECK(imgs.at(ViewRole::Front).height() == 1179 + 50);
  const NormRun r = normalise_and_align(imgs);
  check_matches_clean(r, 0);
  REQUIRE(r.align.has_value());
  const std::set<uint32_t> palette = [] {
    std::set<uint32_t> s;
    for (const Rgba8& c : {kBoot, kPants, kCoat, kSkin, kHood, kScarf}) s.insert(c.r << 16 | c.g << 8 | c.b);
    return s;
  }();
  for (const SourceView& v : r.views) {
    CHECK(v.norm.block == doctest::Approx(39.3).epsilon(0.03));
    CHECK_FALSE(v.norm.block_exact);
    for (const Rgba8& p : v.aligned->pixels())
      if (p.a) CHECK(palette.contains(p.r << 16 | p.g << 8 | p.b));
  }
  CHECK(has_code(r.norm->messages, "input.downscaled"));
}

TEST_CASE("normalize: views upscaled 3x and 4x are brought to one scale, with a warning") {
  const auto clean = character_views();
  std::map<ViewRole, ImageRGBA> imgs{{ViewRole::Front, upscale(clean.at(ViewRole::Front), 3, 5)},
                                     {ViewRole::Side, upscale(clean.at(ViewRole::Side), 4, 2)},
                                     {ViewRole::Top, upscale(clean.at(ViewRole::Top), 4, 9)}};
  const NormRun r = normalise_and_align(imgs);
  check_matches_clean(r, 0);
  REQUIRE(r.norm.has_value());
  bool warned = false;
  for (const Message& m : r.norm->messages)
    if (m.code == "input.scale_per_view") {
      warned = true;
      CHECK(m.severity == Severity::Warning);
      CHECK(W(m)["factors"]["front"] == 3);
      CHECK(W(m)["factors"]["side"] == 4);
    }
  CHECK(warned);
  CHECK(find(r.views, ViewRole::Front).norm.block == 3);
  CHECK(find(r.views, ViewRole::Side).norm.block_exact);
}

TEST_CASE("normalize: oversize normalised art is fitted to the target; clean oversize art gets a diagnostic too_large") {
  // Normalised art beyond the target: one common factor, the longest side becomes target_size.
  const auto clean = character_views();
  std::map<ViewRole, ImageRGBA> big;
  for (const auto& [role, img] : clean) big.emplace(role, on_background(upscale(img, 25, 30), kWhite));  // 750 px tall
  NormalizeSettings small;
  small.target_size = 20;
  const NormRun fitted = normalise_and_align(big, small);
  REQUIRE(fitted.norm.has_value());
  REQUIRE(fitted.align.has_value());
  CHECK(fitted.align->dims.H == 20);
  CHECK(std::max({fitted.align->dims.W, fitted.align->dims.D}) < 20);
  CHECK(has_code(fitted.norm->messages, "input.fitted"));

  // Clean pixel art is never touched, even too large: 601 px tall, no block grid.
  const std::vector<Box> tower{Box{0, 0, 0, 21, 601, 11, kCoat}, Box{0, 300, 0, 21, 307, 11, kScarf}};
  const auto imgs = p3dtest::render_views(tower, Dims{21, 601, 11});
  NormalizeSettings s;
  const NormRun on = normalise_and_align(imgs, s);
  REQUIRE(on.norm.has_value());
  CHECK_FALSE(on.norm->applied);
  REQUIRE_FALSE(on.align.has_value());
  CHECK(on.align.error().code == "view.too_large");

  s.enabled = false;
  const NormRun off = normalise_and_align(imgs, s);
  REQUIRE(off.norm.has_value());
  REQUIRE_FALSE(off.align.has_value());
  const Message& e = off.align.error();
  CHECK(e.code == "view.too_large");
  REQUIRE(e.where.contains("views"));
  const nlohmann::json front = W(e)["views"]["front"];
  for (const char* k : {"original_size", "opaque_bbox", "transparent_share", "detected_scale", "colours", "likely_cause"})
    CHECK(front.contains(k));
  CHECK(front["likely_cause"] == "drawing_too_large");
}

TEST_CASE("normalize: too_large names an opaque background as the likely cause") {
  std::map<ViewRole, ImageRGBA> imgs;
  for (const auto& [role, img] : character_views()) imgs.emplace(role, noisy(on_background(antialiased(img, 23, 4), kWhite), 4, 3));
  NormalizeSettings off;
  off.enabled = false;
  const NormRun r = normalise_and_align(imgs, off);
  REQUIRE(r.norm.has_value());
  REQUIRE_FALSE(r.align.has_value());
  CHECK(r.align.error().code == "view.too_large");
  CHECK(W(r.align.error())["views"]["front"]["likely_cause"] == "opaque_background");
}

// ---- formats -------------------------------------------------------------------------

TEST_CASE("normalize: a JPEG view decodes and normalises like the PNG") {
  std::map<ViewRole, ImageRGBA> imgs;
  for (const auto& [role, img] : character_views()) {
    const auto jpg = encode_jpeg(on_background(upscale(img, 10, 30), kWhite), 92);
    CHECK(sniff_image_format(jpg) == ImageFormat::Jpeg);
    auto decoded = decode_image(jpg, "view.jpg");
    REQUIRE(decoded.has_value());
    imgs.emplace(role, std::move(*decoded));
  }
  const NormRun r = normalise_and_align(imgs);
  check_matches_clean(r, 24);
}

TEST_CASE("image formats: BMP, TGA by name, GIF first frame; PNG stays strict for export checks") {
  ImageRGBA img(3, 2, Rgba8{10, 20, 30, 255});
  img.at(1, 0) = Rgba8{200, 100, 50, 255};
  const auto bmp = encode_with(stbi_write_bmp_to_func, img);
  CHECK(sniff_image_format(bmp) == ImageFormat::Bmp);
  auto b = decode_image(bmp, "x.bmp");
  REQUIRE(b.has_value());
  CHECK(b->at(1, 0) == img.at(1, 0));

  const auto tga = encode_with(stbi_write_tga_to_func, img);
  CHECK(sniff_image_format(tga, "art.TGA") == ImageFormat::Tga);
  CHECK(sniff_image_format(tga, "art.png") == ImageFormat::Unknown);  // no signature: only by name
  auto t = decode_image(tga, "art.tga");
  REQUIRE(t.has_value());
  CHECK(*t == img);
  const auto unnamed = decode_image(tga);
  REQUIRE_FALSE(unnamed.has_value());
  CHECK(unnamed.error().code == "image.unsupported_format");
  CHECK(unnamed.error().text.find("PNG, JPEG, BMP, GIF") != std::string::npos);

  CHECK(sniff_image_format(kGif) == ImageFormat::Gif);
  auto g = decode_image(kGif, "anim.gif");
  REQUIRE(g.has_value());
  CHECK(g->width() == 3);
  CHECK(g->at(0, 0) == Rgba8{255, 0, 0, 255});  // first frame, not the yellow second one
  CHECK(g->at(2, 1).a == 0);

  // decode_png verifies exported textures: it must not accept other formats.
  const auto not_png = decode_png(bmp);
  REQUIRE_FALSE(not_png.has_value());
  CHECK(not_png.error().text.find("PNG") != std::string::npos);
  // Garbage named .jpg is a JPEG that cannot be read; unnamed garbage is an unsupported format.
  const std::vector<uint8_t> junk{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  const auto j = decode_image(junk, "x.jpg");
  REQUIRE_FALSE(j.has_value());
  CHECK(j.error().code == "image.decode_failed");
  CHECK(j.error().text.find("JPEG") != std::string::npos);
  CHECK(decode_image(junk).error().code == "image.unsupported_format");
  // A JPEG whose header is cut off is a decode failure that says JPEG.
  auto jpg = encode_jpeg(img, 90);
  jpg.resize(20);
  const auto broken = decode_image(jpg, "x.jpg");
  REQUIRE_FALSE(broken.has_value());
  CHECK(broken.error().code == "image.decode_failed");
  CHECK(broken.error().text.find("JPEG") != std::string::npos);
}

// ---- refusals --------------------------------------------------------------------

TEST_CASE("normalize: a turnaround sheet in one image is refused with a specific error") {
  const auto clean = character_views();
  const ImageRGBA front = upscale(clean.at(ViewRole::Front), 6, 0);
  ImageRGBA sheet(front.width() * 3 + 80, front.height() + 40);
  for (int i = 0; i < 3; ++i)
    for (int y = 0; y < front.height(); ++y)
      for (int x = 0; x < front.width(); ++x) sheet.at(20 + i * (front.width() + 20) + x, 20 + y) = front.at(x, y);
  std::map<ViewRole, ImageRGBA> imgs{{ViewRole::Front, on_background(sheet, kWhite)},
                                     {ViewRole::Side, on_background(upscale(clean.at(ViewRole::Side), 6, 20), kWhite)},
                                     {ViewRole::Top, on_background(upscale(clean.at(ViewRole::Top), 6, 20), kWhite)}};
  const NormRun r = normalise_and_align(imgs);
  REQUIRE_FALSE(r.norm.has_value());
  CHECK(r.norm.error().code == "input.turnaround_sheet");
  CHECK(W(r.norm.error())["view"] == "front");
  CHECK(W(r.norm.error())["figures"] == 3);
  CHECK(r.norm.error().text.find("separate") != std::string::npos);
}

TEST_CASE("normalize: a view that is only background is refused") {
  std::map<ViewRole, ImageRGBA> imgs;
  for (const auto& [role, img] : character_views()) imgs.emplace(role, on_background(upscale(img, 6, 20), kWhite));
  imgs[ViewRole::Side] = noisy(ImageRGBA(200, 300, kWhite), 5, 11);
  const NormRun r = normalise_and_align(imgs);
  REQUIRE_FALSE(r.norm.has_value());
  CHECK(r.norm.error().code == "input.no_subject");
  CHECK(W(r.norm.error())["view"] == "side");
}

TEST_CASE("normalize: views whose shared axes disagree beyond the tolerance are refused, never too_large") {
  // Top drawn twice as deep as Side shows: Z disagrees by 100 %.
  const auto clean = character_views(), deep = character_views(2);
  std::map<ViewRole, ImageRGBA> imgs{{ViewRole::Front, on_background(antialiased(clean.at(ViewRole::Front), 30, 20), kWhite)},
                                     {ViewRole::Side, on_background(antialiased(clean.at(ViewRole::Side), 30, 20), kWhite)},
                                     {ViewRole::Top, on_background(antialiased(deep.at(ViewRole::Top), 30, 20), kWhite)}};
  const NormRun r = normalise_and_align(imgs);
  REQUIRE_FALSE(r.norm.has_value());
  const Message& e = r.norm.error();
  CHECK(e.code == "input.views_inconsistent");
  CHECK(W(e)["axis"] == "Z");
  CHECK(e.where.contains("extents"));
  CHECK(W(e)["tolerance"] == doctest::Approx(0.15));
  CHECK(e.text.find("Top") != std::string::npos);
  // A looser tolerance accepts it (and then alignment reports the mismatch honestly).
  NormalizeSettings loose;
  loose.axis_tolerance = 1.5;
  const NormRun r2 = normalise_and_align(imgs, loose);
  CHECK(r2.norm.has_value());
}

TEST_CASE("normalize: small shared-axis differences are evened out per view, with a warning") {
  // Painted art without a pixel grid; Side captured 8 % larger than Front and Top.
  const auto clean = character_views();
  auto paint = [](const ImageRGBA& v, double f) { return on_background(dithered(antialiased(v, f, 20), 60), kWhite); };
  std::map<ViewRole, ImageRGBA> imgs{{ViewRole::Front, paint(clean.at(ViewRole::Front), 10)},
                                     {ViewRole::Side, paint(clean.at(ViewRole::Side), 10.8)},
                                     {ViewRole::Top, paint(clean.at(ViewRole::Top), 10)}};
  const NormRun r = normalise_and_align(imgs);
  REQUIRE(r.norm.has_value());
  REQUIRE(r.align.has_value());
  for (const SourceView& v : r.views) CHECK(v.norm.block == 1);  // no grid to find
  // Fitted to the target; Side's height now equals Front's.
  CHECK(r.align->dims.H == 128);
  REQUIRE(find(r.views, ViewRole::Side).working);
  REQUIRE(find(r.views, ViewRole::Front).working);
  CHECK(find(r.views, ViewRole::Side).working->height() == find(r.views, ViewRole::Front).working->height());
  bool warned = false;
  for (const Message& m : r.norm->messages)
    if (m.code == "input.view_rescaled") {
      warned = true;
      CHECK(m.severity == Severity::Warning);
      CHECK(W(m)["view"] == "side");
    }
  CHECK(warned);
}

// ---- settings and determinism ------------------------------------------------------------

TEST_CASE("normalize: settings are range-checked") {
  NormalizeSettings s;
  CHECK_FALSE(check_normalize_settings(s).has_value());
  using Mut = void (*)(NormalizeSettings&);
  for (Mut bad : std::initializer_list<Mut>{[](NormalizeSettings& x) { x.target_size = 8; }, [](NormalizeSettings& x) { x.target_size = 600; },
                   [](NormalizeSettings& x) { x.bg_tolerance = 300; }, [](NormalizeSettings& x) { x.bg_tolerance = -1; },
                   [](NormalizeSettings& x) { x.axis_tolerance = -0.1; }, [](NormalizeSettings& x) { x.axis_tolerance = 2; }}) {
    NormalizeSettings t;
    bad(t);
    const auto e = check_normalize_settings(t);
    REQUIRE(e.has_value());
    CHECK(e->code == "input.invalid");
  }
}

TEST_CASE("normalize: deterministic, and reproduced by save/load and undo through the session") {
  const fs::path dir = fs::temp_directory_path() / "pixel3d_normalize_session";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::vector<ViewImport> imports;
  uint32_t seed = 21;
  for (const auto& [role, img] : character_views()) {
    const fs::path p = dir / (std::string(to_string(role)) + ".png");
    REQUIRE(save_png(noisy(on_background(antialiased(img, 17, 12), kWhite), 5, seed++), p).code == "ok");
    imports.push_back({role, p, std::nullopt});
  }
  Session s;
  REQUIRE(s.new_project(ProjectSettings{}).success);
  REQUIRE(s.import_views(imports).success);
  const OpResult a = s.auto_align();
  REQUIRE(a.success);
  std::string notes;
  for (const auto* list : {&a.info, &a.warnings})
    for (const Message& m : *list) notes += m.code + ": " + m.text + "\n";
  const Dims got = s.document().align->dims;
  INFO("dims " << got.W << "x" << got.H << "x" << got.D << "\n" << notes);
  CHECK(got == kCharDims);
  std::map<ViewRole, ImageRGBA> first;
  for (const SourceView& v : s.document().views) first.emplace(v.role, *v.aligned);
  // Again: identical.
  REQUIRE(s.auto_align().success);
  for (const SourceView& v : s.document().views) CHECK(*v.aligned == first.at(v.role));
  // Save, load, align: identical; the originals are stored, not the working images.
  REQUIRE(s.save(dir / "p.p3d").success);
  Session t;
  REQUIRE(t.load(dir / "p.p3d").success);
  REQUIRE(t.auto_align().success);
  for (const SourceView& v : t.document().views) CHECK(*v.aligned == first.at(v.role));
  // Undo back to before the alignment and redo it: identical.
  REQUIRE(s.undo().success);
  REQUIRE(s.auto_align().success);
  for (const SourceView& v : s.document().views) CHECK(*v.aligned == first.at(v.role));
  fs::remove_all(dir);
}

TEST_CASE("normalize: AI-style art with irregular 8-12 px blocks, noise and near-opaque alpha finds its scale") {
  // Each source pixel becomes a block of 8..12 px (deterministic) with its own
  // shade (-18, 0 or +18: generated art shades every block), every pixel gets
  // small noise and alpha 250, like the generated images users send.
  auto ai_style = [](const ImageRGBA& src, uint32_t seed) {
    uint32_t s = seed;
    auto rnd = [&](int n) {
      s = s * 1664525u + 1013904223u;
      return static_cast<int>((s >> 16) % static_cast<uint32_t>(n));
    };
    std::vector<int> xs{0}, ys{0};
    for (int i = 0; i < src.width(); ++i) xs.push_back(xs.back() + 8 + rnd(5));
    for (int i = 0; i < src.height(); ++i) ys.push_back(ys.back() + 8 + rnd(5));
    const int m = 20;
    ImageRGBA out(xs.back() + 2 * m, ys.back() + 2 * m);
    for (int by = 0; by < src.height(); ++by)
      for (int bx = 0; bx < src.width(); ++bx) {
        const Rgba8 c = src.at(bx, by);
        if (!c.a) continue;
        const int shade = 18 * (rnd(3) - 1);
        for (int y = ys[by]; y < ys[by + 1]; ++y)
          for (int x = xs[bx]; x < xs[bx + 1]; ++x) {
            auto add = [&](uint8_t v) { return static_cast<uint8_t>(std::clamp(v + shade + rnd(9) - 4, 0, 255)); };
            out.at(x + m, y + m) = Rgba8{add(c.r), add(c.g), add(c.b), 250};
          }
      }
    return out;
  };
  std::map<ViewRole, ImageRGBA> imgs;
  uint32_t seed = 5;
  for (const auto& [role, img] : character_views()) imgs.emplace(role, ai_style(img, seed++));
  const NormRun r = normalise_and_align(imgs);
  if (!r.norm) FAIL_CHECK(r.norm.error().text);
  REQUIRE(r.norm.has_value());
  REQUIRE(r.align.has_value());
  for (const SourceView& v : r.views) {
    CAPTURE(to_string(v.role));
    CHECK(v.norm.block == doctest::Approx(10).epsilon(0.15));
  }
  // Within a pixel or two of the clean model on every axis.
  CHECK(std::abs(r.align->dims.W - kCharDims.W) <= 2);
  CHECK(std::abs(r.align->dims.H - kCharDims.H) <= 2);
  CHECK(std::abs(r.align->dims.D - kCharDims.D) <= 2);
}
