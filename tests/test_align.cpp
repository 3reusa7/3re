#include <doctest/doctest.h>

#include "pixel3d/views/align.hpp"
#include "test_helpers.hpp"

using namespace pixel3d;
using p3dtest::Box;

namespace {

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

bool has_code(const AlignResult& r, const std::string& code) {
  for (const auto& m : r.messages)
    if (m.code == code) return true;
  return false;
}

// Asymmetric along every axis: base slab + a corner post + a separate front bar with a Z gap.
std::vector<Box> asym_object() {
  return {Box{0, 0, 0, 6, 2, 3}, Box{0, 2, 0, 2, 6, 1}, Box{0, 0, 4, 3, 1, 5}};
}

}  // namespace

TEST_CASE("align: extents agree after crop") {
  const Dims d{3, 4, 2};
  auto imgs = p3dtest::render_views({Box{0, 0, 0, 3, 4, 2}}, d);
  for (auto& [role, img] : imgs) img = p3dtest::upscale_padded(img, 1, 2, 1, 3, 2);
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE(r.has_value());
  CHECK(r->dims.W == 3);
  CHECK(r->dims.H == 4);
  CHECK(r->dims.D == 2);
  for (const auto& m : r->messages) CHECK(m.severity == Severity::Info);
  CHECK(find(views, ViewRole::Side).aligned->width() == 2);
}

TEST_CASE("align: 4x upscaled padded object equals 1x") {
  const Dims d{6, 6, 5};
  const auto imgs = p3dtest::render_views(asym_object(), d);
  auto views1 = make_views(imgs);
  std::map<ViewRole, ImageRGBA> big;
  for (const auto& [role, img] : imgs) big.emplace(role, p3dtest::upscale_padded(img, 4, 3, 5, 7, 2));
  auto views4 = make_views(big);
  const auto r1 = auto_align(views1, AlignOptions{});
  const auto r4 = auto_align(views4, AlignOptions{});
  REQUIRE(r1.has_value());
  REQUIRE(r4.has_value());
  CHECK(r4->dims.W == r1->dims.W);
  CHECK(r4->dims.H == r1->dims.H);
  CHECK(r4->dims.D == r1->dims.D);
  for (ViewRole role : {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
    CHECK(find(views4, role).scale.k == 4);
    CHECK(*find(views4, role).aligned == *find(views1, role).aligned);
  }
}

TEST_CASE("align: 4x upscaled padded fixture A equals 1x") {
  std::map<ViewRole, ImageRGBA> imgs, big;
  for (ViewRole r : {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
    auto img = load_png(std::string(PIXEL3D_FIXTURES_DIR) + "/A_synthetic_chair/" + std::string(to_string(r)) + ".png");
    REQUIRE(img.has_value());
    big.emplace(r, p3dtest::upscale_padded(*img, 4, 9, 3, 1, 6));
    imgs.emplace(r, std::move(*img));
  }
  auto v1 = make_views(imgs), v4 = make_views(big);
  const auto r1 = auto_align(v1, AlignOptions{});
  const auto r4 = auto_align(v4, AlignOptions{});
  REQUIRE(r1.has_value());
  REQUIRE(r4.has_value());
  CHECK(r1->dims == r4->dims);
  CHECK(r1->dims == Dims{18, 33, 20});  // crop: x∈[1,19), y∈[0,33), z∈[1,21)
  for (ViewRole r : {ViewRole::Front, ViewRole::Side, ViewRole::Top})
    CHECK(*find(v4, r).aligned == *find(v1, r).aligned);
}

TEST_CASE("align: mirrored side is detected and flipped") {
  const Dims d{6, 6, 5};
  auto imgs = p3dtest::render_views(asym_object(), d);
  const ImageRGBA correct_side = imgs.at(ViewRole::Side);
  imgs.at(ViewRole::Side) = flip_x(correct_side);
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE(r.has_value());
  CHECK(find(views, ViewRole::Side).flipped_x);
  CHECK(r->flip_confidence.at(ViewRole::Side) > 0.5);
  CHECK(*find(views, ViewRole::Side).aligned == correct_side);
  CHECK(r->profile_conflicts.at(ViewRole::Side) == 0);
}

TEST_CASE("align: forced flip overrides detection") {
  const Dims d{6, 6, 5};
  auto views = make_views(p3dtest::render_views(asym_object(), d));
  AlignOptions opt;
  opt.force_flip[ViewRole::Side] = true;
  const auto r = auto_align(views, opt);
  REQUIRE(r.has_value());
  CHECK(find(views, ViewRole::Side).flipped_x);
  CHECK(find(views, ViewRole::Side).flip_user_override);
  CHECK(r->profile_conflicts.at(ViewRole::Side) > 0);
}

TEST_CASE("align: extent mismatch beyond tolerance is a conflict") {
  std::map<ViewRole, ImageRGBA> imgs;
  imgs.emplace(ViewRole::Front, ImageRGBA(10, 4, Rgba8{200, 0, 0, 255}));
  imgs.emplace(ViewRole::Side, ImageRGBA(3, 4, Rgba8{200, 0, 0, 255}));
  imgs.emplace(ViewRole::Top, ImageRGBA(13, 3, Rgba8{200, 0, 0, 255}));
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE(r.has_value());
  CHECK(has_code(*r, "extent_mismatch"));
  CHECK(r->dims.W == 13);  // every source pixel is kept; the overhang becomes measurable conflict
  CHECK(r->profile_conflicts.at(ViewRole::Top) > 0);
}

TEST_CASE("align: one pixel mismatch within tolerance is padded with a warning") {
  std::map<ViewRole, ImageRGBA> imgs;
  imgs.emplace(ViewRole::Front, ImageRGBA(10, 4, Rgba8{200, 0, 0, 255}));
  imgs.emplace(ViewRole::Side, ImageRGBA(3, 4, Rgba8{200, 0, 0, 255}));
  imgs.emplace(ViewRole::Top, ImageRGBA(11, 3, Rgba8{200, 0, 0, 255}));
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE(r.has_value());
  CHECK(has_code(*r, "extent_padded"));
  CHECK_FALSE(has_code(*r, "extent_mismatch"));
  CHECK(r->dims.W == 11);
}

TEST_CASE("align: one-pixel-longer leg aligns by structure, not by a tie") {
  // Stool: two legs and a seat. Front draws the left leg one pixel too long, so
  // Front is one row taller than Side; every row is occupied in both views, so
  // only the row structure can tell where Side belongs.
  const Dims d{5, 4, 5};
  auto imgs = p3dtest::render_views({Box{0, 0, 0, 1, 3, 5}, Box{4, 0, 0, 5, 3, 5}, Box{0, 3, 0, 5, 4, 5}}, d);
  ImageRGBA longer(5, 5);
  for (int v = 0; v < 4; ++v)
    for (int u = 0; u < 5; ++u) longer.at(u, v) = imgs.at(ViewRole::Front).at(u, v);
  longer.at(0, 4) = Rgba8{150, 100, 60, 255};
  imgs.at(ViewRole::Front) = longer;
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE(r.has_value());
  CHECK(r->dims.H == 5);
  CHECK(r->profile_conflicts.at(ViewRole::Front) == 1);  // only the extra pixel
  CHECK(r->profile_conflicts.at(ViewRole::Side) == 0);
}

TEST_CASE("align: missing Top is error view.missing_required") {
  auto imgs = p3dtest::render_views({Box{0, 0, 0, 2, 2, 2}}, Dims{2, 2, 2}, {ViewRole::Front, ViewRole::Side});
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == "view.missing_required");
}

TEST_CASE("align: fully transparent view is error view.empty") {
  auto imgs = p3dtest::render_views({Box{0, 0, 0, 2, 2, 2}}, Dims{2, 2, 2});
  imgs.at(ViewRole::Top) = ImageRGBA(4, 4);
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().code == "view.empty");
}

TEST_CASE("align: optional back and bottom views are aligned to their partners") {
  const Dims d{6, 6, 5};
  auto imgs = p3dtest::render_views(asym_object(), d,
                                    {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Back, ViewRole::Bottom});
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE(r.has_value());
  CHECK_FALSE(find(views, ViewRole::Back).flipped_x);
  CHECK(*find(views, ViewRole::Back).aligned == imgs.at(ViewRole::Back));
  CHECK(*find(views, ViewRole::Bottom).aligned == imgs.at(ViewRole::Bottom));
  CHECK(r->profile_conflicts.at(ViewRole::Back) == 0);
}

TEST_CASE("align: stray pixel produces profile conflicts") {
  const Dims d{6, 6, 5};
  auto imgs = p3dtest::render_views(asym_object(), d);
  ImageRGBA front(8, 6);  // two extra columns on the right with one stray pixel
  for (int y = 0; y < 6; ++y)
    for (int x = 0; x < 6; ++x) front.at(x, y) = imgs.at(ViewRole::Front).at(x, y);
  front.at(7, 0) = Rgba8{10, 200, 10, 255};
  imgs.at(ViewRole::Front) = front;
  auto views = make_views(imgs);
  const auto r = auto_align(views, AlignOptions{});
  REQUIRE(r.has_value());
  CHECK(r->profile_conflicts.at(ViewRole::Front) >= 1);
}
