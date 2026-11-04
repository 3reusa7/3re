// Polished styles, step 8: PBR materials per family and the polished GLB.
#include <doctest/doctest.h>

#include <memory>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/style/bake.hpp"
#include "pixel3d/style/materials.hpp"
#include "pixel3d/style/normals.hpp"
#include "pixel3d/style/rounding.hpp"
#include "pixel3d/texture/transfer.hpp"
#include "pixel3d/texture/unique_uv.hpp"

using namespace pixel3d;

namespace {

std::string fixture(const char* name, const char* view) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + view + ".png";
}

struct Styled {
  std::unique_ptr<Session> s;
  TexturedMesh mesh;
};

Styled styled(const char* fx, bool maps) {
  Styled r;
  r.s = std::make_unique<Session>();
  REQUIRE(r.s->new_project(ProjectSettings{}).success);
  REQUIRE(r.s->import_views({{ViewRole::Front, fixture(fx, "front"), {}},
                             {ViewRole::Side, fixture(fx, "side"), {}},
                             {ViewRole::Top, fixture(fx, "top"), {}}})
              .success);
  REQUIRE(r.s->validate(ValidateOptions{}).success);
  const Document& d = r.s->document();
  const SolidResult solid = assemble_parts(round_parts(*d.edited_parts, CornerOptions{}), d.cleanup_options);
  r.mesh = smooth_normals(transfer_textures(solid.mesh, d.solid->mesh, *d.texture, d.align->dims).mesh);
  if (maps) {
    UniqueUVReport u;
    r.mesh = add_unique_uv(r.mesh, 4, 2, &u);
    r.mesh.normal_map = std::make_shared<ImageRGBA>(bake_bevel_normals(r.mesh, d.solid->mesh, u.width, u.height));
    AOOptions ao;
    ao.rays = 8;
    r.mesh.occlusion = std::make_shared<ImageRGBA>(bake_ambient_occlusion(r.mesh, u.width, u.height, ao));
  }
  return r;
}

}  // namespace

TEST_CASE("materials: each triangle takes the family of its texture colour; outline keeps its own") {
  Styled st = styled("D_character", false);
  const Document& d = st.s->document();
  const std::vector<int> fam = triangle_families(st.mesh, *d.families);
  REQUIRE(fam.size() == st.mesh.indices.size() / 3);
  int known = 0;
  for (size_t t = 0; t < fam.size(); ++t) {
    const vec2d uv = (st.mesh.uvs[st.mesh.indices[3 * t]] + st.mesh.uvs[st.mesh.indices[3 * t + 1]] +
                      st.mesh.uvs[st.mesh.indices[3 * t + 2]]) * (1.0 / 3);
    const ImageRGBA& tex = *st.mesh.texture;
    const Rgba8 c = tex.at(std::min(tex.width() - 1, static_cast<int>(uv.x * tex.width())),
                           std::min(tex.height() - 1, static_cast<int>(uv.y * tex.height())));
    CHECK(fam[t] == d.families->family_of(c));
    known += fam[t] >= 0;
  }
  CHECK(known == static_cast<int>(fam.size()));
  // Defaults per style: no metal, no glow; roughness by style.
  const auto mats = default_materials(*d.families, "clay");
  REQUIRE(static_cast<int>(mats.size()) == d.families->family_count);
  for (const StyleMaterial& m : mats) {
    CHECK(m.metallic == 0);
    CHECK(m.emissive == 0);
    CHECK(m.roughness > 0.85);
  }
}

TEST_CASE("polished glb: maps, tangents and per-family materials round-trip exactly") {
  Styled st = styled("D_character", true);
  const Document& d = st.s->document();
  StyleExport se;
  se.materials = default_materials(*d.families, "soft");
  se.tri_material = triangle_families(st.mesh, *d.families);
  se.materials[1].emissive = 2.5;  // the user marked family 1 as glowing
  se.materials[0].metallic = 0.5;
  ExportOptions eo;
  eo.style = &se;
  eo.mipmaps = true;
  const std::vector<uint8_t> glb = write_glb(st.mesh, eo);
  const GlbCheck chk = check_glb_roundtrip(glb, st.mesh, eo, d.align->dims);
  for (const auto& p : chk.problems) MESSAGE(p);
  CHECK(chk.ok);
  const std::string text(glb.begin(), glb.end());
  for (const char* key : {"\"TEXCOORD_1\"", "\"TANGENT\"", "\"normalTexture\"", "\"occlusionTexture\"",
                          "KHR_materials_emissive_strength", "\"emissiveTexture\""})
    CHECK(text.find(key) != std::string::npos);
  // A file whose materials differ from what was asked is refused.
  StyleExport other = se;
  other.materials[0].roughness = 0.1;
  ExportOptions eo2 = eo;
  eo2.style = &other;
  CHECK_FALSE(check_glb_roundtrip(glb, st.mesh, eo2, std::nullopt).ok);
}

TEST_CASE("polished glb: lowpoly without maps, unlit, single material") {
  Styled st = styled("A_synthetic_chair", false);
  const Document& d = st.s->document();
  StyleExport se;
  se.materials = {StyleMaterial{"all", 0.9, 0, 0}};
  se.tri_material.assign(st.mesh.indices.size() / 3, 0);
  ExportOptions eo;
  eo.style = &se;
  eo.unlit = true;
  const std::vector<uint8_t> glb = write_glb(st.mesh, eo);
  const GlbCheck chk = check_glb_roundtrip(glb, st.mesh, eo, d.align->dims);
  for (const auto& p : chk.problems) MESSAGE(p);
  CHECK(chk.ok);
  CHECK(chk.unlit);
  const std::string text(glb.begin(), glb.end());
  CHECK(text.find("\"TEXCOORD_1\"") == std::string::npos);
  CHECK(text.find("normalTexture") == std::string::npos);
}
