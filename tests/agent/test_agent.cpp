// Tests of the Game-Ready Asset Agent core (docs/agent/PLAN.md). Written before the code.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include "agent/agent_decide.hpp"
#include "agent/agent_eval.hpp"
#include "agent/agent_exec.hpp"
#include "agent/agent_fidelity.hpp"
#include "agent/agent_glb.hpp"
#include "agent/agent_gltf_decimate.hpp"
#include "agent/agent_plan.hpp"
#include "agent/agent_probe.hpp"
#include "agent/agent_recovery.hpp"
#include "agent/agent_select.hpp"
#include "agent/agent_spec.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/image.hpp"
#include <cmath>

using namespace p3dagent;
using nlohmann::json;

namespace {

fs::path scratch(const std::string& name) {
  const auto dir = fs::temp_directory_path() / "3re-agent-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

json minimal_spec() {
  return json{{"schema", "3re.asset-spec/1"},
              {"name", "crate"},
              {"input", {{"picture", "crate.png"}}},
              {"target", {{"engine", "godot"}}},
              {"asset_type", "prop"},
              {"triangle_budget", 5000},
              {"texture_size", 1024},
              {"scale", {{"axis", "longest"}, {"metres", 0.8}}},
              {"output_dir", "out"}};
}

AssetSpec spec_from(const json& j) {
  auto r = parse_spec(j, fs::path("C:/specs"));
  REQUIRE_MESSAGE(r.spec.has_value(), (r.errors.empty() ? std::string() : r.errors.front()));
  return *r.spec;
}

bool has_error(const SpecResult& r, const std::string& needle) {
  for (const auto& e : r.errors)
    if (e.find(needle) != std::string::npos) return true;
  return false;
}

Capabilities full_caps() {
  Capabilities c;
  c.gpu_present = true;
  c.vram_mib = 4096;
  c.ram_free_mib = 9000;
  c.weights_verified = true;
  for (const char* t : {"engine", "post_safe", "studio", "auto_texture", "inspect", "audit", "decimate", "glbcheck",
                        "validator", "godot"}) {
    ToolInfo info;
    info.present = info.verified = true;
    c.tools[t] = info;
  }
  return c;
}

// A one-mesh GLB: a box of size sx, sy, sz centred at the origin, 12 triangles.
Glb box_glb(double sx, double sy, double sz, bool textured) {
  Glb g;
  g.bin.assign(36 * 4 + 8 * 12, 0);
  json prim = {{"attributes", {{"POSITION", 0}}}, {"indices", 1}, {"mode", 4}};
  if (textured) {
    prim["attributes"]["TEXCOORD_0"] = 2;
    prim["material"] = 0;
  }
  g.json = {{"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", json::array({{{"nodes", {0}}}})},
            {"nodes", json::array({{{"mesh", 0}, {"name", "model"}}})},
            {"meshes", json::array({{{"primitives", json::array({prim})}}})},
            {"buffers", json::array({{{"byteLength", g.bin.size()}}})},
            {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 96}},
                                         {{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 144}}})},
            {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 8}, {"type", "VEC3"},
                                        {"min", {-sx / 2, -sy / 2, -sz / 2}}, {"max", {sx / 2, sy / 2, sz / 2}}},
                                       {{"bufferView", 1}, {"componentType", 5125}, {"count", 36}, {"type", "SCALAR"}}})}};
  if (textured) {
    g.json["accessors"].push_back({{"bufferView", 0}, {"componentType", 5126}, {"count", 8}, {"type", "VEC2"}});
    g.json["materials"] = json::array({{{"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}}}}});
    g.json["textures"] = json::array({{{"source", 0}}});
    g.json["images"] = json::array({{{"bufferView", 0}, {"mimeType", "image/png"}}});
  }
  return g;
}

fs::path child_exe() { return fs::path(AGENT_CHILD_EXE); }

}  // namespace

// ---------------------------------------------------------------- 1. AssetSpec

TEST_CASE("spec: a minimal picture spec parses with defaults and resolved paths") {
  const auto s = spec_from(minimal_spec());
  CHECK(s.name == "crate");
  CHECK(s.kind == InputKind::Picture);
  CHECK(s.picture == fs::path("C:/specs") / "crate.png");
  CHECK(s.output_dir == fs::path("C:/specs") / "out");
  CHECK(s.type == AssetType::Prop);
  CHECK(s.triangle_budget == 5000);
  CHECK(s.texture_size == 1024);
  CHECK(s.scale_axis == ScaleAxis::Longest);
  CHECK(s.scale_metres == doctest::Approx(0.8));
  CHECK(s.seed == 42);
  CHECK(s.acceptance.khronos_max_errors == 0);
  CHECK(s.acceptance.require_godot_import);
}

TEST_CASE("spec: input kind precedence is saved run > glb > picture") {
  auto j = minimal_spec();
  j["input"]["glb"] = "x.glb";
  CHECK(spec_from(j).kind == InputKind::Glb);
  j["input"].erase("glb");
  j["input"]["reuse_run"] = "C:/runs/Trellis-1";
  j["input"]["native_texture"] = "C:/runs/native.post";
  const auto s = spec_from(j);
  CHECK(s.kind == InputKind::ReuseRun);
  CHECK(s.reuse_run == fs::path("C:/runs/Trellis-1"));
  CHECK(s.picture == fs::path("C:/specs") / "crate.png");  // kept as the regeneration source
}

TEST_CASE("spec: every invalid field is reported by name") {
  auto j = minimal_spec();
  j["schema"] = "other/9";
  j["target"]["engine"] = "unreal";
  j["asset_type"] = "vehicle";
  j["triangle_budget"] = 10;
  j["texture_size"] = 3000;
  j["scale"]["metres"] = -1;
  j["input"] = json::object();
  const auto r = parse_spec(j, "C:/specs");
  CHECK_FALSE(r.spec.has_value());
  CHECK(has_error(r, "schema"));
  CHECK(has_error(r, "engine"));
  CHECK(has_error(r, "asset_type"));
  CHECK(has_error(r, "triangle_budget"));
  CHECK(has_error(r, "texture_size"));
  CHECK(has_error(r, "scale.metres"));
  CHECK(has_error(r, "input"));
}

TEST_CASE("spec: glb and reuse_run together are rejected") {
  auto j = minimal_spec();
  j["input"]["glb"] = "x.glb";
  j["input"]["reuse_run"] = "run";
  CHECK(has_error(parse_spec(j, "C:/specs"), "input"));
}

TEST_CASE("spec: limits and acceptance overrides are read") {
  auto j = minimal_spec();
  j["limits"] = {{"total_seconds", 900}, {"step_seconds", 300}, {"step_ram_mib", {{"texture", 64}}}, {"max_regenerations", 1}};
  j["acceptance"] = {{"texture_allowed", {"PASS", "REVIEW"}}, {"require_godot_import", false}};
  const auto s = spec_from(j);
  CHECK(s.limits.total_seconds == 900);
  CHECK(s.limits.step_seconds == 300);
  CHECK(s.limits.step_ram_mib.at("texture") == 64);
  CHECK(s.limits.max_regenerations == 1);
  CHECK(s.acceptance.texture_allowed.size() == 2);
  CHECK_FALSE(s.acceptance.require_godot_import);
  CHECK(to_json(s)["limits"]["step_ram_mib"]["texture"] == 64);
}

TEST_CASE("spec (M2): game defaults — crossings reported, fidelity limits, 4 texture candidates, picture front") {
  const auto s = spec_from(minimal_spec());
  CHECK(s.intersections == Intersections::Report);
  CHECK(s.fidelity.max_distance_p95_pct == doctest::Approx(0.25));
  CHECK(s.fidelity.min_silhouette_iou == doctest::Approx(0.97));
  CHECK(s.texture.candidates == 4);
  CHECK(s.texture.flow_steps == 12);
  CHECK(s.front == Front::Picture);
  CHECK(s.limits.vram_mib == 0);
  CHECK(s.texture_session.empty());
  const auto j = to_json(s);
  CHECK(j["geometry"]["intersections"] == "report");
  CHECK(j["texture"]["candidates"] == 4);
  CHECK(j["fidelity"]["max_distance_max_pct"] == doctest::Approx(1.5));
}

TEST_CASE("spec (M2): overrides are read and bad values named") {
  auto j = minimal_spec();
  j["geometry"] = {{"intersections", "block"}};
  j["fidelity"] = {{"max_distance_p95_pct", 0.4}, {"min_silhouette_iou", 0.95}, {"max_colour_mean_de", 0.08}};
  j["texture"] = {{"candidates", 1}, {"flow_steps", 24}};
  j["front"] = "keep";
  j["limits"] = {{"vram_mib", 3000}};
  j["input"]["texture_session"] = "C:/runs/texture.session";
  const auto s = spec_from(j);
  CHECK(s.intersections == Intersections::Block);
  CHECK(s.fidelity.max_distance_p95_pct == doctest::Approx(0.4));
  CHECK(s.fidelity.min_silhouette_iou == doctest::Approx(0.95));
  CHECK(s.fidelity.max_colour_mean_de == doctest::Approx(0.08));
  CHECK(s.texture.candidates == 1);
  CHECK(s.texture.flow_steps == 24);
  CHECK(s.front == Front::Keep);
  CHECK(s.limits.vram_mib == 3000);
  CHECK(s.texture_session == fs::path("C:/runs/texture.session"));
  auto bad = minimal_spec();
  bad["geometry"] = {{"intersections", "maybe"}};
  bad["texture"] = {{"candidates", 0}, {"flow_steps", 7}};
  bad["fidelity"] = {{"min_silhouette_iou", 2}};
  bad["front"] = "back";
  bad["limits"] = {{"vram_mib", 10}};
  const auto r = parse_spec(bad, "C:/specs");
  CHECK_FALSE(r.spec.has_value());
  CHECK(has_error(r, "geometry.intersections"));
  CHECK(has_error(r, "texture.candidates"));
  CHECK(has_error(r, "texture.flow_steps"));
  CHECK(has_error(r, "fidelity.min_silhouette_iou"));
  CHECK(has_error(r, "front"));
  CHECK(has_error(r, "limits.vram_mib"));
}

TEST_CASE("spec (M2, A5): budget presets fill what the spec leaves out; explicit numbers win") {
  auto j = minimal_spec();
  j.erase("triangle_budget");
  j.erase("texture_size");
  j["preset"] = "mobile_character";
  auto s = spec_from(j);
  CHECK(s.triangle_budget == 5000);
  CHECK(s.texture_size == 1024);
  CHECK(s.preset == "mobile_character");
  j["preset"] = "hero_character";
  CHECK(spec_from(j).triangle_budget == 50000);
  CHECK(spec_from(j).texture_size == 4096);
  j["triangle_budget"] = 12345;
  CHECK(spec_from(j).triangle_budget == 12345);
  j["preset"] = "console_titan";
  CHECK(has_error(parse_spec(j, "C:/specs"), "preset"));
  auto none = minimal_spec();
  none.erase("triangle_budget");
  CHECK(has_error(parse_spec(none, "C:/specs"), "triangle_budget"));  // no preset, no budget: still an error
}

TEST_CASE("spec: load_spec reports unreadable JSON instead of throwing") {
  const auto dir = scratch("spec");
  std::ofstream(dir / "bad.json") << "{ not json";
  const auto r = load_spec(dir / "bad.json");
  CHECK_FALSE(r.spec.has_value());
  CHECK(has_error(r, "JSON"));
  CHECK(has_error(load_spec(dir / "missing.json"), "cannot read"));
}

// ---------------------------------------------------------------- 2-3. Capabilities + planner

TEST_CASE("capabilities: generation needs verified engine, cleanup, studio, weights and a GPU") {
  auto c = full_caps();
  std::string why;
  CHECK(c.can_generate(&why));
  c.weights_verified = false;
  c.weights_note = "q4/shape_dec.gguf: SHA-256 mismatch";
  CHECK_FALSE(c.can_generate(&why));
  CHECK(why.find("weights") != std::string::npos);
  c = full_caps();
  c.tools["engine"].verified = false;
  CHECK_FALSE(c.can_generate(&why));
  CHECK(why.find("engine") != std::string::npos);
  c = full_caps();
  c.gpu_present = false;
  CHECK_FALSE(c.can_generate(&why));
  CHECK(c.to_json()["tools"]["godot"]["verified"] == true);
}

TEST_CASE("plan: picture input generates, then evaluates and delivers") {
  InputFacts f;
  f.picture_exists = f.picture_has_alpha = true;
  const auto p = make_plan(spec_from(minimal_spec()), full_caps(), f);
  CHECK_FALSE(p.blocked);
  CHECK(p.has("generate"));
  CHECK_FALSE(p.has("reuse_run"));
  CHECK(p.has("inspect"));
  CHECK(p.has("deliver"));
  CHECK(p.has("khronos"));
  CHECK(p.has("reload"));
  CHECK(p.has("godot_import"));
  CHECK(p.has("report"));
  CHECK(p.texture_source == "hybrid");
}

TEST_CASE("plan: a picture without transparency is cut out by the wizard's automatic background") {
  InputFacts f;
  f.picture_exists = true;
  f.picture_has_alpha = false;
  const auto p = make_plan(spec_from(minimal_spec()), full_caps(), f);
  CHECK_FALSE(p.blocked);
  bool noted = false;
  for (const auto& s : p.steps)
    if (s.stage == "generate" && s.reason.find("automatic background") != std::string::npos) noted = true;
  CHECK(noted);
}

TEST_CASE("plan: picture input without generation capability is BLOCKED with the reason") {
  auto c = full_caps();
  c.weights_verified = false;
  c.weights_note = "q4/ss_dec.gguf: SHA-256 mismatch";
  InputFacts f;
  f.picture_exists = f.picture_has_alpha = true;
  const auto p = make_plan(spec_from(minimal_spec()), c, f);
  CHECK(p.blocked);
  CHECK(p.blocked_reason.find("ss_dec") != std::string::npos);
}

TEST_CASE("plan: a saved run skips generation; an outdated cleanup is re-run; the run's cutout textures") {
  auto j = minimal_spec();
  j["input"]["reuse_run"] = "C:/runs/r1";
  j["input"]["native_texture"] = "C:/runs/r1/native.post";
  j["input"]["picture"] = "opaque.jpg";
  InputFacts f;
  f.run_exists = f.run_has_geometry = f.run_has_raw_ply = f.run_has_cutout = true;
  f.picture_exists = true;
  f.picture_has_alpha = false;
  f.run_cleanup_sha256 = "a8986a7d";
  auto c = full_caps();
  c.tools["post_safe"].expected_sha256 = "e281d0a4";
  c.tools["post_safe"].sha256 = "e281d0a4";
  const auto p = make_plan(spec_from(j), c, f);
  CHECK_FALSE(p.blocked);
  CHECK_FALSE(p.has("generate"));
  CHECK(p.has("reuse_run"));
  CHECK(p.has("cleanup"));
  CHECK(p.texture_picture == fs::path("C:/runs/r1") / "texture-source.png");
  CHECK(p.texture_source == "hybrid");
  // The same run with the pinned cleanup is not cleaned again.
  f.run_cleanup_sha256 = "e281d0a4";
  CHECK_FALSE(make_plan(spec_from(j), c, f).has("cleanup"));
}

TEST_CASE("plan: a saved run works without a GPU or weights (no generation needed)") {
  auto j = minimal_spec();
  j["input"]["reuse_run"] = "C:/runs/r1";
  auto c = full_caps();
  c.gpu_present = false;
  c.weights_verified = false;
  InputFacts f;
  f.run_exists = f.run_has_geometry = f.run_has_cutout = true;
  const auto p = make_plan(spec_from(j), c, f);
  CHECK_FALSE(p.blocked);
  CHECK(p.texture_source == "picture");  // no native.post given
}

TEST_CASE("plan: a GLB from another generator keeps its own material; no Godot -> godot stage still planned") {
  auto j = minimal_spec();
  j["input"] = {{"glb", "C:/in/robot.glb"}};
  InputFacts f;
  f.glb_exists = f.glb_textured = true;
  auto c = full_caps();
  c.tools.erase("godot");
  const auto p = make_plan(spec_from(j), c, f);
  CHECK_FALSE(p.blocked);
  CHECK(p.has("import_glb"));
  CHECK_FALSE(p.has("generate"));
  CHECK(p.texture_source == "keep");
  CHECK(p.has("godot_import"));  // runs and reports UNVERIFIED
  bool noted = false;
  for (const auto& n : p.notes)
    if (n.find("UNVERIFIED") != std::string::npos) noted = true;
  CHECK(noted);
}

TEST_CASE("plan: a missing input file blocks") {
  auto j = minimal_spec();
  j["input"] = {{"glb", "C:/in/none.glb"}};
  InputFacts f;
  const auto p = make_plan(spec_from(j), full_caps(), f);
  CHECK(p.blocked);
  CHECK(p.blocked_reason.find("none.glb") != std::string::npos);
}

// ---------------------------------------------------------------- 6. Recovery controller

TEST_CASE("recovery: verdict ranks") {
  CHECK(verdict_rank("PASS") == 0);
  CHECK(verdict_rank("REVIEW") == 1);
  CHECK(verdict_rank("FAIL") == 2);
  CHECK(verdict_rank("UNSAFE") == 2);
  CHECK(verdict_rank("") == 3);
}

TEST_CASE("recovery: over budget -> decimate to the budget; texture worse -> gentler once -> REVIEW") {
  const auto s = spec_from(minimal_spec());
  RecoveryController r(s);
  auto d = r.on_over_budget(146782, false);
  CHECK(d.action == Action::Decimate);
  CHECK(d.target_triangles == 5000);
  CHECK(d.rule == "over_budget");
  CHECK_FALSE(d.reason.empty());
  CHECK(r.on_texture_compared("PASS", "PASS", 146782).action == Action::Accept);
  d = r.on_texture_compared("PASS", "REVIEW", 146782);
  CHECK(d.action == Action::DecimateGentler);
  CHECK(d.target_triangles == 7500);
  d = r.on_texture_compared("PASS", "REVIEW", 146782);
  CHECK(d.action == Action::HumanReview);
  CHECK(r.history().size() == 4);
}

TEST_CASE("recovery: decimation above the budget -> one pass that allows touching (never crossings) -> a person") {
  RecoveryController r(spec_from(minimal_spec()));
  const auto d = r.on_over_budget(9000, true);
  CHECK(d.action == Action::DecimateAllowTouching);
  CHECK(d.target_triangles == 5000);
  CHECK(r.on_over_budget(7000, true).action == Action::HumanReview);
}

TEST_CASE("recovery: no gentler step when 1.5x the budget is not below the original or the decimation already stopped above it") {
  RecoveryController r(spec_from(minimal_spec()));
  CHECK(r.on_texture_compared("PASS", "FAIL", 6000).action == Action::HumanReview);
  RecoveryController r2(spec_from(minimal_spec()));
  CHECK(r2.on_texture_compared("PASS", "REVIEW", 146782, 63660).action == Action::HumanReview);
}

TEST_CASE("recovery: out of memory lowers the texture size to 1024, then REVIEW; generation waits once") {
  RecoveryController r(spec_from(minimal_spec()));
  auto d = r.on_out_of_memory("texture", 4096);
  CHECK(d.action == Action::LowerTextureSize);
  CHECK(d.texture_size == 2048);
  d = r.on_out_of_memory("texture", 2048);
  CHECK(d.texture_size == 1024);
  CHECK(r.on_out_of_memory("texture", 1024).action == Action::HumanReview);
  CHECK(r.on_out_of_memory("generate", 0).action == Action::RetryAfterWait);
  CHECK(r.on_out_of_memory("generate", 0).action == Action::Stop);
}

TEST_CASE("recovery: UNSAFE -> pinned cleanup once, then up to two new seeds, then a person") {
  RecoveryController r(spec_from(minimal_spec()));
  CHECK(r.on_unsafe(true, true, true, 42).action == Action::RerunCleanup);
  auto d = r.on_unsafe(true, true, true, 42);
  CHECK(d.action == Action::Regenerate);
  CHECK(d.seed == 43);
  d = r.on_unsafe(false, false, true, 43);
  CHECK(d.seed == 44);
  CHECK(r.on_unsafe(false, false, true, 44).action == Action::HumanReview);
}

TEST_CASE("recovery: UNSAFE without a way to regenerate is kept and reported, never forced") {
  RecoveryController r(spec_from(minimal_spec()));
  const auto d = r.on_unsafe(false, false, false, 42);
  CHECK(d.action == Action::HumanReview);
  CHECK(d.reason.find("kept") != std::string::npos);
}

TEST_CASE("recovery: texture REVIEW -> alternative candidate once -> a person") {
  RecoveryController r(spec_from(minimal_spec()));
  CHECK(r.on_texture_review(true).action == Action::AlternativeTexture);
  CHECK(r.on_texture_review(true).action == Action::HumanReview);
  RecoveryController r2(spec_from(minimal_spec()));
  CHECK(r2.on_texture_review(false).action == Action::HumanReview);
}

TEST_CASE("recovery: the time budget and the decision cap stop everything") {
  auto j = minimal_spec();
  j["limits"] = {{"total_seconds", 100}};
  const auto s = spec_from(j);
  RecoveryController r(s);
  r.set_elapsed(101);
  auto d = r.on_over_budget(100000, false);
  CHECK(d.action == Action::Stop);
  CHECK(d.reason.find("time budget") != std::string::npos);
  RecoveryController capped(spec_from(minimal_spec()), 2);
  capped.on_texture_review(true);
  capped.on_texture_review(true);
  CHECK(capped.on_over_budget(100000, false).action == Action::Stop);
}

// ---------------------------------------------------------------- 5/7. GLB inspection + delivery transform

TEST_CASE("glb: encode/parse round trip keeps JSON and binary bytes") {
  const auto g = box_glb(2, 4, 1, false);
  const auto bytes = encode_glb(g);
  CHECK(bytes.size() % 4 == 0);
  const auto back = parse_glb(bytes);
  CHECK(back.json == g.json);
  CHECK(back.bin == g.bin);
}

TEST_CASE("glb: parse rejects garbage and truncated files with a reason") {
  CHECK_THROWS_WITH_AS(parse_glb(std::vector<uint8_t>{1, 2, 3}), doctest::Contains("GLB"), std::runtime_error);
  auto bytes = encode_glb(box_glb(1, 1, 1, false));
  bytes.resize(30);
  CHECK_THROWS_AS(parse_glb(bytes), std::runtime_error);
}

TEST_CASE("glb: inspection counts triangles, box, uv and materials") {
  const auto i = inspect_glb(box_glb(2, 4, 1, true));
  CHECK(i.triangles == 12);
  CHECK(i.meshes == 1);
  CHECK(i.has_uv);
  CHECK(i.textured);
  CHECK(i.size()[1] == doctest::Approx(4));
  CHECK(i.min[1] == doctest::Approx(-2));
  CHECK_FALSE(inspect_glb(box_glb(1, 1, 1, false)).textured);
}

TEST_CASE("glb: inspection follows node scale and translation") {
  auto g = box_glb(2, 2, 2, false);
  g.json["nodes"][0]["scale"] = {3, 3, 3};
  g.json["nodes"][0]["translation"] = {0, 10, 0};
  const auto i = inspect_glb(g);
  CHECK(i.size()[0] == doctest::Approx(6));
  CHECK(i.min[1] == doctest::Approx(7));
}

TEST_CASE("delivery: height scaling stands the model on y = 0, centred") {
  const auto g = box_glb(0.5, 1.0, 0.3, false);
  const auto t = delivery_transform(inspect_glb(g), ScaleAxis::Height, 1.8);
  CHECK(t.scale == doctest::Approx(1.8));
  const auto out = with_root_transform(g, t, "agent_root");
  const auto i = inspect_glb(out);
  CHECK(i.size()[1] == doctest::Approx(1.8));
  CHECK(i.min[1] == doctest::Approx(0).epsilon(1e-9));
  CHECK((i.min[0] + i.max[0]) == doctest::Approx(0).epsilon(1e-9));
  CHECK(out.bin == g.bin);
  CHECK(out.json["nodes"].size() == 2);
  CHECK(out.json["scenes"][0]["nodes"].size() == 1);
}

TEST_CASE("delivery: longest-axis scaling") {
  const auto t = delivery_transform(inspect_glb(box_glb(0.25, 1.0, 0.07, false)), ScaleAxis::Longest, 1.1);
  CHECK(t.scale == doctest::Approx(1.1));
  const auto t2 = delivery_transform(inspect_glb(box_glb(1.0, 0.62, 0.38, false)), ScaleAxis::Longest, 2.05);
  CHECK(t2.source_extent == doctest::Approx(1.0));
}

TEST_CASE("files: write_new_file never overwrites") {
  const auto dir = scratch("files");
  write_new_file(dir / "a.bin", {1, 2, 3});
  CHECK_THROWS_AS(write_new_file(dir / "a.bin", {4}), std::runtime_error);
  CHECK(fs::file_size(dir / "a.bin") == 3);
}

// ---------------------------------------------------------------- 5. Acceptance

TEST_CASE("acceptance: all measured and inside the spec -> PASS") {
  const auto s = spec_from(minimal_spec());
  const json m = {{"triangles", 4990}, {"geometry_verdict", "REVIEW"}, {"texture_verdict", "PASS"}, {"khronos_errors", 0},
                  {"reload_ok", true}, {"godot_status", "PASS"}, {"godot_triangles", 4990}, {"scale_error_pct", 0.1},
                  {"up_axis_ok", true}, {"deviation_p99_pct", 0.4}};
  const auto c = evaluate_acceptance(s, m);
  CHECK(overall_status(c) == "PASS");
}

TEST_CASE("acceptance: over budget FAILs; texture REVIEW is REVIEW; no Godot is UNVERIFIED, never PASS") {
  const auto s = spec_from(minimal_spec());
  json m = {{"triangles", 4990}, {"geometry_verdict", "PASS"}, {"texture_verdict", "PASS"}, {"khronos_errors", 0},
            {"reload_ok", true}, {"godot_status", "UNVERIFIED"}, {"scale_error_pct", 0.0}, {"up_axis_ok", true}};
  CHECK(overall_status(evaluate_acceptance(s, m)) == "UNVERIFIED");
  m["godot_status"] = "PASS";
  m["godot_triangles"] = 4990;
  m["texture_verdict"] = "REVIEW";
  CHECK(overall_status(evaluate_acceptance(s, m)) == "REVIEW");
  m["triangles"] = 6000;
  CHECK(overall_status(evaluate_acceptance(s, m)) == "FAIL");
}

TEST_CASE("acceptance: missing measurements are UNVERIFIED; Godot triangle mismatch FAILs; UNSAFE FAILs") {
  const auto s = spec_from(minimal_spec());
  const auto c = evaluate_acceptance(s, json::object());
  for (const auto& k : c) CHECK_MESSAGE(k.status == "UNVERIFIED", k.name);
  json m = {{"triangles", 100}, {"geometry_verdict", "UNSAFE"}, {"texture_verdict", "PASS"}, {"khronos_errors", 0},
            {"reload_ok", true}, {"godot_status", "PASS"}, {"godot_triangles", 99}, {"scale_error_pct", 0.0},
            {"up_axis_ok", true}};
  int fails = 0;
  for (const auto& k : evaluate_acceptance(s, m)) fails += k.status == "FAIL";
  CHECK(fails == 2);
}

TEST_CASE("run log: one JSON object per line with a timestamp") {
  const auto dir = scratch("log");
  {
    RunLog log(dir / "run.jsonl");
    log.append({{"tool", "a"}, {"exit_code", 0}});
    log.append({{"tool", "b"}});
  }
  std::ifstream in(dir / "run.jsonl");
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    const auto j = json::parse(line);
    CHECK(j.contains("timestamp"));
    ++n;
  }
  CHECK(n == 2);
}

TEST_CASE("report: markdown names the result, decisions and human-review items") {
  const json rep = {{"name", "crate"}, {"status", "REVIEW"},
                    {"decisions", json::array({{{"rule", "over_budget"}, {"action", "Decimate"}, {"reason", "146782 > 5000"}}})},
                    {"human_review", json::array({"texture REVIEW after decimation"})},
                    {"deliverables", json::array({{{"path", "C:/o/crate.glb"}, {"sha256", "ab12"}}})}};
  const auto md = render_report_markdown(rep);
  CHECK(md.find("REVIEW") != std::string::npos);
  CHECK(md.find("over_budget") != std::string::npos);
  CHECK(md.find("texture REVIEW after decimation") != std::string::npos);
  CHECK(md.find("ab12") != std::string::npos);
}

// ---------------------------------------------------------------- 4. Executor (real child processes)

TEST_CASE("exec: quoting follows the Windows argv rules") {
  CHECK(quote_windows_arg(L"plain") == L"plain");
  CHECK(quote_windows_arg(L"two words") == L"\"two words\"");
  CHECK(quote_windows_arg(L"") == L"\"\"");
  CHECK(quote_windows_arg(L"a\"b") == L"\"a\\\"b\"");
  CHECK(quote_windows_arg(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
}

TEST_CASE("exec: arguments with spaces, quotes and Arabic text reach the child unchanged") {
  const auto dir = scratch("exec-echo");
  ProcessSpec p;
  p.exe = child_exe();
  p.args = {"echo", "two words", "q\"uote", "\xD8\xBA\xD9\x88\xD9\x83\xD9\x88", "trail\\"};
  p.stdout_file = dir / "out.txt";
  const auto r = run_process(p);
  CHECK(r.started);
  CHECK(r.exit_code == 0);
  std::ifstream in(p.stdout_file, std::ios::binary);
  const std::string out((std::istreambuf_iterator<char>(in)), {});
  CHECK(out.find("[two words]") != std::string::npos);
  CHECK(out.find("[q\"uote]") != std::string::npos);
  CHECK(out.find("[trail\\]") != std::string::npos);
}

TEST_CASE("exec: exit code, time limit and missing executable") {
  ProcessSpec p;
  p.exe = child_exe();
  p.args = {"exit", "7"};
  CHECK(run_process(p).exit_code == 7);
  p.args = {"sleep", "20000"};
  p.timeout_seconds = 1;
  const auto t = run_process(p);
  CHECK(t.timed_out);
  CHECK(t.seconds < 10);
  p.exe = "C:/no/such/tool.exe";
  const auto m = run_process(p);
  CHECK_FALSE(m.started);
  CHECK_FALSE(m.error.empty());
}

TEST_CASE("exec: a job memory limit is a real out-of-memory, detected as such") {
  ProcessSpec p;
  p.exe = child_exe();
  p.args = {"alloc", "400"};
  p.memory_limit_mib = 96;
  const auto r = run_process(p);
  CHECK(r.started);
  CHECK(r.exit_code != 0);
  CHECK(r.memory_limit_hit);
  p.memory_limit_mib = 0;
  p.args = {"alloc", "64"};
  const auto ok = run_process(p);
  CHECK(ok.exit_code == 0);
  CHECK_FALSE(ok.memory_limit_hit);
  CHECK(ok.peak_memory_mib >= 64);
}

TEST_CASE("sha256: streaming hash of a file") {
  const auto dir = scratch("sha");
  std::ofstream(dir / "abc.txt", std::ios::binary) << "abc";
  CHECK(file_sha256(dir / "abc.txt") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(file_sha256(dir / "missing").empty());
}

TEST_CASE("probe: the machine reports RAM and disk") {
  const auto s = probe_system(fs::temp_directory_path());
  CHECK(s.ram_total_mib > 1000);
  CHECK(s.disk_free_mib > 0);
}

// ---------------------------------------------------------------- 2. Tool registry from a release folder

TEST_CASE("probe: tool registry verifies pins and the weights manifest; a tampered hash is unavailable") {
  const auto dir = scratch("probe");
  fs::create_directories(dir / "app" / "runtime");
  fs::create_directories(dir / "models" / "trellis" / "q4");
  std::ofstream(dir / "app" / "runtime" / "trellis-cli.exe", std::ios::binary) << "engine";
  std::ofstream(dir / "app" / "runtime" / "pixel3d-trellis-post-safe.exe", std::ios::binary) << "post";
  std::ofstream(dir / "models" / "trellis" / "q4" / "ss_dec.gguf", std::ios::binary) << "abc";
  const auto engine_sha = file_sha256(dir / "app" / "runtime" / "trellis-cli.exe");
  json manifest = {{"files", json::array({{{"path", "q4/ss_dec.gguf"}, {"size", 3},
                                           {"lfs", {{"oid", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}, {"size", 3}}}}})}};
  std::ofstream(dir / "models" / "trellis" / "models-manifest.json") << manifest.dump();
  const json config = {{"format", "3re-config"},
                       {"trellis", {{"runtime_dir", "runtime"}, {"models_dir", "../models/trellis/q4"},
                                    {"post_executable", "runtime/pixel3d-trellis-post-safe.exe"},
                                    {"weights_manifest", "../models/trellis/models-manifest.json"},
                                    {"engine_sha256", engine_sha}, {"post_sha256", std::string(64, '0')}}}};
  std::ofstream(dir / "app" / "3re.config.json") << config.dump();
  ProbeInputs in;
  in.config = dir / "app" / "3re.config.json";
  in.app_dir = dir / "app";
  in.workers_dir = dir / "app";
  in.disk_path = dir;
  const auto caps = probe_capabilities(in);
  CHECK(caps.tools.at("engine").verified);
  CHECK_FALSE(caps.tools.at("post_safe").verified);  // pin does not match
  CHECK(caps.weights_verified);
  CHECK_FALSE(caps.has("godot"));
  // Tamper: the manifest now pins another hash.
  manifest["files"][0]["lfs"]["oid"] = std::string(64, 'f');
  std::ofstream(dir / "models" / "trellis" / "models-manifest.json", std::ios::trunc) << manifest.dump();
  const auto caps2 = probe_capabilities(in);
  CHECK_FALSE(caps2.weights_verified);
  CHECK(caps2.weights_note.find("ss_dec.gguf") != std::string::npos);
}

// ---------------------------------------------------------------- Worker: pixel3d-decimate-guarded

namespace {

// UV sphere (closed, welded) as a geometry GLB: 2 * seg * (rings - 1) triangles.
Glb sphere_glb(int seg, int rings) {
  std::vector<float> v;
  std::vector<uint32_t> f;
  v.insert(v.end(), {0, 1, 0});
  for (int r = 1; r < rings; ++r)
    for (int s = 0; s < seg; ++s) {
      const double th = 3.14159265358979 * r / rings, ph = 2 * 3.14159265358979 * s / seg;
      v.insert(v.end(), {float(std::sin(th) * std::cos(ph)), float(std::cos(th)), float(std::sin(th) * std::sin(ph))});
    }
  v.insert(v.end(), {0, -1, 0});
  const uint32_t south = uint32_t(v.size() / 3 - 1);
  auto ring = [&](int r, int s) { return uint32_t(1 + (r - 1) * seg + (s % seg)); };
  for (int s = 0; s < seg; ++s) f.insert(f.end(), {0, ring(1, s + 1), ring(1, s)});
  for (int r = 1; r < rings - 1; ++r)
    for (int s = 0; s < seg; ++s) {
      f.insert(f.end(), {ring(r, s), ring(r, s + 1), ring(r + 1, s + 1)});
      f.insert(f.end(), {ring(r, s), ring(r + 1, s + 1), ring(r + 1, s)});
    }
  for (int s = 0; s < seg; ++s) f.insert(f.end(), {south, ring(rings - 1, s), ring(rings - 1, s + 1)});
  Glb g;
  const size_t vb = v.size() * 4, ib = f.size() * 4;
  g.bin.resize(vb + ib);
  std::memcpy(g.bin.data(), v.data(), vb);
  std::memcpy(g.bin.data() + vb, f.data(), ib);
  g.json = {{"asset", {{"version", "2.0"}}}, {"scene", 0}, {"scenes", json::array({{{"nodes", {0}}}})},
            {"nodes", json::array({{{"mesh", 0}}})},
            {"meshes", json::array({json{{"primitives", json::array({json{{"attributes", json{{"POSITION", 0}}}, {"indices", 1}}})}}})},
            {"buffers", json::array({{{"byteLength", g.bin.size()}}})},
            {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", vb}, {"target", 34962}},
                                         {{"buffer", 0}, {"byteOffset", vb}, {"byteLength", ib}, {"target", 34963}}})},
            {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", v.size() / 3}, {"type", "VEC3"},
                                        {"min", {-1, -1, -1}}, {"max", {1, 1, 1}}},
                                       {{"bufferView", 1}, {"componentType", 5125}, {"count", f.size()}, {"type", "SCALAR"}}})}};
  return g;
}

}  // namespace

TEST_CASE("decimate worker: reduces to the target, reports deviation, keeps the shape, never overwrites") {
  const auto dir = scratch("decimate");
  write_new_file(dir / "sphere.glb", encode_glb(sphere_glb(96, 48)));  // 9024 triangles
  ProcessSpec p;
  p.exe = fs::path(DECIMATE_EXE);
  p.args = {pixel3d::to_utf8(dir / "sphere.glb"), pixel3d::to_utf8(dir / "out.glb"), "--target", "1500", "--max-threshold", "1e12"};
  p.stderr_file = dir / "err.txt";
  p.stdout_file = dir / "out.txt";
  const auto r = run_process(p);
  REQUIRE(r.exit_code == 0);
  const auto info = inspect_glb(read_glb(dir / "out.glb"));
  CHECK(info.triangles <= 1500);
  CHECK(info.triangles >= 1000);
  CHECK(info.size()[1] == doctest::Approx(2).epsilon(0.05));
  const auto rep = json::parse(std::ifstream(dir / "out.glb.decimate.json"));
  CHECK(rep["input_faces"] == 9024);
  CHECK(rep["output_faces"] == info.triangles);
  CHECK(rep["budget_met"] == true);
  CHECK(rep["contact_guard"] == true);
  CHECK(rep["deviation"]["p99_pct"].get<double>() < 1.0);
  CHECK(rep["deviation"]["max_pct"].get<double>() >= rep["deviation"]["p99_pct"].get<double>());
  // Second run onto the same output is refused and leaves the file alone.
  const auto before = fs::file_size(dir / "out.glb");
  CHECK(run_process(p).exit_code != 0);
  CHECK(fs::file_size(dir / "out.glb") == before);
}

TEST_CASE("decimate worker: the default threshold cap keeps faces instead of erasing detail") {
  const auto dir = scratch("decimate-cap");
  write_new_file(dir / "sphere.glb", encode_glb(sphere_glb(96, 48)));
  ProcessSpec p;
  p.exe = fs::path(DECIMATE_EXE);
  p.args = {pixel3d::to_utf8(dir / "sphere.glb"), pixel3d::to_utf8(dir / "out.glb"), "--target", "200"};
  REQUIRE(run_process(p).exit_code == 0);
  const auto rep = json::parse(std::ifstream(dir / "out.glb.decimate.json"));
  CHECK(rep["max_threshold"].get<double>() == doctest::Approx(1e-5));
  CHECK(rep["budget_met"] == false);
  CHECK(rep["output_faces"].get<int>() > 200);
}

// ---------------------------------------------------------------- 3re-agent.exe end to end (real workers, no GPU)

namespace {

struct AgentRun {
  int exit_code = -1;
  json report;
  std::vector<json> log;
  fs::path out;
};

AgentRun run_agent(const fs::path& dir, const json& spec, std::vector<std::string> extra = {}) {
  AgentRun a;
  a.out = dir / "out";
  std::ofstream(dir / "spec.json") << spec.dump(1);
  ProcessSpec p;
  p.exe = fs::path(AGENT_EXE);
  p.args = {pixel3d::to_utf8(dir / "spec.json"), "--workers", WORKERS_DIR, "--workers", TOOLS_DIR, "--no-hash-weights"};
  p.args.insert(p.args.end(), extra.begin(), extra.end());
  p.stdout_file = dir / "agent.out.txt";
  p.stderr_file = dir / "agent.err.txt";
  p.timeout_seconds = 600;
  const auto r = run_process(p);
  a.exit_code = r.exit_code;
  if (fs::exists(a.out / "report.json")) a.report = json::parse(std::ifstream(a.out / "report.json"));
  std::ifstream log(a.out / "run.jsonl");
  for (std::string line; std::getline(log, line);) a.log.push_back(json::parse(line));
  return a;
}

json glb_spec(const fs::path& glb, int budget) {
  return {{"schema", "3re.asset-spec/1"}, {"name", "ball"}, {"input", {{"glb", pixel3d::to_utf8(glb)}}},
          {"target", {{"engine", "godot"}}}, {"asset_type", "prop"}, {"triangle_budget", budget}, {"texture_size", 1024},
          {"scale", {{"axis", "longest"}, {"metres", 0.5}}}, {"output_dir", "out"}, {"limits", {{"total_seconds", 600}}}};
}

}  // namespace

TEST_CASE("agent: GLB inside the budget is delivered at the spec's scale; no Godot -> UNVERIFIED, never PASS") {
  const auto dir = scratch("agent-glb");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(32, 16)));  // 960 triangles, 2 units
  const auto a = run_agent(dir, glb_spec(dir / "ball.glb", 5000));
  CHECK(a.exit_code == 1);
  REQUIRE(a.report.is_object());
  CHECK(a.report["status"] == "UNVERIFIED");
  const auto glb = a.out / "ball.glb";
  REQUIRE(fs::exists(glb));
  const auto info = inspect_glb(read_glb(glb));
  CHECK(info.triangles == 960);
  CHECK(std::max({info.size()[0], info.size()[1], info.size()[2]}) == doctest::Approx(0.5));
  CHECK(info.min[1] == doctest::Approx(0).epsilon(1e-6));
  CHECK(a.report["deliverables"][0]["sha256"] == file_sha256(glb));
  bool godot_unverified = false;
  for (const auto& c : a.report["acceptance"])
    if (c["criterion"] == "godot_import") godot_unverified = c["status"] == "UNVERIFIED";
  CHECK(godot_unverified);
  // Every step is in run.jsonl with tool, exit code, duration and peak memory.
  bool inspected = false;
  for (const auto& e : a.log)
    if (e.value("step", "") == "inspect") {
      inspected = true;
      CHECK(e.contains("exit_code"));
      CHECK(e.contains("seconds"));
      CHECK(e.contains("peak_memory_mib"));
      CHECK(e.contains("args"));
    }
  CHECK(inspected);
  CHECK(fs::exists(a.out / "report.md"));
}

TEST_CASE("agent: an over-budget mesh is decimated by the guarded worker and the decision is logged") {
  const auto dir = scratch("agent-budget");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(96, 48)));  // 9024 triangles
  const auto a = run_agent(dir, glb_spec(dir / "ball.glb", 2000));
  REQUIRE(a.report.is_object());
  const auto info = inspect_glb(read_glb(a.out / "ball.glb"));
  CHECK(info.triangles <= 2000);
  bool decided = false;
  for (const auto& d : a.report["decisions"])
    if (d["rule"] == "over_budget" && d["action"] == "Decimate") decided = true;
  CHECK(decided);
  CHECK(a.report["before_after"]["triangles"]["before"] == 9024);
  bool budget_pass = false;
  for (const auto& c : a.report["acceptance"])
    if (c["criterion"] == "triangle_budget") budget_pass = c["status"] == "PASS";
  CHECK(budget_pass);
  // Milestone 2 (game target, the default): decimation candidates judged on budget + fidelity, all logged.
  REQUIRE(a.report["decimation_candidates"].is_array());
  REQUIRE_FALSE(a.report["decimation_candidates"].empty());
  CHECK(a.report["decimation_candidates"][0]["name"] == "guarded QEM");
  CHECK(a.report["decimation_candidates"][0]["chosen"] == true);  // a sphere: the guarded one already meets both
  CHECK(a.report["fidelity"]["geometry"]["judgement"]["status"] == "PASS");
  bool fid = false;
  for (const auto& c : a.report["acceptance"])
    if (c["criterion"] == "fidelity") fid = c["status"] == "PASS";
  CHECK(fid);
  CHECK(a.report["delivery"]["front"]["rotation_y_deg"] == 0);  // a GLB without a picture: axes kept, said so
}

TEST_CASE("agent: an existing output folder is refused (never overwrites)") {
  const auto dir = scratch("agent-refuse");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(16, 8)));
  fs::create_directories(dir / "out");
  std::ofstream(dir / "out" / "keep.txt") << "user file";
  const auto a = run_agent(dir, glb_spec(dir / "ball.glb", 5000));
  CHECK(a.exit_code == 4);
  CHECK(fs::exists(dir / "out" / "keep.txt"));
  CHECK_FALSE(fs::exists(dir / "out" / "report.json"));
}

TEST_CASE("agent: a missing Godot given on the command line is reported UNVERIFIED (fault injection)") {
  const auto dir = scratch("agent-nogodot");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(16, 8)));
  const auto a = run_agent(dir, glb_spec(dir / "ball.glb", 5000), {"--godot", "C:/no/Godot_v4.7-stable_win64_console.exe"});
  REQUIRE(a.report.is_object());
  CHECK(a.report["status"] != "PASS");
  CHECK(a.report["capabilities"]["tools"]["godot"]["present"] == false);
}

namespace {

// Two closed cubes touching along one edge, with separate vertices (as TRELLIS sheets that touch).
Glb touching_cubes_glb() {
  std::vector<float> v;
  std::vector<uint32_t> f;
  auto cube = [&](float x0, float y0) {
    const uint32_t b = uint32_t(v.size() / 3);
    for (int i = 0; i < 8; ++i) v.insert(v.end(), {x0 + float(i & 1), y0 + float((i >> 1) & 1), float((i >> 2) & 1)});
    const uint32_t q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& s : q) f.insert(f.end(), {b + s[0], b + s[1], b + s[2], b + s[0], b + s[2], b + s[3]});
  };
  cube(0, 0);
  cube(1, 1);
  Glb g;
  const size_t vb = v.size() * 4, ib = f.size() * 4;
  g.bin.resize(vb + ib);
  std::memcpy(g.bin.data(), v.data(), vb);
  std::memcpy(g.bin.data() + vb, f.data(), ib);
  g.json = {{"asset", {{"version", "2.0"}}}, {"scene", 0}, {"scenes", json::array({json{{"nodes", {0}}}})},
            {"nodes", json::array({json{{"mesh", 0}}})},
            {"meshes", json::array({json{{"primitives", json::array({json{{"attributes", json{{"POSITION", 0}}}, {"indices", 1}}})}}})},
            {"buffers", json::array({json{{"byteLength", g.bin.size()}}})},
            {"bufferViews", json::array({json{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", vb}},
                                         json{{"buffer", 0}, {"byteOffset", vb}, {"byteLength", ib}}})},
            {"accessors", json::array({json{{"bufferView", 0}, {"componentType", 5126}, {"count", v.size() / 3}, {"type", "VEC3"},
                                            {"min", {0, 0, 0}}, {"max", {2, 2, 1}}},
                                       json{{"bufferView", 1}, {"componentType", 5125}, {"count", f.size()}, {"type", "SCALAR"}}})}};
  return g;
}

}  // namespace

TEST_CASE("decimate worker: touching but separate sheets are never welded (no non-manifold edges)") {
  const auto dir = scratch("decimate-touch");
  write_new_file(dir / "cubes.glb", encode_glb(touching_cubes_glb()));
  ProcessSpec p;
  p.exe = fs::path(DECIMATE_EXE);
  p.args = {pixel3d::to_utf8(dir / "cubes.glb"), pixel3d::to_utf8(dir / "out.glb"), "--target", "16", "--max-threshold", "1e12"};
  REQUIRE(run_process(p).exit_code == 0);
  // Inspect the output topology with the shipped inspector.
  ProcessSpec q;
  q.exe = fs::path(WORKERS_DIR) / "pixel3d-asset-inspect.exe";
  q.args = {pixel3d::to_utf8(dir / "out.glb"), pixel3d::to_utf8(dir / "inspect")};
  REQUIRE(run_process(q).exit_code == 0);
  const auto t = json::parse(std::ifstream(dir / "inspect" / "topology.json"));
  CHECK(t["overfull_edges"] == 0);
  CHECK(t["invalid_vertex_links"] == 0);
  CHECK(t["components"] == 2);
}

TEST_CASE("decimate worker: --no-guard (game targets) reaches the budget the guard refuses and says so") {
  const auto dir = scratch("decimate-noguard");
  write_new_file(dir / "cubes.glb", encode_glb(touching_cubes_glb()));
  ProcessSpec p;
  p.exe = fs::path(DECIMATE_EXE);
  p.args = {pixel3d::to_utf8(dir / "cubes.glb"), pixel3d::to_utf8(dir / "out.glb"), "--target", "16", "--max-threshold", "1e12",
            "--no-guard"};
  REQUIRE(run_process(p).exit_code == 0);
  const auto rep = json::parse(std::ifstream(dir / "out.glb.decimate.json"));
  CHECK(rep["contact_guard"] == false);
  CHECK(rep["allow_touching"] == false);
  CHECK(rep["output_faces"].get<int>() <= 24);
  // Never welded, even without the guard.
  ProcessSpec q;
  q.exe = fs::path(WORKERS_DIR) / "pixel3d-asset-inspect.exe";
  q.args = {pixel3d::to_utf8(dir / "out.glb"), pixel3d::to_utf8(dir / "inspect")};
  REQUIRE(run_process(q).exit_code == 0);
  const auto t = json::parse(std::ifstream(dir / "inspect" / "topology.json"));
  CHECK(t["overfull_edges"] == 0);
  // --no-guard and --allow-touching contradict each other.
  ProcessSpec bad = p;
  bad.args = {pixel3d::to_utf8(dir / "cubes.glb"), pixel3d::to_utf8(dir / "out2.glb"), "--target", "16", "--no-guard", "--allow-touching"};
  CHECK(run_process(bad).exit_code != 0);
}

TEST_CASE("decimate worker: --allow-touching is reported and still never makes the mesh UNSAFE") {
  const auto dir = scratch("decimate-touching");
  write_new_file(dir / "cubes.glb", encode_glb(touching_cubes_glb()));
  ProcessSpec p;
  p.exe = fs::path(DECIMATE_EXE);
  p.args = {pixel3d::to_utf8(dir / "cubes.glb"), pixel3d::to_utf8(dir / "out.glb"), "--target", "16", "--max-threshold", "1e12",
            "--allow-touching"};
  REQUIRE(run_process(p).exit_code == 0);
  const auto rep = json::parse(std::ifstream(dir / "out.glb.decimate.json"));
  CHECK(rep["allow_touching"] == true);
  CHECK(rep["contact_guard"] == true);
  ProcessSpec q;
  q.exe = fs::path(WORKERS_DIR) / "pixel3d-asset-inspect.exe";
  q.args = {pixel3d::to_utf8(dir / "out.glb"), pixel3d::to_utf8(dir / "inspect")};
  REQUIRE(run_process(q).exit_code == 0);
  ProcessSpec a;
  a.exe = fs::path(WORKERS_DIR) / "pixel3d-geometry-audit.exe";
  a.args = {pixel3d::to_utf8(dir / "inspect" / "geometry.bin"), pixel3d::to_utf8(dir / "inspect" / "x.json")};
  REQUIRE(run_process(a).exit_code == 0);
  const auto x = json::parse(std::ifstream(dir / "inspect" / "x.json"));
  const auto pc = x.value("pair_classification", json::object());
  CHECK(pc.value("transverse_crossing", 0) == 0);
  CHECK(pc.value("edge_face_contact", 0) == 0);
  CHECK(pc.value("coplanar_area_overlap", 0) == 0);
}

TEST_CASE("spec: hostile names and relative paths are handled safely") {
  for (const char* bad : {"CON", "-rf", "..", "a\x01" "b", "name.", "lpt1"}) {
    auto j = minimal_spec();
    j["name"] = bad;
    CHECK_MESSAGE(has_error(parse_spec(j, "C:/specs"), "name"), bad);
  }
  auto j = minimal_spec();
  j["input"]["picture"] = "-x.png";
  const auto s = spec_from(j);
  CHECK(s.picture.is_absolute());
  CHECK(pixel3d::to_utf8(s.picture).front() != '-');
}

TEST_CASE("agent: Godot measures the delivered size and grounding (runs when Godot 4 is installed)") {
  const fs::path godot = GODOT_EXE;
  if (!fs::exists(godot)) {
    MESSAGE("Godot not installed at " << pixel3d::to_utf8(godot) << ": this check did not run");
    return;
  }
  const auto dir = scratch("agent-godot");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(32, 16)));
  const auto a = run_agent(dir, glb_spec(dir / "ball.glb", 5000), {"--godot", pixel3d::to_utf8(godot)});
  REQUIRE(a.report.is_object());
  const auto& m = a.report["measured"];
  CHECK(m["godot_status"] == "PASS");
  CHECK(m["godot_triangles"] == 960);
  CHECK(m["scale_error_pct"].get<double>() < 0.5);
  CHECK(m["up_axis_ok"] == true);
}

TEST_CASE("spec: fields of the wrong type are spec errors, never a crash") {
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{{"texture_size", "big"}, {"name", 5}, {"limits", {{"total_seconds", "x"}}}}) {
    auto j = minimal_spec();
    j[key] = value;
    const auto r = parse_spec(j, "C:/specs");
    CHECK_FALSE(r.spec.has_value());
    CHECK_FALSE(r.errors.empty());
  }
  auto j = minimal_spec();
  j["texture_size"] = 512;
  CHECK(has_error(parse_spec(j, "C:/specs"), "texture_size"));
}

// ---------------------------------------------------------------- Baked delivery (3re importer / Smart Retopology compatible)

namespace {

std::vector<float> positions_of(const Glb& g) {
  const auto& acc = g.json["accessors"][0];
  const auto& view = g.json["bufferViews"][acc["bufferView"].get<int>()];
  std::vector<float> v(acc["count"].get<size_t>() * 3);
  std::memcpy(v.data(), g.bin.data() + view.value("byteOffset", 0), v.size() * 4);
  return v;
}

}  // namespace

TEST_CASE("delivery: a single untransformed mesh node gets the transform baked into its vertices") {
  const auto g = sphere_glb(16, 8);  // radius 1, centred
  const auto t = delivery_transform(inspect_glb(g), ScaleAxis::Height, 1.8);
  const auto baked = with_baked_transform(g, t);
  REQUIRE(baked.has_value());
  CHECK(baked->json["nodes"].size() == 1);
  CHECK_FALSE(baked->json["nodes"][0].contains("scale"));
  CHECK_FALSE(baked->json["nodes"][0].contains("translation"));
  const auto i = inspect_glb(*baked);
  CHECK(i.size()[1] == doctest::Approx(1.8));
  CHECK(i.min[1] == doctest::Approx(0).epsilon(1e-6));
  // Accessor bounds and the vertices themselves agree.
  const auto v = positions_of(*baked);
  float lo = 1e30f, hi = -1e30f;
  for (size_t k = 1; k < v.size(); k += 3) lo = std::min(lo, v[k]), hi = std::max(hi, v[k]);
  CHECK(lo == doctest::Approx(0).epsilon(1e-5));
  CHECK(hi == doctest::Approx(1.8).epsilon(1e-5));
  CHECK(baked->json["accessors"][0]["max"][1].get<double>() == doctest::Approx(hi));
  // Indices untouched.
  CHECK(std::equal(g.bin.begin() + g.json["bufferViews"][1]["byteOffset"].get<size_t>(), g.bin.end(),
                   baked->bin.begin() + g.json["bufferViews"][1]["byteOffset"].get<size_t>()));
}

TEST_CASE("delivery: files with their own node transforms or instancing are not baked (root node is used)") {
  auto moved = sphere_glb(8, 4);
  moved.json["nodes"][0]["translation"] = {0, 3, 0};
  CHECK_FALSE(with_baked_transform(moved, delivery_transform(inspect_glb(moved), ScaleAxis::Height, 1)).has_value());
  auto instanced = sphere_glb(8, 4);
  instanced.json["nodes"].push_back({{"mesh", 0}});
  instanced.json["scenes"][0]["nodes"].push_back(1);
  CHECK_FALSE(with_baked_transform(instanced, delivery_transform(inspect_glb(instanced), ScaleAxis::Height, 1)).has_value());
}

TEST_CASE("agent: the delivered GLB opens in 3re's own importer (Smart Retopology input)") {
  const auto dir = scratch("agent-importer");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(32, 16)));
  const auto a = run_agent(dir, glb_spec(dir / "ball.glb", 5000));
  REQUIRE(fs::exists(a.out / "ball.glb"));
  ProcessSpec q;  // pixel3d-asset-inspect = p3dasset::load_geometry_glb, the importer retopology uses
  q.exe = fs::path(WORKERS_DIR) / "pixel3d-asset-inspect.exe";
  q.args = {pixel3d::to_utf8(a.out / "ball.glb"), pixel3d::to_utf8(dir / "inspect")};
  q.stderr_file = dir / "inspect.err.txt";
  CHECK(run_process(q).exit_code == 0);
  const auto info = inspect_glb(read_glb(a.out / "ball.glb"));
  CHECK(info.nodes == 1);
  CHECK(std::max({info.size()[0], info.size()[1], info.size()[2]}) == doctest::Approx(0.5));
  CHECK(a.report["delivery"]["method"] == "baked");
}

TEST_CASE("delivery: the logical-topology accessor of a 3re textured GLB is baked with the rendered positions") {
  auto g = sphere_glb(16, 8);
  // As 3re's texture worker writes it: a second copy of the positions + indices named in asset.extras.
  const size_t vb = g.json["bufferViews"][0]["byteLength"].get<size_t>(), ib = g.json["bufferViews"][1]["byteLength"].get<size_t>();
  const size_t base = g.bin.size();
  g.bin.insert(g.bin.end(), g.bin.begin(), g.bin.begin() + vb + ib);
  g.json["buffers"][0]["byteLength"] = g.bin.size();
  g.json["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", base}, {"byteLength", vb}});
  g.json["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", base + vb}, {"byteLength", ib}});
  json pos = g.json["accessors"][0], idx = g.json["accessors"][1];
  pos["bufferView"] = 2;
  idx["bufferView"] = 3;
  g.json["accessors"].push_back(pos);
  g.json["accessors"].push_back(idx);
  g.json["asset"]["extras"] = {{"pixel3d_topology", {{"position_accessor", 2}, {"index_accessor", 3}}}};
  const auto baked = with_baked_transform(g, delivery_transform(inspect_glb(g), ScaleAxis::Height, 1.8));
  REQUIRE(baked.has_value());
  CHECK(std::equal(baked->bin.begin(), baked->bin.begin() + long(vb), baked->bin.begin() + long(base)));  // same floats
  CHECK(baked->json["accessors"][2]["max"] == baked->json["accessors"][0]["max"]);
  // And the real importer accepts it.
  const auto dir = scratch("baked-logical");
  write_new_file(dir / "t.glb", encode_glb(*baked));
  ProcessSpec q;
  q.exe = fs::path(WORKERS_DIR) / "pixel3d-asset-inspect.exe";
  q.args = {pixel3d::to_utf8(dir / "t.glb"), pixel3d::to_utf8(dir / "inspect")};
  q.stderr_file = dir / "err.txt";
  CHECK(run_process(q).exit_code == 0);
}

// ---------------------------------------------------------------- Game fidelity (milestone 2, A1)

namespace {
FMesh sphere_fmesh(int seg, int rings) {
  return fmesh_from_glb(parse_glb(encode_glb(sphere_glb(seg, rings))));
}
FMesh shifted(FMesh m, std::array<float, 3> d, float scale = 1) {
  for (auto& p : m.pos)
    for (int k = 0; k < 3; ++k) p[size_t(k)] = p[size_t(k)] * scale + d[size_t(k)];
  return m;
}
}  // namespace

TEST_CASE("fidelity: a mesh against itself is identical; topology of a closed sphere is clean") {
  const auto a = sphere_fmesh(48, 24);
  const auto f = measure_fidelity(a, a);
  CHECK(f.distance_max_pct < 1e-4);
  CHECK(f.silhouette_iou_min == doctest::Approx(1.0));
  CHECK(f.distance_max_pct < 1e-4);
  CHECK(f.silhouette_iou.size() == 9);
  CHECK(f.normal_mean_deg < 1e-3);
  CHECK(f.colour_mean_de < 0);  // untextured: colour not measured
  CHECK(f.original.boundary_edges == 0);
  CHECK(f.original.inconsistent_winding_edges == 0);
  CHECK(f.original.degenerate_triangles == 0);
  CHECK(judge_fidelity(f, {})["status"] == "PASS");
}

TEST_CASE("fidelity: a coarser sphere is close; a shifted or shrunk one is not") {
  const auto fine = sphere_fmesh(96, 48), coarse = sphere_fmesh(24, 12);
  const auto close_by = measure_fidelity(fine, coarse);
  CHECK(close_by.distance_p95_pct < 0.5);
  CHECK(close_by.silhouette_iou_min > 0.97);
  const auto moved = measure_fidelity(fine, shifted(fine, {0.1f, 0, 0}));
  CHECK(moved.distance_p95_pct > 1.0);
  CHECK(moved.silhouette_iou_min < 0.97);
  CHECK(judge_fidelity(moved, {})["status"] == "FAIL");
  const auto shrunk = measure_fidelity(fine, shifted(fine, {0, 0, 0}, 0.9f));
  CHECK(shrunk.silhouette_iou_min < 0.9);
}

TEST_CASE("fidelity: holes, flipped faces and degenerate triangles in the reduced mesh are named") {
  const auto a = sphere_fmesh(48, 24);
  auto holed = a;
  holed.tri.erase(holed.tri.begin() + 500, holed.tri.begin() + 504);
  holed.tri_image.resize(holed.tri.size());
  auto flipped = a;
  std::swap(flipped.tri[700][1], flipped.tri[700][2]);
  auto degen = a;
  degen.tri.push_back({degen.tri[0][0], degen.tri[0][0], degen.tri[0][1]});
  degen.tri_image.push_back(-1);
  const auto th = topology(holed), tf = topology(flipped), td = topology(degen);
  CHECK(th.boundary_edges > 0);
  CHECK(th.boundary_loops >= 1);
  CHECK(tf.inconsistent_winding_edges == 3);
  CHECK(td.degenerate_triangles == 1);
  const auto j = judge_fidelity(measure_fidelity(a, holed), {});
  CHECK(j["status"] == "FAIL");
  CHECK(j["reasons"].dump().find("holes") != std::string::npos);
}

TEST_CASE("fidelity: base colour is compared when both meshes are textured") {
  // A quad with UVs and a 2x1 texture (left red, right blue); the reduced copy has the halves swapped.
  auto quad = [](bool swap) {
    std::vector<float> v{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    std::vector<float> uv{0, 1, 1, 1, 1, 0, 0, 0};
    std::vector<uint32_t> idx{0, 1, 2, 0, 2, 3};
    const auto png = pixel3d::encode_png([&] {
      pixel3d::ImageRGBA im(2, 1);
      im.at(0, 0) = swap ? pixel3d::Rgba8{30, 60, 220, 255} : pixel3d::Rgba8{220, 30, 30, 255};
      im.at(1, 0) = swap ? pixel3d::Rgba8{220, 30, 30, 255} : pixel3d::Rgba8{30, 60, 220, 255};
      return im;
    }());
    Glb g;
    auto put = [&](const void* d, size_t n) {
      const size_t off = g.bin.size();
      g.bin.resize(off + ((n + 3) & ~size_t(3)));
      std::memcpy(g.bin.data() + off, d, n);
      return off;
    };
    const size_t ov = put(v.data(), v.size() * 4), ou = put(uv.data(), uv.size() * 4), oi = put(idx.data(), idx.size() * 4),
                 op = put(png.data(), png.size());
    g.json = {{"asset", {{"version", "2.0"}}}, {"scene", 0}, {"scenes", json::array({{{"nodes", {0}}}})}, {"nodes", json::array({{{"mesh", 0}}})},
              {"meshes", json::array({{{"primitives", json::array({{{"attributes", {{"POSITION", 0}, {"TEXCOORD_0", 1}}}, {"indices", 2}, {"material", 0}}})}}})},
              {"materials", json::array({{{"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}}}}})},
              {"textures", json::array({{{"source", 0}}})},
              {"images", json::array({{{"bufferView", 3}, {"mimeType", "image/png"}}})},
              {"buffers", json::array({{{"byteLength", g.bin.size()}}})},
              {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", ov}, {"byteLength", v.size() * 4}},
                                           {{"buffer", 0}, {"byteOffset", ou}, {"byteLength", uv.size() * 4}},
                                           {{"buffer", 0}, {"byteOffset", oi}, {"byteLength", idx.size() * 4}},
                                           {{"buffer", 0}, {"byteOffset", op}, {"byteLength", png.size()}}})},
              {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 4}, {"type", "VEC3"}, {"min", {-1, -1, 0}}, {"max", {1, 1, 0}}},
                                         {{"bufferView", 1}, {"componentType", 5126}, {"count", 4}, {"type", "VEC2"}},
                                         {{"bufferView", 2}, {"componentType", 5125}, {"count", 6}, {"type", "SCALAR"}}})}};
    return fmesh_from_glb(parse_glb(encode_glb(g)));
  };
  const auto a = quad(false), b = quad(true);
  REQUIRE(a.textured());
  const auto same = measure_fidelity(a, a);
  CHECK(same.colour_mean_de == doctest::Approx(0).epsilon(1e-6));
  const auto swapped = measure_fidelity(a, b);
  CHECK(swapped.colour_mean_de > 0.2);
  CHECK(swapped.colour_block_mean_de > 0.15);  // a whole region in another colour shows at block scale too
  CHECK(same.colour_block_mean_de == doctest::Approx(0).epsilon(1e-6));
  CHECK(judge_fidelity(swapped, {})["status"] == "FAIL");
  // Fine stripes (about 2.5 px per period on screen) moved by half a period: pixels differ, 8x8 block means do not.
  auto stripes = [&](int phase) {
    FMesh m = a;
    m.images[0].w = 256;
    m.images[0].h = 1;
    m.images[0].rgba.assign(256 * 4, 255);
    for (int x = 0; x < 256; ++x) {
      const bool dark = ((x + phase) / 2) % 2 == 0;
      for (int k = 0; k < 3; ++k) m.images[0].rgba[size_t(x) * 4 + size_t(k)] = dark ? 20 : 235;
    }
    return m;
  };
  const auto shiftf = measure_fidelity(stripes(0), stripes(2));
  CHECK(shiftf.colour_mean_de > 0.3);
  CHECK(shiftf.colour_block_mean_de < 0.25 * shiftf.colour_mean_de);
  const auto px = render_unlit(a, 0, 0, 32);
  CHECK(px.size() == 32 * 32 * 4);
  CHECK(px[(16 * 32 + 16) * 4 + 3] == 255);
}

// ---------------------------------------------------------------- Front direction (milestone 2, A4)

TEST_CASE("front: the registered picture direction is turned onto +Z (glTF / engine front)") {
  for (double yaw : {0.0, 11.25, 90.0, 114.0, 180.0, 304.5}) {
    const double r = yaw * 3.14159265358979 / 180;
    const auto d = rotate_y({std::sin(r), 0, std::cos(r)}, front_rotation_deg(yaw));
    CHECK(d[0] == doctest::Approx(0).epsilon(1e-9));
    CHECK(d[2] == doctest::Approx(1).epsilon(1e-9));
  }
}

TEST_CASE("delivery: a rotation is baked into positions and normals, then scaled and grounded exactly") {
  // A 2 x 1 x 0.5 box along X, normals on; rotated 90 degrees about Y it lies along Z.
  std::vector<float> v, n;
  std::vector<uint32_t> f;
  const float hx = 1, hy = 0.5f, hz = 0.25f;
  const float c[8][3] = {{-hx, -hy, -hz}, {hx, -hy, -hz}, {hx, hy, -hz}, {-hx, hy, -hz}, {-hx, -hy, hz}, {hx, -hy, hz}, {hx, hy, hz}, {-hx, hy, hz}};
  for (auto& p : c) {
    v.insert(v.end(), {p[0], p[1] + 3, p[2]});  // lifted: the delivery grounds it
    n.insert(n.end(), {1, 0, 0});               // every normal +X (only the rotation matters here)
  }
  for (uint32_t t : {0u, 2u, 1u, 0u, 3u, 2u, 4u, 5u, 6u, 4u, 6u, 7u}) f.push_back(t);
  Glb g;
  const size_t vb = v.size() * 4, nb = n.size() * 4, ib = f.size() * 4;
  g.bin.resize(vb + nb + ib);
  std::memcpy(g.bin.data(), v.data(), vb);
  std::memcpy(g.bin.data() + vb, n.data(), nb);
  std::memcpy(g.bin.data() + vb + nb, f.data(), ib);
  g.json = {{"asset", {{"version", "2.0"}}}, {"scene", 0}, {"scenes", json::array({{{"nodes", {0}}}})}, {"nodes", json::array({{{"mesh", 0}}})},
            {"meshes", json::array({{{"primitives", json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}}, {"indices", 2}}})}}})},
            {"buffers", json::array({{{"byteLength", g.bin.size()}}})},
            {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", vb}}, {{"buffer", 0}, {"byteOffset", vb}, {"byteLength", nb}},
                                         {{"buffer", 0}, {"byteOffset", vb + nb}, {"byteLength", ib}}})},
            {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 8}, {"type", "VEC3"}, {"min", {-1, 2.5, -0.25}}, {"max", {1, 3.5, 0.25}}},
                                       {{"bufferView", 1}, {"componentType", 5126}, {"count", 8}, {"type", "VEC3"}},
                                       {{"bufferView", 2}, {"componentType", 5125}, {"count", f.size()}, {"type", "SCALAR"}}})}};
  const auto t = delivery_transform(g, ScaleAxis::Longest, 4.0, 90.0);
  CHECK(t.rotation_y_deg == 90.0);
  CHECK(t.scale == doctest::Approx(2.0));
  const auto baked = with_baked_transform(g, t);
  REQUIRE(baked);
  const auto info = inspect_glb(*baked);
  CHECK(info.size()[2] == doctest::Approx(4.0).epsilon(1e-5));  // the long side now runs along Z
  CHECK(info.size()[0] == doctest::Approx(1.0).epsilon(1e-5));
  CHECK(info.min[1] == doctest::Approx(0).epsilon(1e-6));       // standing on y = 0
  float nrm[3];
  std::memcpy(nrm, baked->bin.data() + vb, 12);
  CHECK(nrm[0] == doctest::Approx(0).epsilon(1e-6));             // +X normal turned to -Z by R_y(90)
  CHECK(nrm[2] == doctest::Approx(-1).epsilon(1e-6));
  // The root-node path carries the same rotation as a quaternion.
  const auto rooted = with_root_transform(g, t, "root");
  const auto& rn = rooted.json["nodes"].back();
  REQUIRE(rn.contains("rotation"));
  CHECK(rn["rotation"][1].get<double>() == doctest::Approx(std::sin(3.14159265358979 / 4)));
}

// ---------------------------------------------------------------- Candidate choices (milestone 2, A2 / B2)

TEST_CASE("texture candidates: verdict first, then seen-side agreement; the run's own unless clearly beaten") {
  // the anime-style character: the run's own agrees best -> kept.
  std::vector<TextureCandidate> anime{{"own", "PASS", 0.1646, true}, {"s1", "PASS", 0.1690}, {"s2", "PASS", 0.1636 + 0.0}, {"s3", "PASS", 0.1672}};
  anime[2].agreement_de = 0.1645;  // better by less than the margin
  CHECK(choose_texture_candidate(anime).index == 0);
  // Office chair: seed 1 agrees clearly better -> chosen.
  std::vector<TextureCandidate> chair{{"own", "PASS", 0.0978, true}, {"s1", "PASS", 0.0802}, {"s2", "PASS", 0.1170}, {"s3", "PASS", 0.0833}};
  const auto c = choose_texture_candidate(chair);
  CHECK(c.index == 1);
  CHECK(c.reason.find("s1") != std::string::npos);
  // Pixel hood: the run's own is REVIEW, PASS candidates exist -> the best-agreeing PASS one.
  std::vector<TextureCandidate> hood{{"own", "REVIEW", 0.1434, true}, {"s1", "REVIEW", 0.1454}, {"s2", "PASS", 0.1607}, {"s3", "PASS", 0.1528}};
  CHECK(choose_texture_candidate(hood).index == 3);
  // Unmeasured agreement never wins against a measured one of the same verdict.
  std::vector<TextureCandidate> unk{{"own", "PASS", 0.12, true}, {"s1", "PASS", -1}};
  CHECK(choose_texture_candidate(unk).index == 0);
}

TEST_CASE("texture candidates: seen-side agreement is read from the worker report") {
  json r = {{"attempts", json::array({{{"bake", {{"native", {{"calibration", {{"method", "reinhard"}, {"uncalibrated_heldout_delta_e", 0.17},
                                                                              {"tried", json::array({{{"method", "l_quantile"}, {"heldout_delta_e", 0.2}},
                                                                                                     {{"method", "reinhard"}, {"heldout_delta_e", 0.16}}})}}}}}}}}})}};
  CHECK(seen_side_agreement(r) == doctest::Approx(0.16));
  r["attempts"][0]["bake"]["native"]["calibration"]["method"] = "l_quantile_lightness_only";
  CHECK(seen_side_agreement(r) == doctest::Approx(0.17));
  CHECK(seen_side_agreement(json::object()) < 0);
}

TEST_CASE("decimation candidates: first meeting budget and fidelity; crossings only matter in block mode") {
  std::vector<DecimationCandidate> rogue{{"guarded", true, 56034, false, false, 0.106, 0}, {"no_guard", true, 29612, true, true, 0.087, 12}};
  const auto r = choose_decimation(rogue, Intersections::Report);
  CHECK(r.index == 1);
  CHECK(choose_decimation(rogue, Intersections::Block).index == 0);  // over budget, but no crossing
  std::vector<DecimationCandidate> sword{{"guarded", true, 7678, true, true, 0.062, 0}, {"no_guard", true, 7656, true, true, 0.056, 0}};
  CHECK(choose_decimation(sword, Intersections::Report).index == 0);  // the guarded one already does it
  // Nobody meets fidelity: the most faithful one within budget, and the reason says so.
  std::vector<DecimationCandidate> thin{{"guarded", true, 40000, false, false, 0.2, 0}, {"no_guard", true, 9000, true, false, 0.6, 3}, {"x", true, 9500, true, false, 0.4, 0}};
  const auto t = choose_decimation(thin, Intersections::Report);
  CHECK(t.index == 2);
  CHECK(t.reason.find("fidelity") != std::string::npos);
  std::vector<DecimationCandidate> none{{"guarded", false}, {"no_guard", false}};
  CHECK(choose_decimation(none, Intersections::Report).index == SIZE_MAX);
}

TEST_CASE("game geometry verdict: crossings are reported, unsound topology is not accepted") {
  json t = {{"complete", true}, {"triangles", 100}, {"overfull_edges", 0}, {"invalid_vertex_links", 0}, {"inconsistent_winding_edges", 0},
            {"duplicate_faces", 0}, {"internal_faces", 0}, {"zero_area_faces", 0}, {"boundary_edges", 0}};
  json x = {{"pair_classification", {{"transverse_crossing", 153}, {"point_contact", 4}}}, {"edge_star_classification", {{"crossing_sheet_stars", 2}}}};
  json rep;
  CHECK(game_geometry_verdict(t, x, rep) == "PASS");
  CHECK(rep["transverse_crossing"] == 153);
  t["boundary_edges"] = 6;
  CHECK(game_geometry_verdict(t, x, rep) == "REVIEW");
  t["inconsistent_winding_edges"] = 1;
  CHECK(game_geometry_verdict(t, x, rep) == "UNSAFE");
  CHECK(game_geometry_verdict(json::object(), x, rep).empty());
}

// ---------------------------------------------------------------- Foreign multi-mesh GLB (milestone 2, A3)

TEST_CASE("glTF decimation: two meshes share the budget by area; materials, vertices and nodes are untouched") {
  // Mesh 0: a sphere of radius 1; mesh 1: the same sphere data drawn by a node scaled x2 (4x the area).
  Glb a = sphere_glb(64, 32);  // 3968 triangles
  json mesh1 = a.json["meshes"][0];
  a.json["meshes"].push_back(mesh1);
  a.json["materials"] = json::array({{{"name", "keep-me"}, {"pbrMetallicRoughness", {{"baseColorFactor", {1, 0, 0, 1}}}}}});
  a.json["meshes"][0]["primitives"][0]["material"] = 0;
  a.json["nodes"] = json::array({{{"mesh", 0}}, {{"mesh", 1}, {"scale", {2, 2, 2}}, {"translation", {5, 0, 0}}}});
  a.json["scenes"][0]["nodes"] = {0, 1};
  const auto before = inspect_glb(a).triangles;
  REQUIRE(before == 2 * 3968);
  const auto r = decimate_gltf(a, 2000);
  CHECK(r.budget_met);
  CHECK(r.triangles_after <= 2000);
  CHECK(inspect_glb(r.glb).triangles == r.triangles_after);
  CHECK(r.glb.json["materials"] == a.json["materials"]);
  CHECK(r.glb.json["nodes"] == a.json["nodes"]);
  CHECK(r.glb.json["meshes"][0]["primitives"][0]["attributes"] == a.json["meshes"][0]["primitives"][0]["attributes"]);
  // The bigger mesh gets the bigger share (about 4:1 by area).
  const auto& m = r.report["meshes"];
  REQUIRE(m.size() == 2);
  CHECK(m[1]["triangles_after"].get<int>() > 2 * m[0]["triangles_after"].get<int>());
  // It still looks like the original.
  const auto f = measure_fidelity(fmesh_from_glb(a), fmesh_from_glb(r.glb));
  CHECK(f.silhouette_iou_min > 0.95);
  // Round trip through the binary encoder.
  const auto again = parse_glb(encode_glb(r.glb));
  CHECK(inspect_glb(again).triangles == r.triangles_after);
  // Under budget already: unchanged.
  CHECK(decimate_gltf(a, 100000).triangles_after == before);
}

TEST_CASE("exec (M2): GPU memory in use is sampled around every step (NVML; 0 = not measured)") {
  ProcessSpec p;
  p.exe = child_exe();
  p.args = {"sleep", "600"};
  const auto r = run_process(p);
  CHECK(r.exit_code == 0);
  CHECK(r.vram_peak_mib >= r.vram_before_mib);
  if (gpu_memory_used_mib() > 0) CHECK(r.vram_before_mib > 0);  // an NVIDIA driver is installed here
}

TEST_CASE("fidelity loader: a cyclic node hierarchy in a foreign GLB is refused, never recursed forever") {
  Glb g = sphere_glb(8, 4);
  g.json["nodes"] = json::array({{{"mesh", 0}, {"children", {1}}}, {{"children", {0}}}});
  CHECK_THROWS(fmesh_from_glb(g));
  CHECK_THROWS(decimate_gltf(g, 10));
}

// ---------------------------------------------------------------- Review findings (milestone 2)

TEST_CASE("review: accessor counts that overflow the range check are refused, never read") {
  Glb g = sphere_glb(8, 4);
  g.json["accessors"][0]["count"] = (uint64_t(1) << 62) + 1;  // (count-1) * 12 wraps to 0 in 64 bits
  CHECK_THROWS(fmesh_from_glb(g));
  CHECK_THROWS(decimate_gltf(g, 10));
  CHECK_THROWS(delivery_transform(g, ScaleAxis::Height, 1.0, 30.0));
  CHECK_FALSE(with_baked_transform(g, DeliveryTransform{}).has_value());
}

TEST_CASE("review: a mirrored node keeps outward winding (no false inconsistent edges)") {
  Glb g = sphere_glb(16, 8);
  g.json["nodes"] = json::array({{{"mesh", 0}, {"scale", {-1, 1, 1}}}});
  const auto t = topology(fmesh_from_glb(g));
  CHECK(t.inconsistent_winding_edges == 0);
  // Mirrored next to an unmirrored copy: same orientation everywhere, so the faces still point outwards (volume > 0).
  const auto m = fmesh_from_glb(g);
  double vol = 0;
  for (const auto& tr : m.tri) {
    const auto& a = m.pos[tr[0]];
    const auto& b = m.pos[tr[1]];
    const auto& c = m.pos[tr[2]];
    vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6;
  }
  const auto m0 = fmesh_from_glb(sphere_glb(16, 8));
  double vol0 = 0;
  for (const auto& tr : m0.tri) {
    const auto& a = m0.pos[tr[0]];
    const auto& b = m0.pos[tr[1]];
    const auto& c = m0.pos[tr[2]];
    vol0 += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6;
  }
  CHECK(vol * vol0 > 0);
}

TEST_CASE("review: glTF decimation refuses wrong attribute types and leaves meshes outside the scene alone") {
  Glb a = sphere_glb(32, 16);
  a.json["meshes"].push_back(a.json["meshes"][0]);  // mesh 1 is drawn by no node of the scene
  const auto r = decimate_gltf(a, 300);
  CHECK(r.budget_met);
  CHECK(r.glb.json["meshes"][1] == a.json["meshes"][1]);
  // POSITION as VEC2 is refused.
  Glb b = sphere_glb(8, 4);
  b.json["accessors"][0]["type"] = "VEC2";
  CHECK_THROWS(fmesh_from_glb(b));
}

TEST_CASE("review: an unmeasured agreement of the run's own texture keeps the run's own") {
  std::vector<TextureCandidate> c{{"own", "PASS", -1, true}, {"s1", "PASS", 0.05}};
  CHECK(choose_texture_candidate(c).index == 0);
}

// ---------------------------------------------------------------- milestone 3: human-in-the-loop decisions

TEST_CASE("M3 needed count: ladder then bisection finds the smallest passing count within the resolution") {
  int calls = 0;
  const auto s = find_needed_triangles(30000, 150000, [&](int t) -> std::optional<bool> {
    ++calls;
    return t >= 47000;
  });
  CHECK(s.needed >= 47000);
  CHECK(s.needed - s.highest_fail <= 600);  // 2% of the budget
  CHECK(s.highest_fail < 47000);
  CHECK_FALSE(s.stopped);
  CHECK(int(s.probes.size()) == calls);
  CHECK(calls <= 10);
  REQUIRE(s.probes.size() >= 2);
  CHECK(s.probes[0] == std::pair<int, bool>{45000, false});
  CHECK(s.probes[1] == std::pair<int, bool>{60000, true});
}

TEST_CASE("M3 needed count: nothing below the original passes -> the original; a failed measurement stops early") {
  const auto all_fail = find_needed_triangles(1000, 2500, [](int) -> std::optional<bool> { return false; });
  CHECK(all_fail.needed > 2000);  // between the last failing probe and the original
  CHECK(all_fail.needed <= 2500);
  int n = 0;
  const auto stopped = find_needed_triangles(1000, 100000, [&](int) -> std::optional<bool> {
    if (++n > 1) return std::nullopt;
    return false;
  });
  CHECK(stopped.stopped);
  CHECK(stopped.needed == 0);
  CHECK(stopped.probes.size() == 1);
}

namespace {

Question budget_q() {
  Question q;
  q.id = "q1";
  q.kind = "budget_or_fidelity";
  q.title = "t";
  q.options = {{"budget", "b", ""}, {"needed", "n", ""}};
  return q;
}

Question picture_q() {
  Question q;
  q.id = "q2";
  q.kind = "hidden_side_picture";
  q.options = {{"picture", "p", "", true}, {"keep", "k", ""}};
  return q;
}

}  // namespace

TEST_CASE("M3 answers: only this question's options, a named person, and an existing picture for a picture option") {
  std::string err;
  CHECK_FALSE(parse_answer({{"choice", "maybe"}, {"who", "sam"}}, budget_q(), err));
  CHECK(err.find("not one of") != std::string::npos);
  CHECK_FALSE(parse_answer({{"choice", "needed"}}, budget_q(), err));
  CHECK(err.find("who") != std::string::npos);
  CHECK_FALSE(parse_answer({{"id", "q9"}, {"choice", "needed"}, {"who", "sam"}}, budget_q(), err));
  const auto a = parse_answer({{"choice", "needed"}, {"who", "sam"}, {"when", "2026-11-01T10:00:00"}}, budget_q(), err);
  REQUIRE(a);
  CHECK(a->who == "sam");
  CHECK(a->when == "2026-11-01T10:00:00");
  const auto dir = scratch("m3-answers");
  CHECK_FALSE(parse_answer({{"choice", "picture"}, {"who", "sam"}, {"picture", pixel3d::to_utf8(dir / "none.png")}}, picture_q(), err));
  std::ofstream(dir / "back.png") << "x";
  CHECK_FALSE(parse_answer({{"choice", "picture"}, {"who", "sam"}, {"picture", pixel3d::to_utf8(dir / "back.png")}, {"label", "upside"}}, picture_q(), err));
  const auto p = parse_answer({{"choice", "picture"}, {"who", "sam"}, {"picture", pixel3d::to_utf8(dir / "back.png")}}, picture_q(), err);
  REQUIRE(p);
  CHECK(p->label == "back");  // default angle
  const auto k = parse_answer({{"choice", "keep"}, {"who", "sam"}, {"picture", "ignored"}}, picture_q(), err);
  REQUIRE(k);
  CHECK(k->picture.empty());
}

TEST_CASE("M3 channel: headless never answers; an answers file answers by kind with its person; ask waits for the file") {
  const auto dir = scratch("m3-channel");
  {
    DecisionChannel ch(dir / "headless", {});
    auto q = budget_q();
    std::string outcome;
    double waited = -1;
    CHECK_FALSE(ch.ask(q, outcome, waited));
    CHECK(outcome.find("PENDING_DECISION") != std::string::npos);
    CHECK(waited == 0);
    CHECK(fs::exists(dir / "headless" / "decisions" / "q1.question.json"));
    CHECK_FALSE(fs::exists(dir / "headless" / "decisions" / "q1.answer.json"));
  }
  {
    std::ofstream(dir / "answers.json")
        << json::array({{{"kind", "budget_or_fidelity"}, {"choice", "needed"}, {"who", "demo lead"}, {"when", "2026-11-02T09:00:00"}}}).dump();
    DecisionOptions o;
    o.answers_file = dir / "answers.json";
    DecisionChannel ch(dir / "preset", o);
    auto q = budget_q();
    std::string outcome;
    double waited = 0;
    const auto a = ch.ask(q, outcome, waited);
    REQUIRE(a);
    CHECK(a->choice == "needed");
    CHECK(a->who == "demo lead");
    CHECK(a->source == "answers file");
    CHECK(fs::exists(dir / "preset" / "decisions" / "q1.answer.json"));
    auto q2 = picture_q();  // no answer of that kind given: headless -> pending
    CHECK_FALSE(ch.ask(q2, outcome, waited));
  }
  {
    DecisionOptions o;
    o.mode = AskMode::Ask;
    o.wait_seconds = 20;
    DecisionChannel ch(dir / "ask", o);
    std::thread person([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(600));
      std::ofstream(dir / "ask" / "decisions" / "q1.answer.json") << json{{"choice", "nonsense"}, {"who", "x"}}.dump();
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      Answer a;
      a.id = "q1";
      a.choice = "budget";
      a.who = "sam";
      a.when = now_iso();
      a.source = "app";
      std::string err;
      CHECK(write_answer_file(dir / "ask", a, err));
    });
    auto q = budget_q();
    std::string outcome;
    double waited = 0;
    const auto a = ch.ask(q, outcome, waited);
    person.join();
    REQUIRE(a);
    CHECK(a->choice == "budget");
    CHECK(a->source == "app");
    CHECK(waited > 1.0);
    bool refused_kept = false;
    for (const auto& e : fs::directory_iterator(dir / "ask" / "decisions"))
      if (e.path().filename().string().find("refused") != std::string::npos) refused_kept = true;
    CHECK(refused_kept);
  }
  {
    DecisionOptions o;
    o.mode = AskMode::Ask;
    o.wait_seconds = 0.5;
    DecisionChannel ch(dir / "timeout", o);
    auto q = budget_q();
    std::string outcome;
    double waited = 0;
    CHECK_FALSE(ch.ask(q, outcome, waited));
    CHECK(outcome.find("PENDING_DECISION") != std::string::npos);
  }
}

TEST_CASE("M3 answer file: needs the question, never replaces an answer") {
  const auto dir = scratch("m3-write");
  Answer a;
  a.id = "q1";
  a.choice = "needed";
  a.who = "sam";
  std::string err;
  CHECK_FALSE(write_answer_file(dir, a, err));
  fs::create_directories(dir / "decisions");
  std::ofstream(dir / "decisions" / "q1.question.json") << "{}";
  CHECK(write_answer_file(dir, a, err));
  CHECK_FALSE(write_answer_file(dir, a, err));
  CHECK(err.find("already answered") != std::string::npos);
}

TEST_CASE("M3 hidden side: gate issues map to the catalogued kinds; a back seen by a picture is not inferred") {
  const json rep = {{"issues", {"REVIEW: hidden sides are large flat colour patches with little of the picture's detail",
                                "REVIEW: the model's outline matches the picture poorly (44%)",
                                "REVIEW: the back repeats the picture's front features at the same places (possible face or details copied to the back)"}},
                    {"candidates", {{{"quality", {{"hidden_side", {{"back_seen_by_extra_pictures_fraction", 0.0}, {"front_copy_correlation", 0.5}}}}}}}}};
  const auto a = assess_hidden_side(rep, true);
  CHECK(a.inferred);
  CHECK(a.weak);
  REQUIRE(a.findings.size() == 2);
  CHECK(a.findings[0].kind == "flat, low-detail back");
  CHECK(a.findings[0].catalogue.find("D14") != std::string::npos);
  CHECK(a.findings[1].kind == "front copied to the back");
  REQUIRE(a.other_issues.size() == 1);
  CHECK(a.other_issues[0].find("outline") != std::string::npos);
  CHECK(a.numbers["front_copy_correlation"] == 0.5);
  json seen = rep;
  seen["candidates"][0]["quality"]["hidden_side"]["back_seen_by_extra_pictures_fraction"] = 0.8;
  CHECK_FALSE(assess_hidden_side(seen, true).weak);
  CHECK_FALSE(assess_hidden_side({{"issues", {"REVIEW: visible colours drift from the picture"}}}, true).weak);
}

TEST_CASE("M3 acceptance: a person's answer makes the criterion ACCEPTED with who/when; a FAIL elsewhere still wins") {
  std::vector<Criterion> c = {{"triangle_budget", "FAIL", "60000 triangles, budget 30000"}, {"geometry", "PASS", ""}};
  apply_human_acceptance(c, {{"triangle_budget", "decided by sam at 2026-11-02 (q1)"}});
  CHECK(c[0].status == "ACCEPTED");
  CHECK(c[0].detail.find("60000 triangles") != std::string::npos);
  CHECK(c[0].detail.find("sam") != std::string::npos);
  CHECK(overall_status(c) == "ACCEPTED");
  c.push_back({"godot_import", "FAIL", "x"});
  CHECK(overall_status(c) == "FAIL");
  std::vector<Criterion> p = {{"geometry", "PASS", ""}};
  apply_human_acceptance(p, {{"geometry", "x"}});
  CHECK(p[0].status == "PASS");  // nothing to accept
}

namespace {

json budget_question_spec(const fs::path& glb) {
  auto s = glb_spec(glb, 150);  // a 9024-triangle sphere cannot keep its shape at 150 triangles
  s["limits"]["total_seconds"] = 900;
  return s;
}

const json* find_criterion(const json& report, const std::string& name) {
  for (const auto& c : report["acceptance"])
    if (c["criterion"] == name) return &c;
  return nullptr;
}

}  // namespace

TEST_CASE("agent M3: headless, budget impossible within fidelity -> PENDING_DECISION, measured result kept, renders written") {
  const auto dir = scratch("m3-agent-headless");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(96, 48)));
  const auto a = run_agent(dir, budget_question_spec(dir / "ball.glb"));
  REQUIRE(a.report.is_object());
  CHECK(a.exit_code == 2);
  CHECK(a.report["status"] == "FAIL");
  CHECK(a.report["decision_status"] == "PENDING_DECISION");
  CHECK(a.report["decisions_mode"] == "headless");
  REQUIRE(a.report["pending_decisions"].size() == 1);
  const auto& q = a.report["pending_decisions"][0]["question"];
  CHECK(q["kind"] == "budget_or_fidelity");
  CHECK(q["options"].size() == 2);
  const int needed = q["numbers"]["needed"].get<int>();
  CHECK(needed > 150);
  CHECK(needed < 9024);
  CHECK(a.report["fidelity"]["needs_triangles"] == needed);
  CHECK(a.report["needs_search"]["probes"].size() >= 3);
  REQUIRE(q["renders"].size() == 9);  // original / budget / needed x front, side, back
  for (const auto& r : q["renders"]) CHECK(fs::exists(pixel3d::path_from_utf8(r["path"].get<std::string>())));
  CHECK(fs::exists(a.out / "decisions" / "q1.question.json"));
  CHECK(a.report["summary"].get<std::string>().find("PENDING_DECISION") != std::string::npos);
  CHECK(inspect_glb(read_glb(a.out / "ball.glb")).triangles <= 150);  // M2 behaviour: the budget result is delivered
  bool question_logged = false, thumbs = false;
  for (const auto& e : a.log) {
    if (e.value("event", "") == "question") question_logged = true;
    if (e.value("event", "") == "decimation_candidate" && e["thumbnail"].is_string())
      thumbs = fs::exists(pixel3d::path_from_utf8(e["thumbnail"].get<std::string>()));
  }
  CHECK(question_logged);
  CHECK(thumbs);
  std::ifstream md(a.out / "report.md");
  const std::string text((std::istreambuf_iterator<char>(md)), std::istreambuf_iterator<char>());
  CHECK(text.find("PENDING_DECISION") != std::string::npos);
}

TEST_CASE("agent M3: an answer given beforehand delivers the needed count; the budget criterion is ACCEPTED with who/when") {
  const auto dir = scratch("m3-agent-answers");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(96, 48)));
  std::ofstream(dir / "answers.json")
      << json{{"answers", {{{"kind", "budget_or_fidelity"}, {"choice", "needed"}, {"who", "test lead"}, {"when", "2026-11-03T12:00:00"}}}}}.dump();
  const auto a = run_agent(dir, budget_question_spec(dir / "ball.glb"), {"--answers", pixel3d::to_utf8(dir / "answers.json")});
  REQUIRE(a.report.is_object());
  CHECK(a.report["decision_status"] == "DECIDED");
  REQUIRE(a.report["human_decisions"].size() == 1);
  const auto& d = a.report["human_decisions"][0];
  CHECK(d["answer"]["who"] == "test lead");
  CHECK(d["answer"]["when"] == "2026-11-03T12:00:00");
  CHECK(d["answer"]["source"] == "answers file");
  CHECK(d["outcome"].get<std::string>().find("delivered at") != std::string::npos);
  const int needed = d["question"]["numbers"]["needed"].get<int>();
  const auto tris = inspect_glb(read_glb(a.out / "ball.glb")).triangles;
  CHECK(tris > 150);
  CHECK(tris <= needed);
  const auto* budget = find_criterion(a.report, "triangle_budget");
  REQUIRE(budget);
  CHECK((*budget)["status"] == "ACCEPTED");
  CHECK((*budget)["detail"].get<std::string>().find("test lead") != std::string::npos);
  const auto* fid = find_criterion(a.report, "fidelity");
  REQUIRE(fid);
  CHECK((*fid)["status"] == "PASS");
  CHECK(a.report["status"] != "FAIL");  // UNVERIFIED here: no Godot in the unit tests
}

TEST_CASE("agent M3: --ask waits for a person; `3re-agent --answer` answers; the wait is not counted as work") {
  const auto dir = scratch("m3-agent-ask");
  write_new_file(dir / "ball.glb", encode_glb(sphere_glb(96, 48)));
  std::thread person([&] {
    const auto q = dir / "out" / "decisions" / "q1.question.json";
    for (int i = 0; i < 600 && !fs::exists(q); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    ProcessSpec p;
    p.exe = fs::path(AGENT_EXE);
    p.args = {"--answer", pixel3d::to_utf8(dir / "out"), "q1", "budget", "--who", "reviewer"};
    p.stdout_file = dir / "answer.out.txt";
    p.stderr_file = dir / "answer.err.txt";
    CHECK(run_process(p).exit_code == 0);
  });
  const auto a = run_agent(dir, budget_question_spec(dir / "ball.glb"), {"--ask", "--wait-seconds", "120"});
  person.join();
  REQUIRE(a.report.is_object());
  CHECK(a.report["decisions_mode"] == "ask");
  REQUIRE(a.report["human_decisions"].size() == 1);
  CHECK(a.report["human_decisions"][0]["answer"]["who"] == "reviewer");
  CHECK(a.report["human_decisions"][0]["answer"]["source"] == "cli");
  CHECK(a.report["waited_for_person_seconds"].get<double>() > 1.0);
  const auto* fid = find_criterion(a.report, "fidelity");
  REQUIRE(fid);
  CHECK((*fid)["status"] == "ACCEPTED");
  CHECK(inspect_glb(read_glb(a.out / "ball.glb")).triangles <= 150);
  bool answered = false;
  for (const auto& e : a.log)
    if (e.value("event", "") == "answer") answered = true;
  CHECK(answered);
}

TEST_CASE("multi-mesh topology: two separate textured parts touching at a seam are not a non-manifold edge") {
  // Two quads in two primitives sharing an edge position, each with UVs: welding stays within a primitive.
  Glb g;
  const std::vector<float> v{0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 2, 0, 0, 2, 1, 0, 1, 1, 0};
  const std::vector<float> uv(16, 0.5f);
  const std::vector<uint32_t> idx{0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 2, 3};
  g.bin.resize(96 + 64 + 48);
  std::memcpy(g.bin.data(), v.data(), 96);
  std::memcpy(g.bin.data() + 96, uv.data(), 64);
  std::memcpy(g.bin.data() + 160, idx.data(), 48);
  g.json = json::parse(R"({
    "asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 2}, "indices": 4},
                               {"attributes": {"POSITION": 1, "TEXCOORD_0": 3}, "indices": 5}]}],
    "buffers": [{"byteLength": 208}],
    "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48}, {"buffer": 0, "byteOffset": 48, "byteLength": 48},
                    {"buffer": 0, "byteOffset": 96, "byteLength": 32}, {"buffer": 0, "byteOffset": 128, "byteLength": 32},
                    {"buffer": 0, "byteOffset": 160, "byteLength": 24}, {"buffer": 0, "byteOffset": 184, "byteLength": 24}],
    "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
                  {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
                  {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
                  {"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC2"},
                  {"bufferView": 4, "componentType": 5125, "count": 6, "type": "SCALAR"},
                  {"bufferView": 5, "componentType": 5125, "count": 6, "type": "SCALAR"}]})");
  const auto t = topology(fmesh_from_glb(g));
  CHECK(t.overfull_edges == 0);
  CHECK(t.boundary_edges == 8);  // two open quads, 4 border edges each
}
