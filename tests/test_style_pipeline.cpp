// Polished styles wired into the project: settings (format 4), the styled model
// built at validation, its cache, export, and every client's entry point.
#include <doctest/doctest.h>

#include <filesystem>
#include <span>

#include "pixel3d/common/sha256.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/io/json_io.hpp"
#include "pixel3d/io/migrate.hpp"
#include "pixel3d/common/file_io.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string& name) : path(fs::temp_directory_path() / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::vector<ViewImport> fixture(const char* name) {
  auto p = [&](const char* v) { return fs::path(PIXEL3D_FIXTURES_DIR) / name / (std::string(v) + ".png"); };
  return {{ViewRole::Front, p("front"), {}}, {ViewRole::Side, p("side"), {}}, {ViewRole::Top, p("top"), {}}};
}

StyleSettings preset(StylePreset p) {
  StyleSettings s;
  s.preset = p;
  return s;
}

SettingsUpdate style_update(StyleSettings s) {
  SettingsUpdate u;
  u.style = std::move(s);
  return u;
}

}  // namespace

TEST_CASE("style settings: presets, ranges and JSON") {
  const StyleParams soft = style_params(preset(StylePreset::Soft));
  CHECK(soft.radius == 1.5);
  CHECK(soft.smooth_angle == 40);
  CHECK(soft.ao);
  const StyleParams low = style_params(preset(StylePreset::Lowpoly));
  CHECK(low.exponent == 1);
  CHECK(low.smooth_angle == 0);
  CHECK_FALSE(low.ao);
  StyleSettings tuned = preset(StylePreset::Clay);
  tuned.radius = 0.5;
  tuned.materials.push_back(FamilyMaterial{1, 0.2, 1.0, std::nullopt});
  CHECK(style_params(tuned).radius == 0.5);
  CHECK(style_params(tuned).shade_radius == 3.0);  // untouched values keep the preset's
  const auto back = style_settings_from_json(to_json(tuned));
  REQUIRE(back.has_value());
  CHECK(*back == tuned);
  CHECK(to_json(StyleSettings{}) == nlohmann::json{{"preset", "crisp"}});
  StyleSettings bad = preset(StylePreset::Soft);
  bad.radius = 20;
  REQUIRE(check_style_settings(bad));
  CHECK(check_style_settings(bad)->code == "input.invalid");
  CHECK_FALSE(style_settings_from_json({{"preset", "glossy"}}).has_value());
}

TEST_CASE("style: crisp is unchanged and builds no polished model") {
  Session s;
  REQUIRE(s.import_views(fixture("D_character")).success);
  REQUIRE(s.validate(ValidateOptions{}).success);
  CHECK(s.document().settings.style.preset == StylePreset::Crisp);
  CHECK(s.document().styled == nullptr);
}

TEST_CASE("style soft: the polished model is validated, keeps every drawn pixel and is exported") {
  TempDir tmp("pixel3d_style_soft");
  Session s;
  ProjectSettings ps;
  ps.style = preset(StylePreset::Soft);
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views(fixture("D_character")).success);
  const OpResult v = s.validate(ValidateOptions{});
  REQUIRE(v.success);
  const Document& d = s.document();
  REQUIRE(d.styled);
  CHECK(v.status == ValidationStatus::Pass);
  // The validated mesh is the polished one, and it is still pixel-exact in every view.
  REQUIRE(d.metrics);
  for (const auto& [role, m] : d.metrics->views) {
    CAPTURE(to_string(role));
    CHECK(m.mismatch_px == 0);
  }
  CHECK(d.styled->solid.topology.watertight);
  CHECK(d.styled->texture.mesh.normal_map);
  CHECK(d.styled->texture.mesh.occlusion);
  CHECK(d.styled->texture.mesh.indices.size() != d.texture->mesh.indices.size());
  bool applied = false;
  for (const Conflict& c : *d.conflicts) applied |= c.kind == "style.applied";
  CHECK(applied);
  // Export writes the polished GLB: maps, UV1, materials; its round trip is checked.
  ModelExportRequest er;
  er.path = tmp.path / "soft.glb";
  const OpResult ex = s.export_model(er);
  REQUIRE(ex.success);
  const auto bytes = read_file(er.path, 1 << 28);
  REQUIRE(bytes);
  const std::string text(bytes->begin(), bytes->end());
  for (const char* key : {"\"TEXCOORD_1\"", "\"normalTexture\"", "\"occlusionTexture\"", "\"minFilter\":9986"})
    CHECK(text.find(key) != std::string::npos);
  CHECK(ex.data["style"]["style"] == "soft");
  // OBJ cannot carry the maps: geometry and colour only, said out loud.
  ModelExportRequest obj;
  obj.path = tmp.path / "soft.obj";
  const OpResult eo = s.export_model(obj);
  REQUIRE(eo.success);
  bool warned = false;
  for (const Message& m : eo.warnings) warned |= m.code == "export.style_maps_dropped";
  CHECK(warned);
}

TEST_CASE("style: changing it is undoable, keeps the crisp model and reuses a model already built") {
  Session s;
  REQUIRE(s.import_views(fixture("A_synthetic_chair")).success);
  REQUIRE(s.validate(ValidateOptions{}).success);
  const auto crisp_solid = s.document().solid;
  const std::string crisp_key = mesh_input_hash(s.document());
  REQUIRE(s.update_settings(style_update(preset(StylePreset::Lowpoly))).success);
  // The crisp model and its cache key do not depend on the style.
  CHECK(s.document().solid == crisp_solid);
  CHECK(mesh_input_hash(s.document()) == crisp_key);
  CHECK(s.document().gate == nullptr);
  REQUIRE(s.validate(ValidateOptions{}).success);
  const auto lowpoly = s.document().styled;
  REQUIRE(lowpoly);
  CHECK(lowpoly->preset == StylePreset::Lowpoly);
  CHECK(lowpoly->texture.mesh.normal_map == nullptr);  // lowpoly: flat, no maps
  // Back to crisp: no polished model; undo returns to lowpoly without rebuilding it.
  REQUIRE(s.update_settings(style_update(StyleSettings{})).success);
  REQUIRE(s.validate(ValidateOptions{}).success);
  CHECK(s.document().styled == nullptr);
  REQUIRE(s.undo().success);  // style back to lowpoly (validation is not an undo step)
  CHECK(s.document().settings.style.preset == StylePreset::Lowpoly);
  REQUIRE(s.validate(ValidateOptions{}).success);
  CHECK(s.document().styled == lowpoly);  // the same model, from the cache
  // Invalid settings are refused and change nothing.
  StyleSettings bad = preset(StylePreset::Soft);
  bad.exponent = 0.5;
  const OpResult r = s.update_settings(style_update(bad));
  CHECK_FALSE(r.success);
  CHECK(s.document().settings.style.preset == StylePreset::Lowpoly);
}

TEST_CASE("style: saved in the project file (format 4) and reloaded") {
  TempDir tmp("pixel3d_style_file");
  Session s;
  StyleSettings st = preset(StylePreset::Clay);
  st.shade_radius = 2.5;
  st.materials.push_back(FamilyMaterial{0, std::nullopt, 0.5, std::nullopt});
  ProjectSettings ps;
  ps.style = st;
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views(fixture("A_synthetic_chair")).success);
  REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  REQUIRE(s.save(tmp.path / "clay.p3d").success);
  Session t;
  REQUIRE(t.load(tmp.path / "clay.p3d").success);
  CHECK(t.document().settings.style == st);
  CHECK(kProjectFormatVersion >= 4);  // style settings since format 4
  // The family material override reaches the export.
  REQUIRE(t.validate(ValidateOptions{}).success);
  REQUIRE(t.document().styled);
  CHECK(t.document().styled->materials.materials.at(0).metallic == 0.5);
}

TEST_CASE("style: single drawing with normalisation (oversize art on white)") {
  TempDir tmp("pixel3d_style_single");
  // A 12x16 figure with pixel-sized detail, drawn 8x larger on an opaque white background.
  const Rgba8 palette[3] = {{200, 60, 40, 255}, {40, 60, 200, 255}, {60, 160, 70, 255}};
  ImageRGBA img(160, 192, Rgba8{255, 255, 255, 255});
  for (int y = 16; y < 144; ++y)
    for (int x = 32; x < 128; ++x) img.at(x, y) = palette[((x - 32) / 8 + 2 * ((y - 16) / 8)) % 3];
  REQUIRE(save_png(img, tmp.path / "front.png").code == "ok");
  Session s;
  ProjectSettings ps;
  ps.style = preset(StylePreset::Soft);
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views({{ViewRole::Front, tmp.path / "front.png", {}}}).success);
  SingleViewSettings sv;
  sv.enabled = true;
  sv.preset = "thick";
  sv.user_depth = true;
  REQUIRE(s.set_single_view(sv).success);
  const OpResult v = s.validate(ValidateOptions{});
  REQUIRE(v.success);
  REQUIRE(s.document().styled);
  CHECK(s.document().align->dims.W == 12);
  CHECK(s.document().align->dims.H == 16);
  CHECK(s.document().metrics->views.at(ViewRole::Front).mismatch_px == 0);
}
