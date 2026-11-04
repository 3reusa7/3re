// Normalisation settings: stored in the project (format 3, with migration),
// changed through the session (undoable, re-aligning), and source images kept
// in the project under their own format's extension.
#include <doctest/doctest.h>

#include <filesystem>

#include <miniz.h>
#include <stb_image_write.h>

#include "pixel3d/engine/inspect.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/migrate.hpp"
#include "test_helpers.hpp"

using namespace pixel3d;
using p3dtest::Box;
namespace fs = std::filesystem;

namespace {

const Rgba8 kWhite{255, 255, 255, 255};

// A chair with 1-px legs and backrest: no coarser grid fits it, so its scale is determined.
const Dims kChair{7, 15, 7};
std::map<ViewRole, ImageRGBA> chair_on_white(int k) {
  const Rgba8 wood{90, 50, 30, 255}, seat{170, 110, 60, 255}, back{60, 90, 160, 255};
  const std::vector<Box> chair{Box{0, 0, 0, 1, 6, 1, wood}, Box{6, 0, 0, 7, 6, 1, wood}, Box{0, 0, 6, 1, 6, 7, wood},
                               Box{6, 0, 6, 7, 6, 7, wood}, Box{0, 6, 0, 7, 8, 7, seat},  Box{0, 8, 6, 7, 15, 7, back}};
  std::map<ViewRole, ImageRGBA> out;
  for (auto& [role, img] : p3dtest::render_views(chair, kChair)) {
    ImageRGBA big = p3dtest::upscale_padded(img, k, 12, 12, 12, 12);
    for (int y = 0; y < big.height(); ++y)
      for (int x = 0; x < big.width(); ++x)
        if (big.at(x, y).a == 0) big.at(x, y) = kWhite;
    out.emplace(role, std::move(big));
  }
  return out;
}

std::vector<uint8_t> jpeg_of(const ImageRGBA& img) {
  std::vector<uint8_t> out;
  auto sink = [](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
  };
  stbi_write_jpg_to_func(sink, &out, img.width(), img.height(), 4, img.pixels().data(), 92);
  return out;
}

// Every message of a result, for failure output.
std::string notes_of(const OpResult& r) {
  std::string out;
  for (const auto* list : {&r.errors, &r.warnings, &r.info})
    for (const Message& m : *list) out += m.code + ": " + m.text + "\n";
  return out;
}

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

std::vector<ViewImport> write_views(const fs::path& dir, const std::map<ViewRole, ImageRGBA>& imgs, bool jpeg) {
  std::vector<ViewImport> out;
  for (const auto& [role, img] : imgs) {
    const fs::path p = dir / (std::string(to_string(role)) + (jpeg ? ".jpg" : ".png"));
    if (jpeg) REQUIRE(write_file_atomic(p, jpeg_of(img)).code == "ok");
    else REQUIRE(save_png(img, p).code == "ok");
    out.push_back({role, p, std::nullopt});
  }
  return out;
}

std::vector<std::string> zip_entries(const fs::path& p) {
  std::vector<std::string> names;
  mz_zip_archive z{};
  REQUIRE(mz_zip_reader_init_file(&z, p.string().c_str(), 0));
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
    char name[512];
    mz_zip_reader_get_filename(&z, i, name, sizeof name);
    names.push_back(name);
  }
  mz_zip_reader_end(&z);
  return names;
}

}  // namespace

TEST_CASE("project v3: normalisation settings round trip; a JPEG source keeps its .jpg name") {
  TempDir tmp("pixel3d_norm_project");
  const auto imports = write_views(tmp.path, chair_on_white(6), true);
  Session s;
  ProjectSettings ps;
  ps.normalize = NormalizeSettings{true, 96, 30, 0.2};
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views(imports).success);
  const OpResult a = s.auto_align();
  INFO(notes_of(a));
  REQUIRE(a.success);
  CHECK(a.data["views"]["front"]["normalization"]["applied"] == true);
  CHECK(a.data["views"]["front"]["normalization"]["background_removed"] == true);
  REQUIRE(s.save(tmp.path / "p.p3d").success);
  int jpgs = 0;
  for (const std::string& n : zip_entries(tmp.path / "p.p3d")) {
    CHECK(n.find(".png") == std::string::npos);  // no source image is stored under a wrong name
    jpgs += n.size() > 4 && n.substr(n.size() - 4) == ".jpg";
  }
  CHECK(jpgs == 3);
  Session t;
  REQUIRE(t.load(tmp.path / "p.p3d").success);
  CHECK(t.document().settings.normalize == ps.normalize);
  REQUIRE(t.auto_align().success);
  for (const SourceView& v : t.document().views) {
    const SourceView* before = nullptr;
    for (const SourceView& u : s.document().views)
      if (u.role == v.role) before = &u;
    REQUIRE(before);
    CHECK(*v.aligned == *before->aligned);
  }
  // inspect shows the settings and what was done to each view.
  const nlohmann::json pj = inspect_project(t);
  CHECK(pj["settings"]["normalize"]["target_size"] == 96);
  const nlohmann::json vj = inspect_views(t.document());
  CHECK(vj["views"][0]["normalization"]["applied"] == true);
}

TEST_CASE("project migration: format 1 and 2 get the default normalisation settings") {
  const nlohmann::json v2{{"format", "pixel3d.project"},
                          {"format_version", 2},
                          {"inputs", {{"settings", {{"name", "old"}}}, {"views", nlohmann::json::array()}}},
                          {"checkpoints", {{"cp", {{"settings", {{"name", "old"}}}}}}}};
  const auto m = migrate_project_json(v2, 2);
  REQUIRE(m.has_value());
  CHECK((*m)["format_version"] == kProjectFormatVersion);
  CHECK(kProjectFormatVersion >= 3);  // later formats add their own settings on top
  const nlohmann::json n = (*m)["inputs"]["settings"]["normalize"];
  CHECK(n["enabled"] == true);
  CHECK(n["target_size"] == 128);
  CHECK(n["bg_tolerance"] == 24);
  CHECK(n["axis_tolerance"] == doctest::Approx(0.15));
  CHECK((*m)["checkpoints"]["cp"]["settings"]["normalize"]["enabled"] == true);
  // The golden format-1 project loads with the defaults and still validates PASS.
  Session g;
  REQUIRE(g.load(fs::path(PIXEL3D_FIXTURES_DIR) / "golden" / "project_v1.p3d").success);
  CHECK(g.document().settings.normalize == NormalizeSettings{});
  CHECK(g.validate(ValidateOptions{}).status == ValidationStatus::Pass);
}

TEST_CASE("session: normalisation settings are checked, undoable, and re-align the views") {
  TempDir tmp("pixel3d_norm_session");
  const auto imports = write_views(tmp.path, chair_on_white(5), false);
  Session s;
  ProjectSettings bad;
  bad.normalize.target_size = 4;
  const OpResult refused = s.new_project(bad);
  REQUIRE_FALSE(refused.success);
  CHECK(refused.errors.front().code == "input.invalid");

  REQUIRE(s.new_project(ProjectSettings{}).success);
  REQUIRE(s.import_views(imports).success);
  const OpResult first = s.auto_align();
  INFO(notes_of(first));
  REQUIRE(first.success);
  const Dims normalised = s.document().align->dims;
  INFO("dims " << normalised.W << "x" << normalised.H << "x" << normalised.D);
  CHECK(normalised == kChair);
  // Switching normalisation off re-aligns: the white background is part of the drawing again.
  SettingsUpdate off;
  off.normalize.enabled = false;
  REQUIRE(s.update_settings(off).success);
  CHECK(s.document().align == nullptr);
  REQUIRE(s.auto_align().success);
  CHECK(s.document().align->dims.W > normalised.W);
  CHECK(s.document().views.front().working == nullptr);
  // Undo brings normalisation back.
  REQUIRE(s.undo().success);
  REQUIRE(s.undo().success);
  CHECK(s.document().settings.normalize.enabled);
  REQUIRE(s.auto_align().success);
  CHECK(s.document().align->dims == normalised);
  // Out-of-range values are refused and change nothing.
  SettingsUpdate wrong;
  wrong.normalize.bg_tolerance = 999;
  const OpResult w = s.update_settings(wrong);
  REQUIRE_FALSE(w.success);
  CHECK(w.errors.front().code == "input.invalid");
  CHECK(s.document().settings.normalize.bg_tolerance == 24);
  // import_views can set them in the same step (one undo step).
  NormalizeUpdate u;
  u.target_size = 64;
  REQUIRE(s.import_views(imports, u).success);
  CHECK(s.document().settings.normalize.target_size == 64);
  REQUIRE(s.undo().success);
  CHECK(s.document().settings.normalize.target_size == 128);
}
