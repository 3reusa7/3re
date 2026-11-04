// Polished styles, step 6: atlas padding for mipmaps and the mipmapped sampler.
#include <doctest/doctest.h>

#include <memory>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/texture/filtering.hpp"

using namespace pixel3d;

namespace {

std::string fixture(const char* name, const char* view) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + view + ".png";
}

std::unique_ptr<Session> open(const char* fx) {
  auto s = std::make_unique<Session>();
  REQUIRE(s->new_project(ProjectSettings{}).success);
  REQUIRE(s->import_views({{ViewRole::Front, fixture(fx, "front"), {}},
                           {ViewRole::Side, fixture(fx, "side"), {}},
                           {ViewRole::Top, fixture(fx, "top"), {}}})
              .success);
  REQUIRE(s->validate(ValidateOptions{}).success);
  return s;
}

}  // namespace

TEST_CASE("filtering: padded atlas is opaque everywhere, keeps every colour and every render") {
  for (const char* fx : {"A_synthetic_chair", "D_character"}) {
    const std::string name = fx;
    CAPTURE(name);
    auto s = open(fx);
    const Document& d = s->document();
    const TextureResult p = pad_for_filtering(*d.texture, 8);
    const ImageRGBA& src = *d.texture->mesh.texture;
    const ImageRGBA& img = *p.mesh.texture;
    REQUIRE(img.width() == src.width() * 8);
    REQUIRE(img.height() == src.height() * 8);
    int transparent = 0, changed = 0;
    for (int y = 0; y < img.height(); ++y)
      for (int x = 0; x < img.width(); ++x) {
        transparent += img.at(x, y).a != 255;
        const Rgba8 o = src.at(x / 8, y / 8);
        if (o.a == 255 && !(img.at(x, y) == o)) ++changed;
      }
    CHECK(transparent == 0);
    CHECK(changed == 0);
    CHECK(p.mesh.uvs == d.texture->mesh.uvs);
    CHECK(p.atlas.image == p.mesh.texture);
    for (ViewRole role : kAllViewRoles) {
      const Camera cam = camera_for_view(role, d.align->dims, 4);
      CHECK(render(p.mesh, cam, RenderOptions{}).color == render(d.texture->mesh, cam, RenderOptions{}).color);
    }
  }
}

TEST_CASE("filtering: the mipmapped sampler is written and checked; the default stays NEAREST") {
  auto s = open("A_synthetic_chair");
  const Document& d = s->document();
  const TextureResult p = pad_for_filtering(*d.texture, 8);
  ExportOptions eo;
  eo.mipmaps = true;
  const std::vector<uint8_t> glb = write_glb(p.mesh, eo);
  const GlbCheck chk = check_glb_roundtrip(glb, p.mesh, eo, d.align->dims);
  for (const auto& pr : chk.problems) MESSAGE(pr);
  CHECK(chk.ok);
  const std::string text(glb.begin(), glb.end());
  CHECK(text.find("\"minFilter\":9986") != std::string::npos);
  CHECK(text.find("\"magFilter\":9728") != std::string::npos);
  // A file written without mipmaps but checked as if it had them is refused.
  const GlbCheck wrong = check_glb_roundtrip(write_glb(p.mesh, ExportOptions{}), p.mesh, eo, std::nullopt);
  CHECK_FALSE(wrong.ok);
}
