// The "crisp" output is frozen: the GLB bytes and the validation renders of every
// fixture must stay byte-identical to the output recorded before the polished
// styles existed (2026-10-12, main d71aaac). A change here means crisp changed.
#include <doctest/doctest.h>

#include <memory>
#include <span>

#include "pixel3d/common/sha256.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/render/camera.hpp"

using namespace pixel3d;

namespace {

struct Golden {
  const char* fixture;
  ToleranceProfile profile;
  const char* glb;          // write_glb, default options
  const char* glb_roblox;   // studs, front -Z, unlit
  const char* front_1x;     // validation render, Front camera, unlit
  const char* orbit_lit;    // validation render, orbit 30/20, 256 px, lit
};

// The hashes were recorded with Pixel3D 1.0.0, whose GLBs name it as their generator.
constexpr char kGoldenGenerator[] = "Pixel3D 1.0.0";

// clang-format off
const Golden kGolden[] = {
    {"A_synthetic_chair", ToleranceProfile::Strict,
     "0b884414c5eadeffa14441e027a51d0c3d67d8ea26c2a34e4141be2340da4b2e",
     "ee619cb7a1dfb435108ddba092917f63dc190884b924e8fc8674a677a88ca31a",
     "1526eab8c6f9666dc32f9f1464442516bae12de5c20485e6387b373bf5a58ddd",
     "e1bcb4fa31835e64d31def469bc83e6260703db8bc0ce984770bb53a4b8c3083"},
    {"B_hand_chair",      ToleranceProfile::Hand,
     "38771215c08f6866b1ce2c53df5a4852ed339ddfaaa5158ba8dc6b5ab449ec02",
     "4de940ae0f1122e961be1f318f21cfa5f52d2b2ba1e3e12e4518a263ec8c602c",
     "f89df5fac997911fdd09ff753483e33b2abab9f45fcd1c0e26e482cc8fd811fc",
     "41a4f431e02e6267c508e65385f0ca1ad088bacb70bb20b0ce7c7d2fcbe302a2"},
    {"C_asym_planter",    ToleranceProfile::Strict,
     "5aaa12f3ffa4f0a673710d862db5c9d63018315c5c331d8158466adf886f5433",
     "a7d0b2cacee48141b81e99708669b1662623a72d3060765e600f5e0ececa7564",
     "a77441bbe95d17a8e391c04a57423764df1347062de64eeb70d50b740ca4afc8",
     "10260750c0a1dbde1b12475b4c69533f05fda5dd988efc3e3aa53392b1ab9bd4"},
    {"D_character",       ToleranceProfile::Hand,
     "f6c88369262b7c9e4d27034abd1d7a2653c3e6733e787904fee2a4d56c731fbe",
     "049618d08e57f6837e0f3b61944ca3bafd6ac19fa9f78f573cabfeaf47154fa9",
     "9b329f465e07ba0a70e0a857cfbb7fdaec8e515ce34623a65178d7fe81d996ee",
     "5f87e80f9a06cbab9e626aaed885e96f1603e8ee94c25cc76d10c159728832d0"},
};
// clang-format on

std::string png(const std::string& fixture, const char* view) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + fixture + "/" + view + ".png";
}

std::string hash_image(const ImageRGBA& img) {
  std::vector<uint8_t> bytes;
  bytes.reserve(img.pixels().size() * 4 + 8);
  for (int v : {img.width(), img.height()})
    for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<uint8_t>(v >> (8 * k)));
  for (const Rgba8& p : img.pixels()) bytes.insert(bytes.end(), {p.r, p.g, p.b, p.a});
  return sha256_hex(std::span<const uint8_t>(bytes));
}

}  // namespace

TEST_CASE("crisp golden: GLB bytes and validation renders are unchanged") {
  for (const Golden& g : kGolden) {
    CAPTURE(g.fixture);
    Session s;
    ProjectSettings ps;
    ps.profile = g.profile;
    REQUIRE(s.new_project(ps).success);
    REQUIRE(s.import_views({{ViewRole::Front, png(g.fixture, "front"), {}},
                            {ViewRole::Side, png(g.fixture, "side"), {}},
                            {ViewRole::Top, png(g.fixture, "top"), {}}})
                .success);
    REQUIRE(s.validate(ValidateOptions{}).success);
    const Document& d = s.document();
    REQUIRE(d.texture);
    const TexturedMesh& mesh = d.texture->mesh;

    // The generator names the version; it is pinned so that only real changes fail.
    ExportOptions plain;
    plain.generator = kGoldenGenerator;
    const std::vector<uint8_t> glb = write_glb(mesh, plain);
    ExportOptions roblox;
    roblox.generator = kGoldenGenerator;
    roblox.units = ExportUnits::Studs;
    roblox.front_negative_z = true;
    roblox.unlit = true;
    const std::vector<uint8_t> glb_r = write_glb(mesh, roblox);
    RenderOptions lit;
    lit.lit = true;
    const std::string got[4] = {
        sha256_hex(std::span<const uint8_t>(glb)), sha256_hex(std::span<const uint8_t>(glb_r)),
        hash_image(render(mesh, camera_for_view(ViewRole::Front, d.align->dims, 1), RenderOptions{}).color),
        hash_image(render(mesh, camera_orbit(d.align->dims, 30, 20, 256, 256), lit).color)};
    const char* want[4] = {g.glb, g.glb_roblox, g.front_1x, g.orbit_lit};
    for (int k = 0; k < 4; ++k) {
      CAPTURE(k);
      if (got[k] != want[k]) MESSAGE("golden ", std::string(g.fixture), " #", k, " = ", got[k]);
      CHECK(got[k] == want[k]);
    }
  }
}
