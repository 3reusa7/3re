#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <miniz.h>

#include "pixel3d/common/sha256.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/migrate.hpp"
#include "pixel3d/io/project_file.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

std::string fixture(const std::string& name, const std::string& role) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + role + ".png";
}

std::vector<ViewImport> fst(const std::string& name) {
  return {{ViewRole::Front, fixture(name, "front"), {}},
          {ViewRole::Side, fixture(name, "side"), {}},
          {ViewRole::Top, fixture(name, "top"), {}}};
}

fs::path tmp(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / "pixel3d_tests";
  fs::create_directories(dir);
  return dir / name;
}

std::vector<uint8_t> read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("project: sha256 matches the standard test vectors") {
  CHECK(sha256_hex(std::string_view("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex(std::string_view("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("project: save then load reproduces document inputs") {
  Session s(Source::Gui);
  ProjectSettings ps;
  ps.name = "chair";
  ps.pixels_per_meter = 24;
  ps.profile = ToleranceProfile::Strict;
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views(fst("C_asym_planter")).success);
  REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  REQUIRE(s.create_checkpoint("before-edits").success);
  REQUIRE(s.edit_part(PartEdit{{{s.document().candidates->cands[0].id, true}}}).success);
  REQUIRE(s.set_depth(DepthConstraint{ViewRole::Top, rect_polys(2, 2, 16, 12), 11, 1e9, "cavity"}).success);
  REQUIRE(s.assign_texture(TextureAssignment{{0, 1}, ViewRole::Top, SurfaceMode::FlatPalette, ""}).success);
  const fs::path file = tmp("roundtrip.p3d");
  REQUIRE(s.save(file).success);

  Session t;
  const OpResult lr = t.load(file);
  REQUIRE(lr.success);
  const Document& a = s.document();
  const Document& b = t.document();
  CHECK(b.settings.name == "chair");
  CHECK(b.settings.pixels_per_meter == 24);
  CHECK(b.settings.profile == ToleranceProfile::Strict);
  REQUIRE(b.views.size() == 3);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(b.views[i].role == a.views[i].role);
    CHECK(b.views[i].sha256 == a.views[i].sha256);
    CHECK(*b.views[i].original == *a.views[i].original);
    CHECK(*b.views[i].original_bytes == read_all(a.views[i].original_path));  // byte-identical copy
  }
  REQUIRE(b.decisions.size() == 1);
  CHECK(b.decisions[0].candidate == a.decisions[0].candidate);
  REQUIRE(b.depth_constraints.size() == 1);
  CHECK(b.depth_constraints[0].depth_from == 11);
  CHECK(area(b.depth_constraints[0].region) == doctest::Approx(140));
  REQUIRE(b.texture_assignments.size() == 1);
  CHECK(b.texture_assignments[0].mode == SurfaceMode::FlatPalette);
  bool cp = false;
  for (const auto& n : t.checkpoint_names()) cp |= n == "before-edits";
  CHECK(cp);
  CHECK(t.log().size() == s.log().size() + 1);  // + the load itself
  // The loaded project reconstructs to the same geometry.
  REQUIRE(t.reconstruct(ReconstructOptions{}).success);
  CHECK(t.document().solid->topology.triangle_count == a.solid->topology.triangle_count);
}

TEST_CASE("project: cache hit avoids recompute") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  const fs::path file = tmp("cache.p3d");
  REQUIRE(s.save(file).success);
  Session t;
  REQUIRE(t.load(file).success);
  const OpResult r = t.reconstruct(ReconstructOptions{});
  REQUIRE(r.success);
  CHECK(r.data["cache_hit"] == true);
  CHECK(t.document().solid->topology.triangle_count == s.document().solid->topology.triangle_count);
  // Any input change invalidates the cache.
  REQUIRE(t.set_depth(DepthConstraint{ViewRole::Top, rect_polys(4, 6, 10, 12), 1, 1e9, "dent"}).success);
  const OpResult r2 = t.reconstruct(ReconstructOptions{});
  CHECK(r2.data["cache_hit"] == false);
}

TEST_CASE("project: golden v1 file loads") {
  const fs::path golden = fs::path(PIXEL3D_FIXTURES_DIR) / "golden" / "project_v1.p3d";
  if (std::getenv("PIXEL3D_WRITE_GOLDEN")) {
    Session s;
    REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
    REQUIRE(s.reconstruct(ReconstructOptions{}).success);
    fs::create_directories(golden.parent_path());
    REQUIRE(s.save(golden).success);
  }
  REQUIRE(fs::exists(golden));
  Session t;
  const OpResult r = t.load(golden);
  REQUIRE(r.success);
  CHECK(t.document().views.size() == 3);
  CHECK(t.validate(ValidateOptions{}).status == ValidationStatus::Pass);
}

TEST_CASE("project: future version refused with project.too_new") {
  nlohmann::json j{{"format", "pixel3d.project"}, {"format_version", kProjectFormatVersion + 1}};
  const auto m = migrate_project_json(j, kProjectFormatVersion + 1);
  REQUIRE_FALSE(m.has_value());
  CHECK(m.error().code == "project.too_new");
  // Also through the loader, with a real archive.
  mz_zip_archive zip{};
  REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
  const std::string js = j.dump();
  REQUIRE(mz_zip_writer_add_mem(&zip, "project.json", js.data(), js.size(), MZ_DEFAULT_COMPRESSION));
  void* buf = nullptr;
  size_t size = 0;
  REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
  const fs::path file = tmp("future.p3d");
  std::ofstream(file, std::ios::binary).write(static_cast<const char*>(buf), static_cast<std::streamsize>(size));
  mz_free(buf);
  mz_zip_writer_end(&zip);
  Session t;
  const OpResult r = t.load(file);
  CHECK_FALSE(r.success);
  REQUIRE_FALSE(r.errors.empty());
  CHECK(r.errors[0].code == "project.too_new");
}

TEST_CASE("project: not a project file is a clear error") {
  const fs::path file = tmp("notzip.p3d");
  std::ofstream(file, std::ios::binary) << "hello";
  Session t;
  const OpResult r = t.load(file);
  CHECK_FALSE(r.success);
  CHECK(r.errors[0].code == "project.invalid");
  CHECK(t.load(tmp("does-not-exist.p3d")).errors[0].code == "io.not_found");
}
