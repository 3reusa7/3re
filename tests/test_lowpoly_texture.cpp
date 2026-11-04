#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "lowpoly_helpers.hpp"
#include "pixel3d/lowpoly/build.hpp"
#include "pixel3d/lowpoly/style_render.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;
using namespace p3dtest;

namespace {

Result build(const std::string& variant, int budget, bool side = true, bool opp = false, bool baked = false) {
  Input in = load_lowpoly_fixture(variant, side, side ? 0 : 20);
  in.settings.tri_budget = budget;
  in.settings.opposite_projection_head_cloth = opp;
  in.settings.baked_cel = baked;
  auto r = build_lowpoly(in);
  REQUIRE(r);
  return std::move(*r);
}

uint32_t rgb(Rgba8 c) { return (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b; }

}  // namespace

TEST_CASE("lowpoly texture: islands are disjoint, inside the atlas, with gutters") {
  const Result r = build("cloaked", 1000);
  REQUIRE(r.pack.ok);
  CHECK(r.pack.width <= 1024);
  const auto& isl = r.assembly.islands;
  for (size_t i = 0; i < isl.size(); ++i) {
    CHECK(isl[i].x0 >= r.pack.gutter);
    CHECK(isl[i].y0 >= r.pack.gutter);
    CHECK(isl[i].x0 + isl[i].w + r.pack.gutter <= r.pack.width);
    CHECK(isl[i].y0 + isl[i].h + r.pack.gutter <= r.pack.height);
    for (size_t j = i + 1; j < isl.size(); ++j) {
      const bool apart = isl[i].x0 + isl[i].w + r.pack.gutter <= isl[j].x0 || isl[j].x0 + isl[j].w + r.pack.gutter <= isl[i].x0 ||
                         isl[i].y0 + isl[i].h + r.pack.gutter <= isl[j].y0 || isl[j].y0 + isl[j].h + r.pack.gutter <= isl[i].y0;
      CHECK(apart);
    }
  }
  for (const vec2d& uv : r.mesh().uvs) {
    CHECK(uv.x > 0);
    CHECK(uv.x < 1);
    CHECK(uv.y > 0);
    CHECK(uv.y < 1);
  }
}

TEST_CASE("lowpoly texture: texel density is balanced across parts") {
  const Result r = build("humanoid", 1000);
  const SkinnedMesh& m = r.mesh();
  const double rho2 = r.pack.density * r.pack.density;
  std::vector<double> ratio;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
    const double area3 = length(cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a])) / 2;
    if (area3 < 0.5) continue;
    const vec2d ua = m.uvs[a] * r.pack.width, ub = m.uvs[b] * r.pack.width, uc = m.uvs[c] * r.pack.width;
    ratio.push_back(std::abs(cross(ub - ua, uc - ua)) / 2 / area3 / rho2);
  }
  std::sort(ratio.begin(), ratio.end());
  REQUIRE(ratio.size() > 50);
  CHECK(ratio[ratio.size() / 2] == doctest::Approx(1).epsilon(0.2));
  CHECK(ratio[ratio.size() / 10] > 0.5);
  CHECK(ratio[ratio.size() * 9 / 10] < 2.0);
}

TEST_CASE("lowpoly texture: no new colours, every surface mode accounted for") {
  for (const char* variant : {"cloaked", "humanoid"}) {
    CAPTURE(variant);
    const Input in = load_lowpoly_fixture(variant);
    const Result r = build(variant, 1000);
    std::set<uint32_t> palette;
    for (const auto& v : in.views)
      for (int y = 0; y < v.aligned->height(); ++y)
        for (int x = 0; x < v.aligned->width(); ++x)
          if (v.aligned->at(x, y).a >= 128) palette.insert(rgb(v.aligned->at(x, y)));
    const ImageRGBA& tex = *r.bake.texture;
    int foreign = 0;
    for (int y = 0; y < tex.height(); ++y)
      for (int x = 0; x < tex.width(); ++x) foreign += !palette.contains(rgb(tex.at(x, y)));
    CHECK(foreign == 0);
    double sum = 0;
    for (const auto& [k, v] : r.bake.share) sum += v;
    CHECK(sum == doctest::Approx(1));
    CHECK(r.bake.share.at("seen:front") > 0.3);
    CHECK(r.bake.ink_band_px > 0);
  }
}

TEST_CASE("lowpoly texture: the back of the head is never a projection of the face by default") {
  const Result r = build("cloaked", 1000, /*side=*/false);
  const SkinnedMesh& m = r.mesh();
  const ImageRGBA& tex = *r.bake.texture;
  const int W = tex.width();
  // Opposite texels of the head (part 1000) and cloak (1001): none by default.
  auto opposite_on = [&](const Result& res, int part) {
    int n = 0;
    const SkinnedMesh& mm = res.mesh();
    for (size_t t = 0; t + 2 < mm.indices.size(); t += 3) {
      if (mm.vertex_part[mm.indices[t]] != part) continue;
      const vec2d c = (mm.uvs[mm.indices[t]] + mm.uvs[mm.indices[t + 1]] + mm.uvs[mm.indices[t + 2]]) * (1.0 / 3);
      const int x = std::clamp(static_cast<int>(c.x * W), 0, W - 1), y = std::clamp(static_cast<int>(c.y * W), 0, W - 1);
      n += res.bake.mode[static_cast<size_t>(y) * W + x] == TexelMode::Opposite;
    }
    return n;
  };
  (void)m;
  CHECK(opposite_on(r, 1000) == 0);
  CHECK(opposite_on(r, 1001) == 0);
  CHECK(r.bake.share.at("opposite") > 0);  // the legs and horns may use it
  const Result on = build("cloaked", 1000, false, /*opp=*/true);
  CHECK(opposite_on(on, 1000) > 0);
}

TEST_CASE("lowpoly texture: baked cel darkens away from the standard light, reported") {
  const Result plain = build("cloaked", 800);
  const Result baked = build("cloaked", 800, true, false, true);
  REQUIRE(plain.bake.texture->width() == baked.bake.texture->width());
  int darker = 0, brighter = 0;
  const ImageRGBA &a = *plain.bake.texture, &b = *baked.bake.texture;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) {
      darker += b.at(x, y).r < a.at(x, y).r;
      brighter += b.at(x, y).r > a.at(x, y).r;
    }
  CHECK(darker > 0);
  CHECK(brighter == 0);
  CHECK(std::any_of(baked.messages.begin(), baked.messages.end(), [](const Message& m) { return m.code == "lowpoly.baked_lighting"; }));
  CHECK(cel_factor(1.0, 3) == 1.0);
  CHECK(cel_factor(-1.0, 3) < cel_factor(0.3, 3));
}

TEST_CASE("lowpoly texture: an inverted hull counts toward the budget") {
  Input in = load_lowpoly_fixture("humanoid");
  in.settings.tri_budget = 1200;
  in.settings.hull = true;
  const auto r = build_lowpoly(in);
  REQUIRE(r);
  CHECK(r->hull_triangles > 0);
  CHECK(r->triangles == 2 * r->alloc.total);
  CHECK(r->triangles <= 1200);
}
