// Presentation renderer (style "beauty"): separate from the validation rasterizer.
#include <doctest/doctest.h>

#include <memory>
#include <set>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/render/beauty.hpp"

using namespace pixel3d;

namespace {

// Unit-size cube scaled to `s`, flat normals, one white texel.
TexturedMesh cube(double s) {
  TexturedMesh m;
  const vec3d n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const vec3d& f : n) {
    // Two tangents with cross(u, v) = f, so the quad winds counter-clockwise from outside.
    const vec3d u = std::abs(f.y) > 0.5 ? vec3d{1, 0, 0} : normalize(cross(vec3d{0, 1, 0}, f));
    const vec3d v = cross(f, u);
    const vec3d c = vec3d{0.5, 0.5, 0.5} + f * 0.5;
    const uint32_t base = static_cast<uint32_t>(m.positions.size());
    for (const vec2d& q : {vec2d{-0.5, -0.5}, vec2d{0.5, -0.5}, vec2d{0.5, 0.5}, vec2d{-0.5, 0.5}}) {
      m.positions.push_back((c + u * q.x + v * q.y) * s);
      m.normals.push_back(f);
      m.uvs.push_back({0.5, 0.5});
    }
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    m.tri_part.insert(m.tri_part.end(), {0, 0});
  }
  m.texture = std::make_shared<ImageRGBA>(1, 1, Rgba8{255, 255, 255, 255});
  return m;
}

int luma(Rgba8 c) { return (2 * c.r + 5 * c.g + c.b) / 8; }

std::string fixture(const char* name, const char* view) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + view + ".png";
}

}  // namespace

TEST_CASE("beauty: deterministic, sized, anti-aliased") {
  const TexturedMesh m = cube(10);
  BeautyOptions o;
  o.width = 96, o.height = 80;
  const ImageRGBA a = render_beauty(m, 45, 20, o), b = render_beauty(m, 45, 20, o);
  CHECK(a == b);
  CHECK(a.width() == 96);
  CHECK(a.height() == 80);
  // Supersampling leaves intermediate tones along the silhouette.
  std::set<int> tones;
  for (const Rgba8& p : a.pixels()) tones.insert(luma(p));
  CHECK(tones.size() > 40);
}

TEST_CASE("beauty: the key light comes from the camera's upper left") {
  const TexturedMesh m = cube(10);
  BeautyOptions o;
  o.width = o.height = 128;
  o.shadows = false;
  const ImageRGBA img = render_beauty(m, 45, 20, o);
  // At yaw 45 the +Z face fills the left half of the cube, the +X face the right half.
  const int left = luma(img.at(52, 72)), right = luma(img.at(76, 72));
  CAPTURE(left);
  CAPTURE(right);
  CHECK(left > right + 20);
  // The top face (towards the key) is the brightest of the three.
  CHECK(luma(img.at(64, 40)) > left);
}

TEST_CASE("beauty: shadows only darken, and they reach the ground") {
  const TexturedMesh m = cube(10);
  BeautyOptions on, off;
  on.width = on.height = off.width = off.height = 128;
  off.shadows = false;
  off.ground = false;
  const ImageRGBA a = render_beauty(m, 30, 25, on), b = render_beauty(m, 30, 25, off);
  int darker = 0;
  for (size_t i = 0; i < a.pixels().size(); ++i) {
    CHECK(luma(a.pixels()[i]) <= luma(b.pixels()[i]) + 1);
    darker += luma(a.pixels()[i]) + 8 < luma(b.pixels()[i]);
  }
  CHECK(darker > 30);  // the soft cast and contact shadow around the cube
}

TEST_CASE("beauty: transparent background keeps only the model and its shadow") {
  const TexturedMesh m = cube(10);
  BeautyOptions o;
  o.width = o.height = 64;
  o.transparent = true;
  const ImageRGBA img = render_beauty(m, 30, 20, o);
  CHECK(img.at(0, 0).a == 0);
  CHECK(img.at(63, 0).a == 0);
  CHECK(img.at(32, 32).a == 255);
}

TEST_CASE("beauty: session renders by style; checks never use it") {
  Session s;
  REQUIRE(s.new_project(ProjectSettings{}).success);
  REQUIRE(s.import_views({{ViewRole::Front, fixture("A_synthetic_chair", "front"), {}},
                          {ViewRole::Side, fixture("A_synthetic_chair", "side"), {}},
                          {ViewRole::Top, fixture("A_synthetic_chair", "top"), {}}})
              .success);
  REQUIRE(s.validate(ValidateOptions{}).success);
  const nlohmann::json metrics_before = s.document().metrics ? nlohmann::json(s.document().gate->checks.size()) : nlohmann::json();

  std::vector<NamedImage> imgs;
  OrbitRequest orbit;
  orbit.size = 64;
  orbit.style = RenderStyle::Beauty;
  REQUIRE(s.render_orbit(orbit, &imgs).success);
  REQUIRE(imgs.size() == 1);
  CHECK(imgs[0].name == "orbit_yaw030_el+20_beauty");
  CHECK(imgs[0].image.width() == 64);

  OrbitRequest plain = orbit;
  plain.style = RenderStyle::Validation;
  std::vector<NamedImage> v;
  REQUIRE(s.render_orbit(plain, &v).success);
  CHECK(v[0].name == "orbit_yaw030_el+20");
  CHECK_FALSE(v[0].image == imgs[0].image);

  RenderRequest views;
  views.style = RenderStyle::Beauty;
  views.beauty_size = 32;
  imgs.clear();
  REQUIRE(s.render_views(views, &imgs).success);
  CHECK(imgs.size() == 3);
  CHECK(imgs[0].name == "front_beauty");
  views.compare = true;
  CHECK_FALSE(s.render_views(views, &imgs).success);

  TurntableRequest tt;
  tt.frames = 4;
  tt.size = 32;
  tt.style = RenderStyle::Beauty;
  imgs.clear();
  const OpResult r = s.render_turntable(tt, &imgs);
  REQUIRE(r.success);
  CHECK(r.data["style"] == "beauty");
  CHECK(imgs.size() == 5);  // 4 frames + contact sheet
  CHECK(nlohmann::json(s.document().gate->checks.size()) == metrics_before);
}
