// M5 export: every format and engine preset is written, reloaded by an
// independent reader and re-rendered; units and front direction change exactly
// the scale and the orientation; the honesty rule holds for every format.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/io/obj_writer.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

std::string fixture(const std::string& name, const std::string& role) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + role + ".png";
}

void open(Session& s, const std::string& name) {
  ProjectSettings ps;
  ps.profile = ToleranceProfile::Strict;
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views({{ViewRole::Front, fixture(name, "front"), {}},
                          {ViewRole::Side, fixture(name, "side"), {}},
                          {ViewRole::Top, fixture(name, "top"), {}}})
              .success);
  REQUIRE(s.project_textures(TextureOptions{}).success);
}

struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() / ("pixel3d_export_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

std::vector<uint8_t> read_bytes(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

std::string read_text(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::vector<vec3d> obj_vertices(const std::string& obj) {
  std::vector<vec3d> v;
  std::istringstream in(obj);
  std::string line;
  while (std::getline(in, line))
    if (line.rfind("v ", 0) == 0) {
      std::istringstream ls(line.substr(2));
      vec3d p;
      ls >> p.x >> p.y >> p.z;
      v.push_back(p);
    }
  return v;
}

}  // namespace

TEST_CASE("export: every format and preset reloads identically from disk") {
  Session s;
  open(s, "A_synthetic_chair");
  const TexturedMesh& mesh = s.document().texture->mesh;
  const Dims dims = s.document().align->dims;
  TempDir tmp;
  for (EnginePreset preset : {EnginePreset::Generic, EnginePreset::Godot, EnginePreset::Unity, EnginePreset::Roblox})
    for (ExportFormat format : {ExportFormat::Glb, ExportFormat::Gltf, ExportFormat::Obj}) {
      CAPTURE(to_string(preset));
      CAPTURE(to_string(format));
      ModelExportRequest r;
      r.path = tmp.path / (std::string(to_string(preset)) + "_model" + std::string(extension(format)));
      r.format = format;
      r.preset = preset;
      const OpResult res = s.export_model(r);
      REQUIRE(res.success);
      CHECK(res.data["check"]["ok"] == true);
      CHECK(res.data["status"] == "PASS");
      const PresetDefaults d = preset_defaults(preset);
      ExportOptions eo;
      eo.pixels_per_meter = s.document().settings.pixels_per_meter;
      eo.units = d.units;
      eo.front_negative_z = d.front_negative_z;
      CHECK(res.data["units"] == to_string(d.units));
      const std::string base = to_utf8(r.path.stem());
      // Read back what is on disk with the independent readers.
      GlbCheck c;
      switch (format) {
        case ExportFormat::Glb:
          CHECK(res.data["files"].size() == 1);
          c = check_glb_roundtrip(read_bytes(r.path), mesh, eo, dims);
          break;
        case ExportFormat::Gltf:
          CHECK(res.data["files"].size() == 3);
          c = check_gltf_roundtrip(read_text(r.path), read_bytes(tmp.path / (base + ".bin")),
                                   read_bytes(tmp.path / (base + ".png")), base, mesh, eo, dims);
          break;
        case ExportFormat::Obj:
          CHECK(res.data["files"].size() == 3);
          c = check_obj_roundtrip(read_text(r.path), read_text(tmp.path / (base + ".mtl")),
                                  read_bytes(tmp.path / (base + ".png")), base, mesh, eo, dims);
          CHECK(res.info.size() >= 1);  // the OBJ sampling note
          break;
      }
      INFO(to_json(c).dump());
      CHECK(c.ok);
      CHECK(c.rerender_diff_px <= 2);
    }
}

TEST_CASE("export: units scale and front direction are exact") {
  Session s;
  open(s, "A_synthetic_chair");
  const Dims d = s.document().align->dims;
  const double ppm = s.document().settings.pixels_per_meter;
  auto size_of = [&](ExportUnits u) {
    ModelExportRequest r;
    r.format = ExportFormat::Obj;
    r.units = u;
    const OpResult res = s.export_model(r);
    REQUIRE(res.success);
    return std::array<double, 3>{res.data["size"][0].get<double>(), res.data["size"][1].get<double>(),
                                 res.data["size"][2].get<double>()};
  };
  const auto m = size_of(ExportUnits::Metres), cm = size_of(ExportUnits::Centimetres), studs = size_of(ExportUnits::Studs),
             px = size_of(ExportUnits::Pixels);
  CHECK(m[1] == doctest::Approx(d.H / ppm));
  for (int k = 0; k < 3; ++k) {
    CHECK(cm[k] == doctest::Approx(m[k] * 100));
    CHECK(studs[k] == doctest::Approx(m[k] / kMetresPerStud));
    CHECK(px[k] == doctest::Approx(m[k] * ppm));
  }
  CHECK(px[0] == doctest::Approx(d.W));
  // Front −Z is a 180° turn about +Y: x and z change sign, y stays, winding is untouched.
  const TexturedMesh& mesh = s.document().texture->mesh;
  ExportOptions plus, minus;
  plus.pixels_per_meter = minus.pixels_per_meter = ppm;
  minus.front_negative_z = true;
  const auto a = obj_vertices(write_obj(mesh, plus, "m").obj), b = obj_vertices(write_obj(mesh, minus, "m").obj);
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    CHECK(b[i].x == doctest::Approx(-a[i].x).epsilon(1e-6));
    CHECK(b[i].y == doctest::Approx(a[i].y).epsilon(1e-6));
    CHECK(b[i].z == doctest::Approx(-a[i].z).epsilon(1e-6));
  }
  // The front of the drawing (largest world z) is at +z with +Z and at −z with −Z.
  double front_z = -1e9;
  size_t front_i = 0;
  for (size_t i = 0; i < mesh.positions.size(); ++i)
    if (mesh.positions[i].z > front_z) front_z = mesh.positions[i].z, front_i = i;
  CHECK(a[front_i].z > 0);
  CHECK(b[front_i].z < 0);
  // Unlit glTF carries KHR_materials_unlit, and the check sees it.
  ModelExportRequest u;
  u.format = ExportFormat::Glb;
  u.unlit = true;
  const OpResult ur = s.export_model(u);
  REQUIRE(ur.success);
  CHECK(ur.data["check"]["unlit"] == true);
}

TEST_CASE("export: NEEDS_REVIEW is refused in every format and nothing is written") {
  Session s;
  open(s, "C_asym_planter");
  TempDir tmp;
  for (ExportFormat f : {ExportFormat::Glb, ExportFormat::Gltf, ExportFormat::Obj}) {
    ModelExportRequest r;
    r.path = tmp.path / ("c" + std::string(extension(f)));
    const OpResult res = s.export_model(r);
    CHECK_FALSE(res.success);
    CHECK(res.errors.front().code == "export.needs_review");
  }
  CHECK(fs::is_empty(tmp.path));
  // Accepted explicitly: written, with the warning.
  ModelExportRequest r;
  r.path = tmp.path / "c.obj";
  r.allow_needs_review = true;
  const OpResult ok = s.export_model(r);
  REQUIRE(ok.success);
  bool warned = false;
  for (const Message& m : ok.warnings) warned |= m.code == "export.status_accepted";
  CHECK(warned);
  CHECK(fs::exists(tmp.path / "c.obj"));
  CHECK(fs::exists(tmp.path / "c.mtl"));
  CHECK(fs::exists(tmp.path / "c.png"));
}

TEST_CASE("export: format from the extension, mismatches refused, all files or none") {
  Session s;
  open(s, "A_synthetic_chair");
  TempDir tmp;
  ModelExportRequest by_ext;
  by_ext.path = tmp.path / "chair.GLTF";
  const OpResult a = s.export_model(by_ext);
  REQUIRE(a.success);
  CHECK(a.data["format"] == "gltf");
  ModelExportRequest roblox;  // no extension given: the preset decides (OBJ)
  roblox.preset = EnginePreset::Roblox;
  const OpResult b = s.export_model(roblox);
  REQUIRE(b.success);
  CHECK(b.data["format"] == "obj");
  CHECK(b.data["front"] == "-z");
  ModelExportRequest mismatch;
  mismatch.path = tmp.path / "x.obj";
  mismatch.format = ExportFormat::Glb;
  const OpResult c = s.export_model(mismatch);
  CHECK_FALSE(c.success);
  CHECK(c.errors.front().code == "input.invalid");
  // The main file cannot be written (a folder is in the way): the companions are removed again.
  fs::create_directories(tmp.path / "blocked.gltf");
  ModelExportRequest blocked;
  blocked.path = tmp.path / "blocked.gltf";
  const OpResult e = s.export_model(blocked);
  CHECK_FALSE(e.success);
  CHECK_FALSE(fs::exists(tmp.path / "blocked.bin"));
  CHECK_FALSE(fs::exists(tmp.path / "blocked.png"));
  // export_glb is the GLB special case of the same command.
  GlbExportRequest g;
  g.path = tmp.path / "compat.glb";
  const OpResult compat = s.export_glb(g);
  REQUIRE(compat.success);
  CHECK(compat.data["glb"]["ok"] == true);
  CHECK(read_bytes(g.path) == write_glb(s.document().texture->mesh, [&] {
          ExportOptions eo;
          eo.pixels_per_meter = s.document().settings.pixels_per_meter;
          return eo;
        }()));
}
