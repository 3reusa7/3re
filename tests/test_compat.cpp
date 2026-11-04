// Projects saved by an older release open in this one and give the same model.
//
// tests/fixtures/golden/ holds projects written by Pixel3D 1.0.0 (format 1 and
// format 2 with editor inputs and a single-drawing project) and, in
// compat_expected.json, the SHA-256 of the GLB that 1.0.0 exported from each
// (tools/make_compat_goldens.py). Migrated through every format step, each must
// export byte-identical bytes here; only the generator string names the
// version, so it is pinned to the old one for the comparison.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <span>

#include "pixel3d/common/sha256.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/io/migrate.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

nlohmann::json expected() {
  std::ifstream f(fs::path(PIXEL3D_FIXTURES_DIR) / "golden" / "compat_expected.json");
  REQUIRE(f.good());
  return nlohmann::json::parse(f);
}

}  // namespace

TEST_CASE("compat: 1.0.0 projects (format 1 and 2) export the same GLB as 1.0.0") {
  const nlohmann::json want = expected();
  const std::string generator = want.at("generator").get<std::string>();
  REQUIRE(want.at("projects").size() == 3);
  for (const auto& [file, exp] : want.at("projects").items()) {
    CAPTURE(file);
    Session s;
    const OpResult l = s.load(fs::path(PIXEL3D_FIXTURES_DIR) / "golden" / file);
    REQUIRE(l.success);
    // Every newer setting takes the value that reproduces the old behaviour.
    CHECK(s.document().settings.normalize == NormalizeSettings{});
    CHECK(s.document().settings.style == StyleSettings{});
    REQUIRE(s.validate(ValidateOptions{}).success);
    const Document& d = s.document();
    REQUIRE(d.texture);
    ExportOptions eo;
    eo.pixels_per_meter = d.settings.pixels_per_meter;
    eo.generator = generator;
    const std::vector<uint8_t> glb = write_glb(d.texture->mesh, eo);
    CHECK(sha256_hex(std::span<const uint8_t>(glb)) == exp.at("glb").get<std::string>());
  }
}

TEST_CASE("compat: migration chains every format step up to the current one") {
  const nlohmann::json v1{{"format", "pixel3d.project"},
                          {"format_version", 1},
                          {"inputs", {{"settings", {{"name", "old"}}}, {"views", nlohmann::json::array()}}},
                          {"checkpoints", {{"cp", {{"settings", {{"name", "old"}}}}}}}};
  const auto m = migrate_project_json(v1, 1);
  REQUIRE(m.has_value());
  CHECK((*m)["format_version"] == kProjectFormatVersion);
  CHECK(kProjectFormatVersion == 5);
  for (const nlohmann::json* in : {&(*m)["inputs"], &(*m)["checkpoints"]["cp"]}) {
    CHECK((*in)["settings"]["normalize"]["enabled"] == true);
    CHECK((*in)["settings"]["style"]["preset"] == "crisp");
    CHECK((*in)["lowpoly"]["enabled"] == false);
    CHECK((*in)["part_types"] == nlohmann::json::object());
  }
}
