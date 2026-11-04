#include <doctest/doctest.h>

#include "fixturegen/lowpoly_character.hpp"
#include "lowpoly_helpers.hpp"
#include "pixel3d/image/mask.hpp"

using namespace pixel3d;
using namespace p3dtest;

TEST_CASE("lowpoly fixture: committed E drawings equal the generator output") {
  for (const auto& [variant, fx] : {std::pair{std::string("cloaked"), p3dfix::make_cloaked_character()},
                                   std::pair{std::string("humanoid"), p3dfix::make_humanoid_character()}}) {
    CAPTURE(variant);
    const std::string dir = lowpoly_fixture_dir(variant);
    CHECK(*load_png(dir + "/front.png") == fx.front.art);
    CHECK(*load_png(dir + "/side.png") == fx.side.art);
    CHECK(*load_png(dir + "/front_parts.png") == fx.front.parts);
    CHECK(*load_png(dir + "/side_parts.png") == fx.side.parts);
  }
}

TEST_CASE("lowpoly fixture: front + side align to the tight ground-truth box") {
  for (const char* variant : {"cloaked", "humanoid"}) {
    CAPTURE(variant);
    const lowpoly::Input in = load_lowpoly_fixture(variant);
    const auto meta = lowpoly_fixture_meta(variant);
    CHECK(in.dims == Dims{meta["dims"][0].get<int>(), meta["dims"][1].get<int>(), meta["dims"][2].get<int>()});
    REQUIRE(in.views.size() == 2);
    for (const auto& v : in.views) {
      const auto sz = view_image_size(v.role, in.dims);
      CHECK(v.aligned->width() == sz.w);
      CHECK(v.aligned->height() == sz.h);
    }
  }
}

TEST_CASE("lowpoly fixture: part masks cover exactly the opaque pixels, without overlap") {
  for (const char* variant : {"cloaked", "humanoid"}) {
    CAPTURE(variant);
    const lowpoly::Input in = load_lowpoly_fixture(variant);
    for (const auto& v : in.views) {
      const Mask2D opaque = opaque_mask(*v.aligned);
      Mask2D cover(opaque.width(), opaque.height());
      int overlaps = 0;
      for (const auto& p : in.parts) {
        auto it = p.masks.find(v.role);
        if (it == p.masks.end()) continue;
        for (int y = 0; y < opaque.height(); ++y)
          for (int x = 0; x < opaque.width(); ++x)
            if (it->second.get(x, y)) {
              overlaps += cover.get(x, y);
              cover.set(x, y, true);
            }
      }
      CHECK(overlaps == 0);
      CHECK(cover == opaque);
    }
  }
}

TEST_CASE("lowpoly fixture: front-only variant gets the requested depth") {
  const lowpoly::Input in = load_lowpoly_fixture("cloaked", false, 20);
  CHECK(in.views.size() == 1);
  CHECK(in.dims.D == 20);
  CHECK(in.settings.user_depth);
}
