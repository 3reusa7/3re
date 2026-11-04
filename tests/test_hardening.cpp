// Hostile inputs and failure reporting: .p3d archives, oversized images, file
// writes that fail, export scale/status rules, and session invariants found by
// the M1 review.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>

#include <miniz.h>

#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/limits.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/json_safe.hpp"
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
  const fs::path dir = fs::temp_directory_path() / "pixel3d_tests" / "hardening";
  fs::create_directories(dir);
  return dir / name;
}

using Entries = std::map<std::string, std::string>;

void write_zip(const fs::path& path, const Entries& entries, int level = MZ_DEFAULT_COMPRESSION) {
  mz_zip_archive zip{};
  REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
  for (const auto& [name, data] : entries)
    REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), static_cast<mz_uint>(level)));
  void* buf = nullptr;
  size_t size = 0;
  REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
  std::ofstream(path, std::ios::binary).write(static_cast<const char*>(buf), static_cast<std::streamsize>(size));
  mz_free(buf);
  mz_zip_writer_end(&zip);
}

Entries read_zip(const fs::path& path) {
  Entries out;
  mz_zip_archive zip{};
  REQUIRE(mz_zip_reader_init_file(&zip, path.string().c_str(), 0));
  for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i) {
    mz_zip_archive_file_stat st;
    REQUIRE(mz_zip_reader_file_stat(&zip, i, &st));
    size_t size = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
    REQUIRE(p);
    out[st.m_filename] = std::string(static_cast<const char*>(p), size);
    mz_free(p);
  }
  mz_zip_reader_end(&zip);
  return out;
}

// A saved, valid project (A, reconstructed) as archive entries.
Entries saved_project(const std::string& file) {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  REQUIRE(s.save(tmp(file)).success);
  return read_zip(tmp(file));
}

OpResult load_entries(const std::string& file, const Entries& e) {
  write_zip(tmp(file), e);
  Session s;
  return s.load(tmp(file));
}

std::string first_error(const OpResult& r) { return r.errors.empty() ? "" : r.errors[0].code; }

bool has_message(const std::vector<Message>& ms, const std::string& code) {
  for (const Message& m : ms)
    if (m.code == code) return true;
  return false;
}

}  // namespace

TEST_CASE("p3d: archives beyond the entry limit are refused") {
  Entries e = saved_project("entries_base.p3d");
  for (uint32_t i = 0; i < limits::kMaxArchiveEntries; ++i) e["junk/" + std::to_string(i)] = "x";
  const OpResult r = load_entries("entries.p3d", e);
  CHECK_FALSE(r.success);
  CHECK(first_error(r) == "project.invalid");
}

TEST_CASE("p3d: an oversized entry is refused before it is extracted") {
  Entries e = saved_project("big_base.p3d");
  std::mt19937 rng(3);
  std::string big(limits::kMaxProjectJsonBytes + 1024, ' ');
  for (char& c : big) c = static_cast<char>('a' + rng() % 26);  // incompressible
  e["project.json"] = "\"" + big + "\"";
  const OpResult r = load_entries("big.p3d", e);
  CHECK_FALSE(r.success);
  CHECK(first_error(r) == "project.too_large");
}

TEST_CASE("p3d: a compression bomb is refused") {
  Entries e = saved_project("bomb_base.p3d");
  e["cache/padding.bin"] = std::string(8u << 20, '\0');
  const OpResult r = load_entries("bomb.p3d", e);
  CHECK_FALSE(r.success);
  CHECK(first_error(r) == "project.invalid");
}

TEST_CASE("p3d: deeply nested JSON is refused without recursion") {
  Entries e = saved_project("deep_base.p3d");
  e["project.json"] = std::string(100000, '[') + std::string(100000, ']');
  const OpResult r = load_entries("deep.p3d", e);
  CHECK_FALSE(r.success);
  CHECK(first_error(r) == "project.invalid");
  CHECK_FALSE(json_depth_within(std::string(65, '{'), 64));
  CHECK(json_depth_within("{\"a\": \"[[[[[[[[[[\"}", 2));
}

TEST_CASE("p3d: wrong JSON types and out-of-range values are clean errors") {
  const Entries base = saved_project("types_base.p3d");
  const nlohmann::json pj = nlohmann::json::parse(base.at("project.json"));
  auto with = [&](const std::function<void(nlohmann::json&)>& change) {
    Entries e = base;
    nlohmann::json j = pj;
    change(j);
    e["project.json"] = j.dump();
    return load_entries("types.p3d", e);
  };
  CHECK(first_error(with([](nlohmann::json& j) { j["format_version"] = "1"; })) == "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) { j["inputs"] = nlohmann::json::array(); })) == "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) { j["inputs"]["settings"]["alpha_threshold"] = 300; })) == "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) { j["inputs"]["settings"]["pixels_per_meter"] = -5; })) == "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) { j["inputs"]["settings"]["name"] = 42; })) == "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) { j["inputs"]["views"][1]["role"] = "front"; })) == "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) { j["inputs"]["texture"]["visibility_samples"] = 1 << 30; })) ==
        "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) { j["inputs"]["views"][0]["sha256"] = "../../etc"; })) == "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) {
          j["inputs"]["texture_assignments"] = {{{"tris", {0}}, {"view", "bottom"}, {"mode", "manual"}}};
        })) == "project.invalid");
  CHECK(first_error(with([](nlohmann::json& j) { j["checkpoints"] = 5; })) == "project.invalid");
  // The untouched project still loads.
  CHECK(with([](nlohmann::json&) {}).success);
}

TEST_CASE("p3d: a tampered mesh cache is never used") {
  Entries e = saved_project("cache_base.p3d");
  std::string& mesh = e["cache/mesh.bin"];
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::memcpy(mesh.data() + 16, &nan, sizeof nan);  // first vertex x
  write_zip(tmp("cache_nan.p3d"), e);
  Session s;
  const OpResult l = s.load(tmp("cache_nan.p3d"));
  REQUIRE(l.success);
  CHECK(l.data["mesh_cache"] == false);
  const OpResult r = s.reconstruct(ReconstructOptions{});
  REQUIRE(r.success);
  CHECK(r.data["cache_hit"] == false);
  CHECK(r.stats.watertight);
  // A cache whose size disagrees with its header is rejected too.
  mesh.resize(mesh.size() - 4);
  CHECK_FALSE(decode_mesh_cache(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(mesh.data()), mesh.size())));
}

TEST_CASE("image: an oversized PNG is refused from its header") {
  ImageRGBA small(4, 4, Rgba8{10, 20, 30, 255});
  std::vector<uint8_t> png = encode_png(small);
  // IHDR width/height are big-endian at byte offsets 16 and 20.
  for (int k : {16, 20}) {
    png[k] = 0x00, png[k + 1] = 0x00, png[k + 2] = 0x27, png[k + 3] = 0x10;  // 10000 px
  }
  const auto img = decode_png(png);
  REQUIRE_FALSE(img);
  CHECK(img.error().code == "image.too_large");
  // And through the API.
  const fs::path p = tmp("huge.png");
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  Session s;
  const OpResult r = s.import_views({{ViewRole::Front, p, {}}});
  CHECK(first_error(r) == "image.too_large");
}

TEST_CASE("file_io: failed writes are reported and leave no temp files") {
  const fs::path blocker = tmp("blocker.txt");
  REQUIRE(write_file_atomic(blocker, std::string_view("x")).code == "ok");
  const Message m = write_file_atomic(blocker / "sub" / "out.bin", std::string_view("data"));
  CHECK(m.code == "io.write_failed");
  CHECK(m.where.contains("path"));
  // Replacing an existing file works and is complete.
  REQUIRE(write_file_atomic(blocker, std::string_view("hello")).code == "ok");
  const auto back = read_file(blocker, 100);
  REQUIRE(back);
  CHECK(std::string(back->begin(), back->end()) == "hello");
  for (const auto& entry : fs::directory_iterator(blocker.parent_path()))
    CHECK(entry.path().filename().string().find(".tmp") == std::string::npos);
  CHECK(read_file(blocker, 2).error().code == "io.too_large");
}

TEST_CASE("session: render artifacts report every file that was not written") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.project_textures(TextureOptions{}).success);
  const fs::path blocker = tmp("artifact_blocker");
  REQUIRE(write_file_atomic(blocker, std::string_view("x")).code == "ok");
  ArtifactRequest bad;
  bad.dir = blocker / "renders";
  const OpResult r = s.write_artifacts(bad);
  CHECK_FALSE(r.success);
  CHECK(first_error(r) == "artifacts.incomplete");
  CHECK(has_message(r.errors, "io.write_failed"));
  CHECK(r.data["written"] == 0);
  ArtifactRequest good;
  good.dir = tmp("renders_ok");
  good.turntable = true;
  const OpResult ok = s.write_artifacts(good);
  REQUIRE(ok.success);
  CHECK(ok.data["written"].get<int>() == 6 + 3 + 25);  // 3 views × (1×, 8×), 3 rotations, 24 frames + sheet
  for (const auto& f : ok.data["files"]) CHECK(fs::exists(path_from_utf8(f["path"].get<std::string>())));
}

TEST_CASE("session: export_glb always uses the project scale") {
  Session s;
  ProjectSettings ps;
  ps.pixels_per_meter = 16;
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  const fs::path out = tmp("scaled.glb");
  OpResult ex = s.export_glb(GlbExportRequest{out});
  REQUIRE(ex.success);
  CHECK(ex.data["pixels_per_meter"] == 16.0);
  CHECK(ex.data["glb"]["bbox_height"].get<double>() == doctest::Approx(33.0 / 16.0));
  CHECK(fs::file_size(out) == ex.data["bytes"].get<size_t>());
  SettingsUpdate u;
  u.pixels_per_meter = 33;
  REQUIRE(s.update_settings(u).success);
  CHECK(s.document().gate == nullptr);  // the scale change invalidated validation
  ex = s.export_glb(GlbExportRequest{out});
  REQUIRE(ex.success);
  CHECK(ex.data["glb"]["bbox_height"].get<double>() == doctest::Approx(1.0));
  u.pixels_per_meter = 0;
  CHECK(first_error(s.update_settings(u)) == "input.invalid");
}

TEST_CASE("session: export never passes NEEDS_REVIEW silently") {
  Session s;
  REQUIRE(s.import_views(fst("C_asym_planter")).success);
  const fs::path out = tmp("c.glb");
  fs::remove(out);
  OpResult ex = s.export_glb(GlbExportRequest{out});
  CHECK_FALSE(ex.success);
  CHECK(first_error(ex) == "export.needs_review");
  CHECK(ex.errors[0].where["blocking_conflicts"].size() >= 2);
  CHECK_FALSE(fs::exists(out));
  GlbExportRequest accept{out};
  accept.allow_needs_review = true;
  ex = s.export_glb(accept);
  REQUIRE(ex.success);
  CHECK(ex.status == ValidationStatus::NeedsReview);
  CHECK(has_message(ex.warnings, "export.status_accepted"));
  CHECK(fs::exists(out));
}

TEST_CASE("session: validation without the GLB round trip is not PASS") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  ValidateOptions vo;
  vo.glb_roundtrip = false;
  const OpResult v = s.validate(vo);
  REQUIRE(v.success);
  CHECK(v.status == ValidationStatus::NeedsReview);
  CHECK(v.validation["failed_checks"].dump().find("glb_roundtrip") != std::string::npos);
}

TEST_CASE("session: new_project starts a fresh session") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.reconstruct(ReconstructOptions{}).success);
  REQUIRE(s.create_checkpoint("old").success);
  REQUIRE(s.new_project(ProjectSettings{}).success);
  CHECK(s.checkpoint_names().empty());
  CHECK_FALSE(s.can_undo());
  CHECK(s.log().size() == 1);
  CHECK(s.document().views.empty());
  ProjectSettings bad;
  bad.pixels_per_meter = -1;
  CHECK(first_error(s.new_project(bad)) == "input.invalid");
}

TEST_CASE("session: a failed reconstruct adds no checkpoint") {
  Session s;
  REQUIRE(s.import_views({{ViewRole::Front, fixture("A_synthetic_chair", "front"), {}}}).success);
  CHECK_FALSE(s.reconstruct(ReconstructOptions{}).success);
  CHECK(s.checkpoint_names().empty());
}

TEST_CASE("session: a refused removal keeps the ambiguity blocking") {
  Session s;
  REQUIRE(s.import_views(fst("C_asym_planter")).success);
  REQUIRE(s.match_parts().success);
  PartEdit remove_all;
  for (const Candidate& c : s.document().candidates->cands)
    if (c.status == CandStatus::Ambiguous) remove_all.decisions.push_back({c.id, false});
  REQUIRE_FALSE(remove_all.decisions.empty());
  const OpResult e = s.edit_part(remove_all);
  REQUIRE(e.success);
  CHECK(has_message(e.warnings, "edit.decision_refused"));
  CHECK_FALSE(e.data["refused_decisions"].empty());
  const OpResult v = s.validate(ValidateOptions{});
  CHECK(v.status == ValidationStatus::NeedsReview);
  // The refused removal is a blocking contradiction, so the legs can never PASS this way.
  bool contradiction = false;
  for (const auto& c : v.data["conflicts"])
    contradiction |= (c["kind"] == "edit.uncovers_atom" || c["kind"] == "ambiguous_correspondence") &&
                     c["blocks_pass"] == true;
  CHECK(contradiction);
}

TEST_CASE("session: texture assignments are bound to their mesh") {
  Session s;
  REQUIRE(s.import_views(fst("A_synthetic_chair")).success);
  REQUIRE(s.project_textures(TextureOptions{}).success);
  CHECK(first_error(s.assign_texture(TextureAssignment{{0}, ViewRole::Bottom, SurfaceMode::Manual, ""})) == "input.invalid");
  CHECK(first_error(s.assign_texture(TextureAssignment{{1 << 20}, ViewRole::Top, SurfaceMode::Manual, ""})) == "input.invalid");
  REQUIRE(s.assign_texture(TextureAssignment{{0, 1}, ViewRole::Top, SurfaceMode::FlatPalette, ""}).success);
  CHECK(s.document().texture->faces[0].mode == SurfaceMode::Manual);
  // A geometry change renumbers triangles: the assignment is reported stale, not misapplied.
  const OpResult d = s.set_depth(DepthConstraint{ViewRole::Top, rect_polys(4, 6, 10, 12), 1, 1e9, "dent"});
  REQUIRE(d.success);
  CHECK(has_message(d.warnings, "texture_assignment.stale"));
  for (const FaceTexture& f : s.document().texture->faces) CHECK(f.mode != SurfaceMode::Manual);
}

TEST_CASE("session: results carry revision, changed ids and validation reasons") {
  Session s;
  const OpResult i = s.import_views(fst("C_asym_planter"));
  REQUIRE(i.success);
  CHECK(i.changed_ids["views"]["added"].size() == 3);
  const OpResult v = s.validate(ValidateOptions{});
  CHECK(v.revision == s.document().revision);
  CHECK(v.validation["status"] == "NEEDS_REVIEW");
  CHECK(v.validation["blocking_conflicts"].size() >= 2);
  const OpResult u = s.undo();
  REQUIRE(u.success);
  CHECK(u.changed_ids["views"]["removed"].size() == 3);
}

TEST_CASE("session: the model size limit is enforced") {
  ImageRGBA wide(limits::kMaxModelSide + 3, 4, Rgba8{200, 50, 50, 255});
  const fs::path f = tmp("wide_front.png"), t = tmp("wide_top.png"), sd = tmp("wide_side.png");
  REQUIRE(save_png(wide, f).code == "ok");
  REQUIRE(save_png(wide, t).code == "ok");
  REQUIRE(save_png(ImageRGBA(4, 4, Rgba8{200, 50, 50, 255}), sd).code == "ok");
  Session s;
  REQUIRE(s.import_views({{ViewRole::Front, f, {}}, {ViewRole::Side, sd, {}}, {ViewRole::Top, t, {}}}).success);
  CHECK(first_error(s.auto_align()) == "view.too_large");
}
