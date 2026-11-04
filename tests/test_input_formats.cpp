#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include <miniz.h>

#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/limits.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/image/image.hpp"
#include "pixel3d/image/layered.hpp"

// 1.2.0 input formats: WebP (libwebp), Aseprite and PSD (layered sources).
// Ground truth never comes from our own decoder: the Aseprite files were written by
// Aseprite 1.3.18 with its own PNG exports of frame 1 (whole image and single layers);
// the PSD files were written by Krita 5 (from OpenRaster) and their composites read by
// Pillow; the WebP files were encoded by Pillow's libwebp from the Aseprite export.
// tests/fixtures/G_layered/README.md records how each file was made.

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

std::string gfix(const std::string& name) { return std::string(PIXEL3D_FIXTURES_DIR) + "/G_layered/" + name; }

std::vector<uint8_t> bytes_of(const std::string& name) {
  auto b = read_file(gfix(name), limits::kMaxImageFileBytes);
  REQUIRE_MESSAGE(b.has_value(), name);
  return *b;
}

ImageRGBA png(const std::string& name) {
  auto i = load_png(gfix(name));
  REQUIRE_MESSAGE(i.has_value(), name);
  return *i;
}

// Fully transparent pixels compare equal whatever their RGB (exporters differ there).
bool same_pixels(const ImageRGBA& a, const ImageRGBA& b) {
  if (a.width() != b.width() || a.height() != b.height()) return false;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) {
      const Rgba8 p = a.at(x, y), q = b.at(x, y);
      if (p.a == 0 && q.a == 0) continue;
      if (!(p == q)) return false;
    }
  return true;
}

const SourceLayer* layer(const LayeredImage& l, const std::string& name) {
  for (const auto& s : l.layers)
    if (s.name == name) return &s;
  return nullptr;
}

// Alpha > 0 of a layer, on the canvas.
Mask2D layer_mask(const SourceLayer& s, int w, int h) {
  Mask2D m(w, h);
  for (int y = 0; y < s.pixels.height(); ++y)
    for (int x = 0; x < s.pixels.width(); ++x)
      if (s.pixels.at(x, y).a > 0) m.set(s.rect.x0 + x, s.rect.y0 + y, true);
  return m;
}

Mask2D alpha_mask(const ImageRGBA& img) {
  Mask2D m(img.width(), img.height());
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x) m.set(x, y, img.at(x, y).a > 0);
  return m;
}

bool has_note(const LayeredImage& l, const std::string& code) {
  return std::any_of(l.notes.begin(), l.notes.end(), [&](const Message& m) { return m.code == code; });
}

// Every prefix and every single-byte change of a valid file: never a crash (UBSan in
// Debug), and a truncated file is never decoded as an image.
void robustness(const std::vector<uint8_t>& good, const char* hint) {
  for (size_t n = 0; n < good.size(); n += std::max<size_t>(1, good.size() / 400)) {
    const std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<ptrdiff_t>(n));
    const auto r = decode_image(cut, hint);
    const std::string what = std::string(hint) + " decoded from its first " + std::to_string(n) + " bytes";
    CHECK_FALSE_MESSAGE(r.has_value(), what);
  }
  for (size_t i = 0; i < good.size(); i += std::max<size_t>(1, good.size() / 300)) {
    std::vector<uint8_t> bad = good;
    bad[i] ^= 0xA5;
    const auto r = decode_image(bad, hint);
    if (r) {
      CHECK(r->width() > 0);
      CHECK(r->height() > 0);
    }
  }
}

}  // namespace

TEST_CASE("input formats: signatures and extensions") {
  CHECK(sniff_image_format(bytes_of("rgb_layers.aseprite")) == ImageFormat::Aseprite);
  CHECK(sniff_image_format(bytes_of("krita_layers.psd")) == ImageFormat::Psd);
  CHECK(sniff_image_format(bytes_of("lossless.webp")) == ImageFormat::Webp);
  CHECK(file_extension(ImageFormat::Aseprite) == "aseprite");
  CHECK(file_extension(ImageFormat::Psd) == "psd");
  const std::vector<uint8_t> junk{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  for (const char* name : {"x.aseprite", "x.ase", "x.psd", "x.webp"}) {
    CAPTURE(name);
    CHECK(decode_image(junk, name).error().code == "image.decode_failed");
  }
}

TEST_CASE("input formats: Aseprite first frame equals Aseprite's own export") {
  for (const auto& [file, expected] : {std::pair{"rgb_layers.aseprite", "rgb_layers_frame1.png"},
                                       std::pair{"indexed.aseprite", "indexed_frame1.png"},
                                       std::pair{"groups_opacity.aseprite", "groups_opacity_frame1.png"},
                                       std::pair{"grayscale.aseprite", "grayscale_frame1.png"}}) {
    CAPTURE(file);
    const auto img = decode_image(bytes_of(file), file);
    REQUIRE_MESSAGE(img.has_value(), img.error().text);
    CHECK(same_pixels(*img, png(expected)));
  }
}

TEST_CASE("input formats: Aseprite layers become suggestions with their visibility and groups") {
  const auto l = decode_aseprite(bytes_of("rgb_layers.aseprite"));
  REQUIRE(l.has_value());
  CHECK(l->format == ImageFormat::Aseprite);
  CHECK(has_note(*l, "layered.first_frame"));  // two frames: only the first is used
  REQUIRE(l->layers.size() == 3);
  for (const auto& [name, file] : {std::pair{"body", "rgb_layers_frame1_body.png"}, std::pair{"head", "rgb_layers_frame1_head.png"}}) {
    CAPTURE(name);
    const SourceLayer* s = layer(*l, name);
    REQUIRE(s);
    CHECK(s->visible);
    CHECK(layer_mask(*s, 10, 12) == alpha_mask(png(file)));  // Aseprite's single-layer export
  }
  const SourceLayer* hat = layer(*l, "hat_hidden");
  REQUIRE(hat);
  CHECK_FALSE(hat->visible);
  CHECK(layer_mask(*hat, 10, 12).count() == 12);  // its pixels are still read (2 x 6)

  const auto g = decode_aseprite(bytes_of("groups_opacity.aseprite"));
  REQUIRE(g.has_value());
  const SourceLayer* decal = layer(*g, "decal");
  const SourceLayer* secret = layer(*g, "secret");
  REQUIRE(decal);
  REQUIRE(secret);
  CHECK(decal->path == "grp/decal");
  CHECK(decal->visible);
  CHECK(secret->path == "hidden_grp/secret");
  CHECK_FALSE(secret->visible);  // the layer is visible, its group is not
  REQUIRE(decal->rect.x0 <= 1);
  CHECK(decal->pixels.at(1 - decal->rect.x0, 0 - decal->rect.y0).a == 128);  // canvas (1, 0): layer opacity applied
}

TEST_CASE("input formats: PSD composite equals the composite Krita stored (read by Pillow)") {
  for (const auto& [file, expected] : {std::pair{"krita_layers.psd", "krita_layers_merged.png"},
                                       std::pair{"krita_groups.psd", "krita_groups_merged.png"}}) {
    CAPTURE(file);
    const auto img = decode_image(bytes_of(file), file);
    REQUIRE_MESSAGE(img.has_value(), img.error().text);
    CHECK(same_pixels(*img, png(expected)));
  }
}

TEST_CASE("input formats: PSD raster layers, visibility and groups") {
  const auto l = decode_psd(bytes_of("krita_layers.psd"));
  REQUIRE_MESSAGE(l.has_value(), l.error().text);
  REQUIRE(l->layers.size() == 3);
  const SourceLayer* body = layer(*l, "body");
  const SourceLayer* head = layer(*l, "head");
  const SourceLayer* hat = layer(*l, "hat_hidden");
  REQUIRE(body);
  REQUIRE(head);
  REQUIRE(hat);
  // The OpenRaster source: body x 2..7, y 5..11; head x 3..6, y 1..5; hat x 2..7, y 0..1.
  auto rect_mask = [](int x0, int y0, int x1, int y1) {
    Mask2D m(10, 12);
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) m.set(x, y, true);
    return m;
  };
  CHECK(layer_mask(*body, 10, 12) == rect_mask(2, 5, 8, 12));
  CHECK(layer_mask(*head, 10, 12) == rect_mask(3, 1, 7, 6));
  CHECK(layer_mask(*hat, 10, 12) == rect_mask(2, 0, 8, 2));
  CHECK(body->visible);
  CHECK(head->visible);
  CHECK_FALSE(hat->visible);
  CHECK(body->pixels.at(0, 0) == Rgba8{0x7a, 0x4e, 0x2c, 255});

  const auto g = decode_psd(bytes_of("krita_groups.psd"));
  REQUIRE(g.has_value());
  REQUIRE(g->layers.size() == 3);  // group records are not raster layers
  const SourceLayer* decal = layer(*g, "decal");
  const SourceLayer* secret = layer(*g, "secret");
  REQUIRE(decal);
  REQUIRE(secret);
  CHECK(decal->path == "grp/decal");
  CHECK(decal->visible);
  CHECK(secret->path == "hidden_grp/secret");
  CHECK_FALSE(secret->visible);
}

TEST_CASE("input formats: WebP lossless, lossy and the first frame of an animation") {
  const ImageRGBA expected = png("rgb_layers_frame1.png");
  const auto lossless = decode_image(bytes_of("lossless.webp"), "lossless.webp");
  REQUIRE_MESSAGE(lossless.has_value(), lossless.error().text);
  CHECK(same_pixels(*lossless, expected));
  const auto anim = decode_image(bytes_of("animated.webp"), "animated.webp");
  REQUIRE_MESSAGE(anim.has_value(), anim.error().text);
  CHECK(same_pixels(*anim, expected));  // frame 2 has a red pixel at (0, 0)
  const auto lossy = decode_image(bytes_of("lossy.webp"), "lossy.webp");
  REQUIRE_MESSAGE(lossy.has_value(), lossy.error().text);
  CHECK(lossy->width() == expected.width());
  CHECK(lossy->height() == expected.height());
}

TEST_CASE("input formats: corrupt files never crash and truncated files are never decoded") {
  for (const char* file : {"rgb_layers.aseprite", "indexed.aseprite", "groups_opacity.aseprite", "grayscale.aseprite",
                           "krita_layers.psd",
                           "krita_groups.psd", "lossless.webp", "lossy.webp", "animated.webp"}) {
    CAPTURE(file);
    robustness(bytes_of(file), file);
  }
}

TEST_CASE("input formats: oversized headers are refused before any pixel is allocated") {
  // Aseprite: canvas width/height at bytes 8-11 (little endian).
  std::vector<uint8_t> ase = bytes_of("rgb_layers.aseprite");
  ase[8] = ase[9] = ase[10] = ase[11] = 0xFF;  // 65535 x 65535
  CHECK(decode_image(ase, "x.aseprite").error().code == "image.too_large");
  // PSD: height and width at bytes 14-21 (big endian).
  std::vector<uint8_t> psd = bytes_of("krita_layers.psd");
  for (int i = 14; i < 22; ++i) psd[static_cast<size_t>(i)] = i % 4 == 2 ? 0x00 : 0x7F;
  CHECK(decode_image(psd, "x.psd").error().code == "image.too_large");
  // WebP lossless: 14-bit width-1 and height-1 after the 0x2F signature byte at offset 20.
  std::vector<uint8_t> webp = bytes_of("lossless.webp");
  REQUIRE(webp[20] == 0x2F);
  webp[21] = 0xFF, webp[22] = 0xFF, webp[23] = 0xFF;
  webp[24] = static_cast<uint8_t>((webp[24] & 0xF0) | 0x0F);  // 16384 x 16384
  CHECK(decode_image(webp, "x.webp").error().code == "image.too_large");
  // Too large on disk.
  std::vector<uint8_t> big(static_cast<size_t>(limits::kMaxImageFileBytes) + 1, 0);
  std::copy(ase.begin(), ase.begin() + 16, big.begin());
  CHECK(decode_image(big, "x.aseprite").error().code == "image.too_large");
}

TEST_CASE("input formats: an Aseprite cel that inflates beyond its size is refused (zlib bomb)") {
  // A minimal valid file: one frame, one layer, one compressed 2 x 2 RGBA cel whose zlib
  // stream holds 1 MiB of zeros.
  std::vector<uint8_t> f(128, 0);
  auto put16 = [](std::vector<uint8_t>& v, size_t at, uint16_t x) { v[at] = x & 255, v[at + 1] = x >> 8; };
  auto put32 = [](std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
  };
  put16(f, 4, 0xA5E0);
  put16(f, 6, 1);
  put16(f, 8, 2);
  put16(f, 10, 2);
  put16(f, 12, 32);
  std::vector<uint8_t> zeros(1 << 20, 0), z(compressBound(static_cast<mz_ulong>(zeros.size())));
  mz_ulong zlen = static_cast<mz_ulong>(z.size());
  REQUIRE(compress(z.data(), &zlen, zeros.data(), static_cast<mz_ulong>(zeros.size())) == MZ_OK);
  z.resize(zlen);
  std::vector<uint8_t> layer_chunk(6 + 18, 0), cel_chunk(6 + 20, 0);
  put32(layer_chunk, 0, static_cast<uint32_t>(layer_chunk.size()));
  put16(layer_chunk, 4, 0x2004);
  put16(layer_chunk, 6, 1);  // visible
  layer_chunk[18] = 255;     // opacity
  put32(cel_chunk, 0, static_cast<uint32_t>(cel_chunk.size() + z.size()));
  put16(cel_chunk, 4, 0x2005);
  // Cel: layer 6, x 8, y 10, opacity 12, type 13, z-index 15, reserved 17-21, w 22, h 24.
  cel_chunk[12] = 255;
  put16(cel_chunk, 13, 2);  // compressed image
  put16(cel_chunk, 22, 2);
  put16(cel_chunk, 24, 2);
  std::vector<uint8_t> frame(16, 0);
  put32(frame, 0, static_cast<uint32_t>(16 + layer_chunk.size() + cel_chunk.size() + z.size()));
  put16(frame, 4, 0xF1FA);
  put16(frame, 6, 2);
  f.insert(f.end(), frame.begin(), frame.end());
  f.insert(f.end(), layer_chunk.begin(), layer_chunk.end());
  f.insert(f.end(), cel_chunk.begin(), cel_chunk.end());
  f.insert(f.end(), z.begin(), z.end());
  put32(f, 0, static_cast<uint32_t>(f.size()));
  const auto r = decode_image(f, "bomb.aseprite");
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == "image.decode_failed");
  CHECK(r.error().text.find("compressed cel") != std::string::npos);
  // The same file with an honest 2 x 2 stream decodes: the refusal above is the bound.
  std::vector<uint8_t> four(16, 200), z2(compressBound(16));
  mz_ulong z2len = static_cast<mz_ulong>(z2.size());
  REQUIRE(compress(z2.data(), &z2len, four.data(), 16) == MZ_OK);
  std::vector<uint8_t> g(f.begin(), f.begin() + static_cast<ptrdiff_t>(f.size() - z.size()));
  g.insert(g.end(), z2.begin(), z2.begin() + static_cast<ptrdiff_t>(z2len));
  put32(g, 0, static_cast<uint32_t>(g.size()));
  put32(g, 128, static_cast<uint32_t>(16 + layer_chunk.size() + cel_chunk.size() + z2len));
  put32(g, 128 + 16 + layer_chunk.size(), static_cast<uint32_t>(cel_chunk.size() + z2len));
  const auto ok = decode_image(g, "small.aseprite");
  REQUIRE_MESSAGE(ok.has_value(), ok.error().text);
  CHECK(ok->at(1, 1) == Rgba8{200, 200, 200, 200});
}

TEST_CASE("input formats: a layered file is imported as a view and kept in the project as it was") {
  const fs::path dir = fs::temp_directory_path() / "pixel3d_tests" / "input_formats";
  fs::create_directories(dir);
  Session s;
  ProjectSettings ps;
  ps.name = "layered";
  ps.lowpoly.enabled = true;
  REQUIRE(s.new_project(ps).success);
  const auto imp = s.import_views({{ViewRole::Front, gfix("rgb_layers.aseprite"), {}}, {ViewRole::Side, gfix("krita_layers.psd"), {}}});
  REQUIRE_MESSAGE(imp.success, to_json(imp).dump());
  REQUIRE(s.save((dir / "layered.p3d").string()).success);
  Session t;
  const OpResult loaded = t.load(dir / "layered.p3d");
  REQUIRE_MESSAGE(loaded.success, to_json(loaded).dump());
  REQUIRE(t.document().views.size() == 2);
  for (const SourceView& v : t.document().views) {
    CAPTURE(to_string(v.role));
    REQUIRE(v.original_bytes);
    CHECK(*v.original_bytes == bytes_of(v.role == ViewRole::Front ? "rgb_layers.aseprite" : "krita_layers.psd"));
    CHECK(same_pixels(*v.original, png(v.role == ViewRole::Front ? "rgb_layers_frame1.png" : "krita_layers_merged.png")));
  }
}

TEST_CASE("layer parts: visible layers are suggested in aligned view pixels; the user accepts or rejects them") {
  Session s;
  ProjectSettings ps;
  ps.name = "layers";
  ps.lowpoly.enabled = true;
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views({{ViewRole::Front, gfix("rgb_layers.aseprite"), {}}, {ViewRole::Side, gfix("krita_layers.psd"), {}}}).success);
  const auto revision = s.current_status().revision;
  const OpResult p = s.suggest_layer_parts({});
  REQUIRE_MESSAGE(p.success, to_json(p).dump());
  CHECK(p.revision == revision);  // a proposal changes nothing
  CHECK(s.document().masks.parts.empty());
  std::map<std::string, nlohmann::json> by;
  for (const auto& l : p.data["layers"]) by[l["name"].get<std::string>()] = l;
  REQUIRE(by.size() == 3);
  CHECK(by["body"]["suggested"] == true);
  CHECK(by["body"]["type"] == "torso");
  CHECK(by["head"]["type"] == "head");
  CHECK(by["hat_hidden"]["suggested"] == false);
  CHECK(by["hat_hidden"]["why_not"] == "hidden layer");
  // Aligned Front = the drawing's opaque box (x 2..7, y 1..11): head 4 x 5, body 6 x 7.
  CHECK(by["head"]["views"]["front"]["pixels"] == 20);
  CHECK(by["head"]["views"]["front"]["box"] == nlohmann::json{1, 0, 5, 5});
  CHECK(by["body"]["views"]["front"]["pixels"] == 42);
  CHECK(by["body"]["views"]["side"]["pixels"] == 42);  // the PSD side view, same layer name

  // Accept only "head": one part, one undo step.
  const OpResult a = s.suggest_layer_parts({.apply = true, .accept = {"head"}});
  REQUIRE_MESSAGE(a.success, to_json(a).dump());
  REQUIRE(s.document().masks.parts.size() == 1);
  const int id = a.data["part_ids"][0].get<int>();
  CHECK(s.document().part_names.at(id) == "head");
  CHECK(s.document().part_types.at(id) == lowpoly::PartType::Head);
  CHECK(s.document().masks.parts.at(id).masks.at(ViewRole::Front).count() == 20);
  // Applying again over existing parts needs replace; replace takes every suggested layer.
  CHECK_FALSE(s.suggest_layer_parts({.apply = true}).success);
  REQUIRE(s.suggest_layer_parts({.apply = true, .replace = true}).success);
  CHECK(s.document().masks.parts.size() == 2);  // body + head; the hidden layer is never created
  REQUIRE(s.undo().success);
  CHECK(s.document().masks.parts.size() == 1);
  // A layer name that does not exist is refused; a project without layered views says so.
  CHECK_FALSE(s.suggest_layer_parts({.apply = true, .replace = true, .accept = {"nope"}}).success);
  Session plain;
  REQUIRE(plain.new_project(ps).success);
  REQUIRE(plain.import_views({{ViewRole::Front, gfix("rgb_layers_frame1.png"), {}}}).success);
  const OpResult none = plain.suggest_layer_parts({});
  REQUIRE_FALSE(none.success);
  CHECK(none.errors.front().code == "layers.none");
}
