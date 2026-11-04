#include "agent_run.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include "agent_decide.hpp"
#include "agent_eval.hpp"
#include "agent_exec.hpp"
#include "agent_fidelity.hpp"
#include "agent_glb.hpp"
#include "agent_godot.hpp"
#include "agent_gltf_decimate.hpp"
#include "agent_hygiene.hpp"
#include "agent_plan.hpp"
#include "agent_probe.hpp"
#include "agent_recovery.hpp"
#include "agent_select.hpp"
#include "backends/mesh_asset.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/image.hpp"

namespace p3dagent {
namespace {

using nlohmann::json;
using pixel3d::to_utf8;
using Clock = std::chrono::steady_clock;


fs::path exe_dir() {
  wchar_t buf[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
  return fs::path(std::wstring(buf, n)).parent_path();
}

std::string env(const char* name) {
  wchar_t buf[32768];
  const DWORD n = GetEnvironmentVariableW(widen(name).c_str(), buf, 32768);
  return n && n < 32768 ? narrow(std::wstring(buf, n)) : std::string();
}

json read_json_file(const fs::path& f) {
  auto bytes = pixel3d::read_file(f, 64u << 20);
  if (!bytes) throw std::runtime_error("cannot read " + to_utf8(f));
  return json::parse(bytes->begin(), bytes->end());
}

std::string read_text(const fs::path& f) {
  std::ifstream in(f, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

void write_text_new(const fs::path& f, const std::string& text) { write_new_file(f, std::vector<uint8_t>(text.begin(), text.end())); }

struct Source {
  fs::path run, geometry, raw, native, cutout, textured, textured_json;
  uint32_t seed = 42;
};

struct Geometry {
  fs::path glb;
  std::string verdict;  // PASS / REVIEW / UNSAFE / "" (not measured)
  std::string game_verdict;  // game target (A1): soundness only, crossings reported (agent_select.hpp)
  json reported = json::object();  // crossings / contacts / topology counts, as reported
  int64_t triangles = 0;
  int64_t crossings = -1;
  json topology, intersections;
};

struct Texture {
  fs::path glb;
  std::string verdict;
  json summary;
  int size = 0;
  bool hybrid = false;
  fs::path native;  // the TRELLIS texture volume it was made with ("" = picture only)
};

class Runner {
 public:
  Runner(AssetSpec spec, AgentOptions opt, fs::path out) : spec_(std::move(spec)), opt_(std::move(opt)), out_(std::move(out)),
                                                         work_(out_ / "work"), log_(out_ / "run.jsonl"), rec_(spec_),
                                                         chan_(out_, opt_.decisions) {}
  int run();

 private:
  double elapsed() const { return std::chrono::duration<double>(Clock::now() - t0_).count(); }
  // The time budget counts the agent's own work, not the time a person takes to answer a question.
  double busy() const { return elapsed() - waited_; }
  void say(const std::string& text) {
    char stamp[32];
    std::snprintf(stamp, sizeof stamp, "[%7.1fs] ", elapsed());
    std::cout << stamp << text << std::endl;
  }
  ProcessResult step(const std::string& name, const std::string& tool, const fs::path& exe, const std::vector<std::string>& args,
                     const std::string& ram_key, json extra = json::object(), std::map<std::string, std::string> envs = {},
                     double timeout = 0, fs::path cwd = {});
  void record(const Decision& d);
  void human(const std::string& text) { human_.push_back(text); }
  bool generate(uint32_t seed, Source& src);
  bool cleanup(Source& src);
  Geometry inspect(const fs::path& glb, const std::string& label);
  std::optional<Texture> texture(const fs::path& mesh, const fs::path& picture, const fs::path& native, int size, const std::string& label,
                                 const std::vector<std::pair<fs::path, std::string>>& views = {});
  std::optional<fs::path> decimate(const fs::path& glb, int target, json& report, const std::string& mode = "guarded");
  json fidelity(const fs::path& original, const fs::path& reduced, const std::string& label);
  Texture texture_candidates(const Source& src, const fs::path& mesh, const fs::path& picture, Texture own);
  void thumbnail(const fs::path& glb, double yaw, const fs::path& png, int size = 192);
  json renders(const fs::path& glb, const std::vector<double>& yaws, const fs::path& dir, const std::string& prefix, const std::string& group,
               const std::string& caption);
  // Milestone 3: ask a person (or record PENDING_DECISION). Returns the index into human_decisions_ when answered.
  std::optional<std::pair<Answer, size_t>> ask(Question q);
  void decided(size_t index, const std::string& outcome, json details = json::object());
  void budget_question(const Source& src, const Geometry& geo, const std::vector<DecimationCandidate>& cands, const std::vector<fs::path>& outs,
                       const json& table, size_t chosen, fs::path& mesh, bool& decimated, Geometry& geo_after, json& dec_report);
  std::optional<Texture> hidden_side_question(const fs::path& mesh, const fs::path& picture, const Texture& tex);
  json validate(const fs::path& delivered, const GlbInfo& info);
  int finish(const std::string& status, const std::string& summary, json extra);

  AssetSpec spec_;
  AgentOptions opt_;
  fs::path out_, work_;
  RunLog log_;
  RecoveryController rec_;
  Capabilities caps_;
  Plan plan_;
  Clock::time_point t0_ = Clock::now();
  json steps_ = json::array(), decisions_ = json::array();
  std::vector<std::string> human_;
  std::vector<std::pair<fs::path, std::string>> deliverables_;
  int step_no_ = 0;
  fs::path config_, last_stdout_;
  std::string delivery_method_;
  json texture_candidates_ = json::array();
  json decimation_candidates_ = json::array();
  json front_;
  int needs_triangles_ = 0;
  int64_t needs_stalled_at_ = 0;
  json fidelity_geometry_ = json::object(), fidelity_appearance_ = json::object();
  DecisionChannel chan_;
  double waited_ = 0;
  json human_decisions_ = json::array(), pending_decisions_ = json::array(), needs_search_ = json::object(), hidden_side_ = json::object();
  std::vector<std::pair<std::string, std::string>> accepted_;  // criterion -> why (a person's answer)
  // Milestone 4: the spec's extra pictures as PNG (the worker reads PNG), used by every texture of the run.
  std::vector<std::pair<fs::path, std::string>> spec_views_;
  std::map<std::string, json> pictures_by_glb_;  // per textured GLB: what the worker's registration said about each extra picture
  bool prepare_views();
};

ProcessResult Runner::step(const std::string& name, const std::string& tool, const fs::path& exe, const std::vector<std::string>& args,
                           const std::string& ram_key, json extra, std::map<std::string, std::string> envs, double timeout, fs::path cwd) {
  ++step_no_;
  char prefix[16];
  std::snprintf(prefix, sizeof prefix, "%02d-", step_no_);
  ProcessSpec p;
  p.exe = exe;
  p.args = args;
  p.cwd = cwd;
  p.env = std::move(envs);
  // Validation steps (explicit timeout) always get their own time: an exhausted budget must not turn
  // into a false FAIL. Every other step is bounded by what is left of the total budget.
  const double remaining = std::max(5.0, spec_.limits.total_seconds - busy());
  p.timeout_seconds = timeout > 0 ? timeout : std::min(spec_.limits.step_seconds, remaining);
  const auto lim = spec_.limits.step_ram_mib.find(ram_key);
  p.memory_limit_mib = lim == spec_.limits.step_ram_mib.end() ? 0 : lim->second;
  p.stdout_file = work_ / (prefix + name + ".out.txt");
  p.stderr_file = work_ / (prefix + name + ".err.txt");
  say("run " + name + ": " + tool + (p.memory_limit_mib ? " (memory limit " + std::to_string(p.memory_limit_mib) + " MiB)" : ""));
  log_.append({{"event", "step_started"}, {"name", name}, {"tool", tool}, {"number", step_no_}});  // the run viewer shows what runs now
  const auto r = run_process(p);
  last_stdout_ = p.stdout_file;
  json e = {{"step", name}, {"tool", tool}, {"exe", to_utf8(exe)}, {"args", args}, {"memory_limit_mib", p.memory_limit_mib},
            {"timeout_seconds", p.timeout_seconds}, {"started", r.started}, {"exit_code", r.exit_code}, {"timed_out", r.timed_out},
            {"memory_limit_hit", r.memory_limit_hit}, {"seconds", r.seconds}, {"peak_memory_mib", r.peak_memory_mib},
            {"vram_before_mib", r.vram_before_mib}, {"vram_peak_mib", r.vram_peak_mib},
            {"stdout", to_utf8(p.stdout_file)}, {"stderr", to_utf8(p.stderr_file)}};
  if (!r.error.empty()) e["error"] = r.error;
  if (extra.is_object()) e.update(extra);
  log_.append(e);
  steps_.push_back({{"step", name}, {"tool", tool}, {"exit_code", r.exit_code}, {"seconds", r.seconds},
                    {"peak_memory_mib", r.peak_memory_mib}, {"vram_before_mib", r.vram_before_mib}, {"vram_peak_mib", r.vram_peak_mib},
                    {"timed_out", r.timed_out}, {"memory_limit_hit", r.memory_limit_hit}});
  char tail[160];
  char gpu[64] = "";
  if (r.vram_peak_mib > r.vram_before_mib + 16)
    std::snprintf(gpu, sizeof gpu, ", GPU +%llu MiB", static_cast<unsigned long long>(r.vram_peak_mib - r.vram_before_mib));
  std::snprintf(tail, sizeof tail, "  -> exit %d, %.1f s, peak %llu MiB%s%s%s", r.exit_code, r.seconds,
                static_cast<unsigned long long>(r.peak_memory_mib), gpu, r.timed_out ? ", TIMED OUT" : "",
                r.memory_limit_hit ? ", OUT OF MEMORY (job limit)" : "");
  say(r.started ? tail : "  -> not started: " + r.error);
  return r;
}

void Runner::record(const Decision& d) {
  json j = d.to_json();
  j["elapsed_seconds"] = elapsed();
  decisions_.push_back(j);
  log_.append({{"event", "decision"}, {"decision", j}});
  say(std::string("decision [") + d.rule + "] " + to_string(d.action) + ": " + d.reason);
  if (d.action == Action::HumanReview || d.action == Action::Stop) human(d.reason);
}

bool Runner::generate(uint32_t seed, Source& src) {
  const auto tag = "gen-seed" + std::to_string(seed);
  const auto summary = work_ / (tag + ".json");
  const auto project = work_ / tag;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto r = step("generate", "3re-studio --e2e-generate (TRELLIS.2 wizard path)", caps_.tools["studio"].path,
                        {"--e2e-generate", to_utf8(spec_.picture), "--e2e-out", to_utf8(summary), "--project-dir", to_utf8(project),
                         "--e2e-seed", std::to_string(seed)},
                        "generate", {{"seed", seed}}, {{"THREERE_TRELLIS_CONFIG", to_utf8(config_)}});
    json s = json::object();
    try {
      s = read_json_file(summary);
      if (!s.is_object()) s = json::object();
    } catch (...) {
    }
    const auto result = s.value("result", std::string());
    const bool oom = r.memory_limit_hit || result.find("memory") != std::string::npos || result.find("RAM") != std::string::npos;
    // The wizard can stop after it wrote the geometry and TRELLIS's texture volume (its 20-minute bounded runtime
    // runs out while it textures a dense model): the agent textures the mesh itself anyway, so it continues from that run.
    bool partial = false;
    if (r.exit_code != 0 && s.contains("run_directory") && s["run_directory"].is_string()) {
      const auto run = pixel3d::path_from_utf8(s["run_directory"].get<std::string>());
      if (fs::exists(run / "geometry.glb") && fs::exists(run / "raw.ply") && fs::exists(run / "input.png")) {
        partial = true;
        record({Action::Accept, "generation_partial",
                "the wizard stopped (" + (result.empty() ? "exit " + std::to_string(r.exit_code) : result) +
                    ") after writing the cleaned geometry" + (fs::exists(run / "native.post") ? " and TRELLIS's texture volume" : "") +
                    ": the agent continues from that run and textures it itself"});
      }
    }
    if ((r.exit_code == 0 || partial) && s.contains("run_directory")) {
      src = Source{};
      src.seed = seed;
      src.run = pixel3d::path_from_utf8(s["run_directory"].get<std::string>());
      src.geometry = src.run / "geometry.glb";
      src.raw = src.run / "raw.ply";
      if (fs::exists(src.run / "native.post")) src.native = src.run / "native.post";
      if (fs::exists(src.run / "texture-source.png")) src.cutout = src.run / "texture-source.png";
      if (fs::exists(src.run / "textured.glb") && fs::exists(src.run / "textured.glb.texture.json")) {
        src.textured = src.run / "textured.glb";
        src.textured_json = src.run / "textured.glb.texture.json";
      }
      log_.append({{"event", "generated"}, {"run", to_utf8(src.run)}, {"validation", s.value("validation", "")},
                   {"total_seconds", s.value("total_seconds", 0.0)}, {"backend_memory", s["backend_report"].value("memory", json::object())}});
      return true;
    }
    if (!oom) {
      human("generation failed: " + (result.empty() ? "exit code " + std::to_string(r.exit_code) : result));
      return false;
    }
    rec_.set_elapsed(busy());
    const auto d = rec_.on_out_of_memory("generate", 0);
    record(d);
    if (d.action != Action::RetryAfterWait) return false;
    for (int i = 0; i < 60; ++i) {  // up to 5 minutes for the wizard's admission RAM
      if (probe_system(out_).ram_free_mib >= 3328) break;
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::error_code ec;
    fs::remove(summary, ec);
    fs::remove_all(project, ec);
  }
  return false;
}

bool Runner::cleanup(Source& src) {
  const auto out = work_ / "geometry-safe.glb";
  if (fs::exists(out)) return false;
  const auto r = step("cleanup", "pixel3d-trellis-post-safe (pinned GeometrySafe)", caps_.tools["post_safe"].path,
                      {to_utf8(src.raw), to_utf8(out), "--preserve-topology"}, "cleanup");
  if (r.exit_code != 0 || !fs::exists(out)) {
    human("re-running the pinned cleanup on raw.ply failed (exit " + std::to_string(r.exit_code) + ")");
    return false;
  }
  src.geometry = out;
  // The run's textured GLB was made on the old mesh: never deliver it for the cleaned one.
  src.textured.clear();
  src.textured_json.clear();
  return true;
}

Geometry Runner::inspect(const fs::path& glb, const std::string& label) {
  Geometry g;
  g.glb = glb;
  try {
    g.triangles = inspect_glb(read_glb(glb)).triangles;
  } catch (const std::exception& e) {
    human(std::string("cannot read ") + to_utf8(glb) + ": " + e.what());
    return g;
  }
  if (!caps_.has("inspect") || !caps_.has("audit")) {
    human("geometry audit tools missing: geometry is UNVERIFIED");
    return g;
  }
  const auto dir = work_ / ("inspect-" + label);
  const auto r1 = step("inspect", "pixel3d-asset-inspect", caps_.tools["inspect"].path, {to_utf8(glb), to_utf8(dir)}, "inspect");
  if (r1.exit_code != 0) {
    // A3: a GLB with several meshes / nodes (other generators): the whole scene in world space, exact positions
    // welded (UV seams are not holes), measured with the same topology code and the same exact crossing audit.
    try {
      const auto fm = fmesh_from_glb(read_glb(glb));
      pixel3d::Mesh mesh;
      std::map<std::pair<int32_t, std::array<float, 3>>, uint32_t> weld;  // within each primitive only
      std::vector<uint32_t> id(fm.pos.size());
      for (size_t i = 0; i < fm.pos.size(); ++i) {
        const auto [it, fresh] = weld.emplace(std::make_pair(fm.vert_prim[i], fm.pos[i]), uint32_t(mesh.positions.size()));
        if (fresh) mesh.positions.push_back({double(fm.pos[i][0]), double(fm.pos[i][1]), double(fm.pos[i][2])});
        id[i] = it->second;
      }
      for (const auto& t : fm.tri) mesh.tris.push_back({id[t[0]], id[t[1]], id[t[2]]});
      fs::create_directories(dir);
      p3dasset::write_geometry_binary(mesh, dir / "geometry.bin");
      write_text_new(dir / "topology.json", p3dasset::inspect_geometry(mesh).dump(1));
      log_.append({{"event", "multi_mesh_audit"}, {"glb", to_utf8(glb)}, {"triangles", mesh.tris.size()}, {"vertices_welded", mesh.positions.size()},
                   {"why", "pixel3d-asset-inspect reads single-mesh 3re files only"}});
      say("geometry " + label + ": several meshes — audited as one world-space mesh (" + std::to_string(mesh.tris.size()) + " triangles)");
    } catch (const std::exception& e) {
      human(std::string("multi-mesh audit failed: ") + e.what());
      return g;
    }
  }
  const auto r2 = step("audit", "pixel3d-geometry-audit (exact self-intersections)", caps_.tools["audit"].path,
                       {to_utf8(dir / "geometry.bin"), to_utf8(dir / "intersections.json")}, "audit");
  if (r2.exit_code != 0) return g;
  g.topology = json::object();
  g.intersections = json::object();
  try {
    g.topology = read_json_file(dir / "topology.json");
    g.intersections = read_json_file(dir / "intersections.json");
    if (!g.topology.is_object() || !g.intersections.is_object()) throw std::runtime_error("not JSON objects");
    g.verdict = p3dasset::validation_name(p3dasset::classify_validation(g.topology, g.intersections));
    if (g.verdict == "Unknown" || g.verdict.empty()) g.verdict.clear();
  } catch (const std::exception& e) {
    human(std::string("geometry audit unreadable: ") + e.what());
    g.verdict.clear();
    g.topology = json::object();
    g.intersections = json::object();
  }
  const auto pc = g.intersections.value("pair_classification", json::object());
  const int crossings = g.intersections.contains("pair_classification") ? pc.value("transverse_crossing", 0) : -1;
  g.crossings = crossings;
  g.game_verdict = game_geometry_verdict(g.topology, g.intersections, g.reported);
  log_.append({{"event", "geometry"}, {"label", label}, {"glb", to_utf8(glb)}, {"verdict", g.verdict}, {"triangles", g.triangles},
               {"components", g.topology.value("components", -1)}, {"boundary_edges", g.topology.value("boundary_edges", -1)},
               {"transverse_crossings", crossings}});
  say("geometry " + label + ": " + std::to_string(g.triangles) + " triangles, audit " + (g.verdict.empty() ? "not measured" : g.verdict) +
      ", crossings " + (crossings < 0 ? std::string("not measured") : std::to_string(crossings)));
  return g;
}

std::optional<fs::path> Runner::decimate(const fs::path& glb, int target, json& report, const std::string& mode) {
  const bool allow_touching = mode == "touching", no_guard = mode == "no_guard";
  const auto out = work_ / ("decimated-" + std::to_string(target) + (allow_touching ? "-touching" : no_guard ? "-noguard" : "") + ".glb");
  std::vector<std::string> args = {to_utf8(glb), to_utf8(out), "--target", std::to_string(target), "--max-threshold", "1e12"};
  if (allow_touching) args.push_back("--allow-touching");
  if (no_guard) args.push_back("--no-guard");
  const auto r = step("decimate", allow_touching ? "pixel3d-decimate-guarded (no crossings; touching allowed)"
                                  : no_guard   ? "pixel3d-decimate-guarded --no-guard (QEM without the crossing guard; crossings reported)"
                                               : "pixel3d-decimate-guarded (exact contact guard)",
                      caps_.tools["decimate"].path, args, "decimate", {{"target", target}, {"mode", mode}});
  if (r.exit_code != 0 || !fs::exists(out)) return std::nullopt;
  try {
    report = read_json_file(fs::path(out).concat(L".decimate.json"));
  } catch (...) {
    return std::nullopt;
  }
  return out;
}

bool Runner::prepare_views() {
  for (const auto& v : spec_.views) {
    const fs::path png = work_ / ("view-" + std::to_string(spec_views_.size() + 1) + "-" + v.label + ".png");
    try {
      auto bytes = pixel3d::read_file(v.picture, 256u << 20);
      if (!bytes) throw std::runtime_error("cannot read the picture");
      auto img = pixel3d::decode_image(*bytes, to_utf8(v.picture.filename()));
      if (!img) throw std::runtime_error("not a picture the agent can read");
      write_new_file(png, pixel3d::encode_png(*img));
    } catch (const std::exception& e) {
      human("extra picture " + to_utf8(v.picture) + " (" + v.label + ") could not be used: " + e.what());
      continue;
    }
    spec_views_.push_back({png, v.label});
    log_.append({{"event", "picture_added"}, {"picture", to_utf8(v.picture)}, {"label", v.label}, {"png", to_utf8(png)}});
  }
  return spec_views_.size() == spec_.views.size();
}

std::optional<Texture> Runner::texture(const fs::path& mesh, const fs::path& picture, const fs::path& native, int size,
                                       const std::string& label, const std::vector<std::pair<fs::path, std::string>>& views) {
  if (!caps_.has("auto_texture")) {
    human("texture worker missing: the mesh cannot be textured");
    return std::nullopt;
  }
  // The spec's extra pictures always take part; a picture given later (hidden-side answer) is added to them.
  std::vector<std::pair<fs::path, std::string>> use_views = spec_views_;
  use_views.insert(use_views.end(), views.begin(), views.end());
  for (;;) {
    const auto out = work_ / ("textured-" + label + "-" + std::to_string(size) + ".glb");
    std::vector<std::string> args = {to_utf8(mesh), to_utf8(picture), to_utf8(out), "--max-size", std::to_string(size), "--seconds",
                                     std::to_string(int(std::min(600.0, spec_.limits.step_seconds)))};
    if (!native.empty()) args.insert(args.end(), {"--native", to_utf8(native)});
    for (const auto& [file, angle] : use_views) args.insert(args.end(), {"--view", to_utf8(file), angle});
    std::string tool = native.empty() ? "pixel3d-auto-texture (picture only" : "pixel3d-auto-texture (hybrid: picture + native.post";
    tool += use_views.empty() ? ")" : " + " + std::to_string(use_views.size()) + " more picture(s), multi-angle)";
    const auto r = step("texture", tool, caps_.tools["auto_texture"].path, args, "texture", {{"texture_size", size}, {"label", label}});
    json summary = json::object();
    try {
      summary = read_json_file(fs::path(out).concat(L".texture.json"));
      if (!summary.is_object()) summary = json::object();
    } catch (...) {
    }
    if (r.exit_code == 0 && fs::exists(out)) {
      Texture t{out, summary.value("verdict", std::string()), summary, size, !native.empty(), native};
      say("texture " + label + ": verdict " + t.verdict);
      if (!use_views.empty() && summary.contains("views")) {
        json& pictures = pictures_by_glb_[to_utf8(out)];
        pictures = json::array();
        for (const auto& v : summary["views"])
          pictures.push_back({{"name", v.value("name", std::string())}, {"label", v.value("label", std::string())},
                               {"status", v.value("status", std::string())}, {"reason", v.value("reason", std::string())},
                               {"fit", v.value("fit", -1.0)}, {"labelled_yaw_from_front", v.value("labelled_yaw_from_front", json(nullptr))},
                               {"best_fit_anywhere", v.value("best_fit_anywhere", json(nullptr))}});
        log_.append({{"event", "pictures_verified"}, {"label", label}, {"pictures", pictures}});
      }
      log_.append({{"event", "texture"}, {"label", label}, {"glb", to_utf8(out)}, {"verdict", t.verdict},
                   {"issues", summary.value("issues", json::array())}, {"selected", summary.value("selected", "")}});
      return t;
    }
    const auto error = summary.value("error", std::string());
    const bool oom = r.memory_limit_hit || error.find("bad_alloc") != std::string::npos;
    if (!oom) {
      human("texture worker failed (exit " + std::to_string(r.exit_code) + "): " + error);
      return std::nullopt;
    }
    rec_.set_elapsed(busy());
    const auto d = rec_.on_out_of_memory("texture", size);
    record(d);
    if (d.action != Action::LowerTextureSize) return std::nullopt;
    size = d.texture_size;
  }
}

json Runner::fidelity(const fs::path& original, const fs::path& reduced, const std::string& label) {
  // In process (seconds): the same code as pixel3d-mesh-fidelity (docs/agent/FIDELITY.md).
  const auto t0 = Clock::now();
  json j = json::object();
  try {
    const auto f = measure_fidelity(fmesh_from_glb(read_glb(original)), fmesh_from_glb(read_glb(reduced)));
    j = f.to_json();
    j["judgement"] = judge_fidelity(f, spec_.fidelity);
  } catch (const std::exception& e) {
    j = {{"error", e.what()}, {"judgement", {{"status", "UNVERIFIED"}, {"reasons", json::array({e.what()})}}}};
  }
  const double sec = std::chrono::duration<double>(Clock::now() - t0).count();
  ++step_no_;
  log_.append({{"step", "fidelity"}, {"tool", "3re-agent (in process, agent_fidelity)"}, {"label", label}, {"original", to_utf8(original)},
               {"reduced", to_utf8(reduced)}, {"fidelity", j}, {"exit_code", 0}, {"seconds", sec}, {"peak_memory_mib", 0}});
  steps_.push_back({{"step", "fidelity"}, {"tool", "3re-agent (in process)"}, {"exit_code", 0}, {"seconds", sec}, {"peak_memory_mib", 0}});
  const auto& d = j.value("distance", json::object());
  char line[300];
  std::snprintf(line, sizeof line, "fidelity %s: %s (distance p95 %.3f%% max %.3f%%, IoU min %.4f, normals %.1f deg%s)", label.c_str(),
                j["judgement"].value("status", std::string("?")).c_str(), d.value("p95_pct", -1.0), d.value("max_pct", -1.0),
                j.value("silhouette_iou", json::object()).value("min", -1.0), j.value("normal_mean_deg", -1.0),
                j.contains("colour") && j["colour"].is_object()
                    ? (", colour dE " + std::to_string(j["colour"].value("block_mean_delta_e", -1.0)).substr(0, 6)).c_str()
                    : "");
  say(line);
  for (const auto& r : j["judgement"].value("reasons", json::array())) say("  - " + r.get<std::string>());
  return j;
}

void Runner::thumbnail(const fs::path& glb, double yaw, const fs::path& png, int size) {
  try {
    const int n = size;
    const auto px = render_unlit(fmesh_from_glb(read_glb(glb)), yaw, 10, n);
    pixel3d::ImageRGBA img(n, n);
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        const uint8_t* p = &px[(size_t(y) * n + x) * 4];
        img.at(x, y) = p[3] ? pixel3d::Rgba8{p[0], p[1], p[2], 255} : pixel3d::Rgba8{40, 40, 44, 255};
      }
    fs::create_directories(png.parent_path());
    write_new_file(png, pixel3d::encode_png(img));
  } catch (const std::exception& e) {
    log_.append({{"event", "thumbnail_failed"}, {"glb", to_utf8(glb)}, {"error", e.what()}});
  }
}

json Runner::renders(const fs::path& glb, const std::vector<double>& yaws, const fs::path& dir, const std::string& prefix, const std::string& group,
                     const std::string& caption) {
  json out = json::array();
  for (double yaw : yaws) {
    char name[96];
    std::snprintf(name, sizeof name, "%s-yaw%03d.png", prefix.c_str(), int(std::lround(std::fmod(yaw + 720.0, 360.0))));
    const auto png = dir / name;
    thumbnail(glb, yaw, png, 256);
    if (fs::exists(png)) out.push_back({{"path", to_utf8(png)}, {"group", group}, {"caption", caption}, {"yaw", yaw}});
  }
  return out;
}

std::optional<std::pair<Answer, size_t>> Runner::ask(Question q) {
  q.id = "q" + std::to_string(chan_.next_number());
  q.asked_at = now_iso();
  log_.append({{"event", "question"}, {"question", q.to_json()}, {"mode", opt_.decisions.mode == AskMode::Ask ? "ask" : "headless"}});
  say("QUESTION " + q.id + " — " + q.title);
  say("  " + q.why);
  for (const auto& o : q.options) say("  option " + o.id + ": " + o.label);
  if (opt_.decisions.mode == AskMode::Ask)
    say("  waiting for an answer (app run viewer, or: 3re-agent --answer \"" + to_utf8(out_) + "\" " + q.id + " <option>)");
  std::string outcome;
  double waited = 0;
  std::optional<Answer> a;
  try {
    a = chan_.ask(q, outcome, waited, [&](double s) {
      if (s < 0) say("  " + outcome + " (still waiting)");
      else log_.append({{"event", "waiting"}, {"question", q.id}, {"waited_seconds", s}});
    });
  } catch (const std::exception& e) {
    outcome = std::string("PENDING_DECISION (the question could not be asked: ") + e.what() + ")";
  }
  waited_ += waited;
  if (!a) {
    say("PENDING_DECISION " + q.id + ": " + outcome);
    log_.append({{"event", "pending_decision"}, {"question", q.id}, {"status", "PENDING_DECISION"}, {"why", outcome}, {"waited_seconds", waited}});
    pending_decisions_.push_back({{"status", "PENDING_DECISION"}, {"question", q.to_json()}, {"why", outcome}});
    return std::nullopt;
  }
  say("ANSWER " + q.id + ": " + a->choice + " — " + a->who + ", " + a->when + " (" + a->source + ", waited " + std::to_string(int(waited)) + " s)");
  log_.append({{"event", "answer"}, {"question", q.id}, {"answer", a->to_json()}, {"waited_seconds", waited}});
  human_decisions_.push_back({{"question", q.to_json()}, {"answer", a->to_json()}, {"waited_seconds", waited}});
  return std::make_pair(*a, size_t(human_decisions_.size() - 1));
}

void Runner::decided(size_t index, const std::string& outcome, json details) {
  if (index >= human_decisions_.size()) return;
  human_decisions_[index]["outcome"] = outcome;
  if (!details.empty()) human_decisions_[index]["details"] = details;
  log_.append({{"event", "decision_applied"}, {"question", human_decisions_[index]["question"]["id"]}, {"outcome", outcome}, {"details", details}});
  say("decision " + human_decisions_[index]["question"].value("id", std::string()) + " applied: " + outcome);
}

void Runner::budget_question(const Source& src, const Geometry& geo, const std::vector<DecimationCandidate>& cands, const std::vector<fs::path>& outs,
                             const json& table, size_t chosen, fs::path& mesh, bool& decimated, Geometry& geo_after, json& dec_report) {
  std::map<int, std::pair<fs::path, json>> probed;  // triangles -> (file, fidelity)
  std::map<int, json> probed_reports;
  const auto search = find_needed_triangles(spec_.triangle_budget, int(geo.triangles), [&](int t) -> std::optional<bool> {
    if (busy() > 0.5 * spec_.limits.total_seconds) return std::nullopt;
    json rep;
    const auto o = decimate(src.geometry, t, rep, "no_guard");
    if (!o) return std::nullopt;
    const int64_t got = rep.value("output_faces", int64_t(0));
    if (got > int64_t(t * 1.05)) {  // the decimator stops above this target: a higher target cannot tell more (M2 rule)
      needs_stalled_at_ = got;
      log_.append({{"event", "needs_probe_stalled"}, {"target", t}, {"reached", got}});
      return std::nullopt;
    }
    const auto f = fidelity(src.geometry, *o, "what the shape needs: " + std::to_string(t) + " triangles");
    probed[t] = {*o, f};
    probed_reports[t] = rep;
    return f["judgement"].value("status", std::string()) == "PASS";
  });
  json probes = json::array();
  for (const auto& [t, pass] : search.probes) probes.push_back({{"triangles", t}, {"pass", pass}, {"fidelity", probed[t].second["judgement"]}});
  needs_triangles_ = search.needed;
  needs_search_ = {{"needed", search.needed}, {"highest_fail", search.highest_fail}, {"stopped_early", search.stopped}, {"probes", probes},
                   {"method", "QEM without the crossing guard; 1.5x / 2x / 3x the budget, then bisection to within max(2% of the budget, 100) triangles"}};
  log_.append({{"event", "fidelity_needs"}, {"probes", probes}, {"needs_triangles", needs_triangles_}, {"search", needs_search_}});
  const bool needed_is_original = search.needed >= geo.triangles;
  if (search.needed <= 0 || (!needed_is_original && !probed.count(search.needed))) {
    human(needs_stalled_at_ > 0
              ? "no decimation reaches the budget within the fidelity limits, and QEM without the crossing guard stops at " +
                    std::to_string(needs_stalled_at_) + " triangles whatever the target (it refuses further collapses on this shape)"
              : std::string("no decimation reaches the budget within the fidelity limits, and the smallest faithful count could not be measured "
                            "(time budget or worker failure): a person must decide"));
    return;
  }
  // The decimation stops at or below its target: what is shown and delivered is the mesh's own count.
  const int needed_actual = needed_is_original ? int(geo.triangles) : probed_reports[search.needed].value("output_faces", search.needed);
  needs_triangles_ = needed_actual;
  Question q;
  q.kind = "budget_or_fidelity";
  q.title = "The triangle budget cannot be met within fidelity";
  const json at_budget = chosen != SIZE_MAX ? table[chosen] : json::object();
  std::string loss;
  for (const auto& r : at_budget.value("fidelity", json::object()).value("reasons", json::array())) loss += (loss.empty() ? "" : "; ") + r.get<std::string>();
  q.why = "At the budget of " + std::to_string(spec_.triangle_budget) + " triangles the reduced model no longer looks like the original (" +
          (loss.empty() ? std::string("fidelity FAIL") : loss) + "). The smallest count found that stays within the fidelity limits is " +
          std::to_string(needed_actual) + " triangles (" + std::to_string(int(std::lround(100.0 * needed_actual / spec_.triangle_budget))) +
          "% of the budget)" + (search.stopped ? std::string(", an upper bound: the search stopped after ") + std::to_string(search.probes.size()) + " probes" : std::string()) +
          ". The agent does not lower fidelity or raise the budget by itself.";
  q.numbers = {{"budget", spec_.triangle_budget}, {"original_triangles", geo.triangles}, {"at_budget", at_budget},
               {"needed", needed_actual}, {"needed_target", search.needed}, {"needed_is_original", needed_is_original}, {"search", needs_search_}};
  if (!needed_is_original) {
    const auto& f = probed[search.needed].second;
    q.numbers["at_needed"] = {{"triangles", needed_actual}, {"fidelity", f["judgement"]}, {"distance", f.value("distance", json::object())},
                              {"silhouette_iou_min", f.value("silhouette_iou", json::object()).value("min", -1.0)},
                              {"normal_mean_deg", f.value("normal_mean_deg", -1.0)}};
  }
  // Renders a person can compare: the original, the budget result and the needed count, front / side / back.
  const auto rdir = chan_.dir() / "q-budget";
  const std::vector<double> yaws = {0, 90, 180};
  q.renders = renders(src.geometry, yaws, rdir, "original", "original", "original, " + std::to_string(geo.triangles) + " triangles");
  if (chosen != SIZE_MAX)
    for (auto& r : renders(outs[chosen], yaws, rdir, "budget", "budget", "at the budget, " + std::to_string(cands[chosen].triangles) + " triangles (fidelity FAIL)"))
      q.renders.push_back(r);
  if (!needed_is_original)
    for (auto& r : renders(probed[search.needed].first, yaws, rdir, "needed", "needed", "needed, " + std::to_string(needed_actual) + " triangles (fidelity PASS)"))
      q.renders.push_back(r);
  if (chosen != SIZE_MAX)
    q.options.push_back({"budget", "Deliver at the budget (" + std::to_string(cands[chosen].triangles) + " triangles) with the measured loss",
                         loss.empty() ? "fidelity FAIL" : loss});
  q.options.push_back({"needed",
                       needed_is_original ? "Deliver the original mesh (" + std::to_string(geo.triangles) + " triangles, not reduced)"
                                          : "Deliver at " + std::to_string(needed_actual) + " triangles (above the budget, fidelity PASS)",
                       std::to_string(needed_actual - spec_.triangle_budget) + " triangles over the budget of " + std::to_string(spec_.triangle_budget)});
  const auto ans = ask(q);
  if (!ans) {
    human("no decimation reaches the budget within the fidelity limits; the shape keeps them at " + std::to_string(needed_actual) +
          " triangles: PENDING_DECISION (raise the budget or accept the loss)");
    return;
  }
  const auto& [a, idx] = *ans;
  const std::string by = "decided by " + a.who + " at " + a.when + " (" + human_decisions_[idx]["question"].value("id", std::string()) + ")";
  if (a.choice != "needed") {
    accepted_.push_back({"fidelity", "the measured visible loss was accepted: " + by});
    decided(idx, "delivered at the budget (" + std::to_string(geo_after.triangles) + " triangles) with the measured loss", {{"triangles", geo_after.triangles}});
    return;
  }
  Geometry g = needed_is_original ? geo : inspect(probed[search.needed].first, "decimated-needed");
  if (g.game_verdict == "UNSAFE") {
    decided(idx, "not applied: the mesh at " + std::to_string(search.needed) + " triangles is unsound; the budget result is delivered");
    human("the needed-count mesh is unsound: a person must look at it");
    return;
  }
  if (needed_is_original) {
    mesh = src.geometry;
    decimated = false;
    fidelity_geometry_ = json::object();
    dec_report = json();
  } else {
    mesh = probed[search.needed].first;
    decimated = true;
    fidelity_geometry_ = probed[search.needed].second;
    dec_report = probed_reports[search.needed];
  }
  geo_after = g;
  for (auto& row : decimation_candidates_) row["chosen"] = false;
  decimation_candidates_.push_back({{"name", "QEM without the crossing guard at the needed count (a person's decision)"}, {"ran", true},
                                    {"triangles", g.triangles}, {"budget_met", false}, {"fidelity", fidelity_geometry_.value("judgement", json::object())},
                                    {"game_geometry", g.game_verdict}, {"crossings", g.crossings}, {"chosen", true}});
  accepted_.push_back({"triangle_budget", "over the budget: " + by + "; the shape needs " + std::to_string(needed_actual) +
                                              " triangles to stay within the fidelity limits"});
  decided(idx, "delivered at " + std::to_string(g.triangles) + " triangles (budget " + std::to_string(spec_.triangle_budget) + ")", {{"triangles", g.triangles}});
}

std::optional<Texture> Runner::hidden_side_question(const fs::path& mesh, const fs::path& picture, const Texture& tex) {
  // M3 (1b): the hidden side is TRELLIS's guess (or a reconstruction) and the texture checks found a defect of a kind
  // catalogued in docs/agent/TEXTURE-DIAGNOSIS.md: ask for a picture of the back or a side; with one, the existing
  // multi-angle path paints from it, the checks run again and both results are shown.
  const auto a0 = assess_hidden_side(tex.summary, tex.hybrid);
  hidden_side_ = {{"before", a0.to_json()}};
  log_.append({{"event", "hidden_side"}, {"assessment", a0.to_json()}, {"texture", to_utf8(tex.glb)}, {"verdict", tex.verdict}});
  if (!a0.weak) {
    say(std::string("hidden side: ") + (a0.inferred ? "inferred, no catalogued defect measured" : "seen by the pictures"));
    return std::nullopt;
  }
  const double yaw = tex.summary.value("registration", json::object()).value("yaw", 0.0);
  const std::vector<double> yaws = {yaw + 180, yaw + 135, yaw + 225};
  const auto rdir = chan_.dir() / "q-hidden-side";
  Question q;
  q.kind = "hidden_side_picture";
  q.title = "The hidden side is weak: a picture of the back or a side would fix it";
  std::string found;
  for (const auto& f : a0.findings) found += (found.empty() ? "" : "; ") + f.kind + " — " + f.catalogue;
  q.why = std::string("No picture shows the back: it is ") + (tex.hybrid ? "TRELLIS's own texture (model-inferred)" : "reconstructed from the picture's edges") +
          ", and the texture checks found: " + found + ". A picture of the back (or a side) lets the agent paint it from a real view with the "
          "multi-angle path and check it again; without one it can only deliver the guess.";
  q.numbers = {{"texture_verdict", tex.verdict}, {"assessment", a0.to_json()}, {"issues", tex.summary.value("issues", json::array())}};
  q.renders = renders(tex.glb, yaws, rdir, "before", "before", "delivered texture, hidden side");
  q.options.push_back({"picture", "Use my picture of the back or a side", "the agent re-textures with it (multi-angle) and shows before / after", true});
  q.options.push_back({"keep", "Deliver the inferred back as it is", "texture stays " + tex.verdict + " with the findings above"});
  const auto ans = ask(q);
  if (!ans) {
    human("hidden side: " + found + ": PENDING_DECISION (a picture of the back or a side would let the agent repaint it)");
    return std::nullopt;
  }
  const auto& [a, idx] = *ans;
  const std::string by = "decided by " + a.who + " at " + a.when + " (" + human_decisions_[idx]["question"].value("id", std::string()) + ")";
  if (a.choice != "picture") {
    if (a0.other_issues.empty()) accepted_.push_back({"texture", "the inferred hidden side was accepted as it is: " + by});
    decided(idx, "the inferred hidden side is delivered as it is");
    return std::nullopt;
  }
  // The worker reads PNG: convert whatever the person gave (JPEG, WebP, ...) once, keep the original path in the record.
  fs::path png = work_ / (std::string("view-") + a.label + ".png");
  try {
    const auto file = pixel3d::path_from_utf8(a.picture);
    auto bytes = pixel3d::read_file(file, 256u << 20);
    if (!bytes) throw std::runtime_error("cannot read the picture");
    auto img = pixel3d::decode_image(*bytes, to_utf8(file.filename()));
    if (!img) throw std::runtime_error("not a picture the agent can read");
    write_new_file(png, pixel3d::encode_png(*img));
  } catch (const std::exception& e) {
    decided(idx, std::string("not applied: ") + e.what());
    human(std::string("the given picture could not be used: ") + e.what());
    return std::nullopt;
  }
  auto t2 = texture(mesh, picture, tex.native, tex.size, "with-" + a.label, {{png, a.label}});
  if (!t2) {
    decided(idx, "not applied: the texture worker failed with the picture");
    return std::nullopt;
  }
  const auto a1 = assess_hidden_side(t2->summary, t2->hybrid);
  const auto views = t2->summary.value("views", json::array());
  bool used = false;
  std::string view_reason;
  for (const auto& v : views) {
    if (v.value("status", std::string()) == "accepted") used = true;
    else view_reason = v.value("reason", std::string());
  }
  json after = renders(t2->glb, yaws, rdir, "after", "after", "with the " + a.label + " picture");
  const bool better = used && verdict_rank(t2->verdict) <= verdict_rank(tex.verdict);
  hidden_side_["after"] = a1.to_json();
  hidden_side_["picture"] = {{"file", a.picture}, {"label", a.label}, {"used_by_worker", used}, {"views", views}};
  json details = {{"before", {{"verdict", tex.verdict}, {"findings", a0.to_json()["findings"]}, {"renders", q.renders}, {"glb", to_utf8(tex.glb)}}},
                  {"after", {{"verdict", t2->verdict}, {"findings", a1.to_json()["findings"]}, {"renders", after}, {"glb", to_utf8(t2->glb)},
                             {"views", views}}},
                  {"kept", better ? "after" : "before"}};
  if (!better) {
    decided(idx, used ? "picture used, but the checks rate the result worse (" + t2->verdict + " vs " + tex.verdict + "): the first texture is kept"
                      : "the worker did not accept the picture (" + view_reason + "): the first texture is kept",
            details);
    human(used ? "the texture with the given picture is rated worse: compare before / after" : "the given picture was rejected: " + view_reason);
    return std::nullopt;
  }
  decided(idx, "re-textured with the " + a.label + " picture: texture " + tex.verdict + " -> " + t2->verdict + ", hidden-side findings " +
                   std::to_string(a0.findings.size()) + " -> " + std::to_string(a1.findings.size()),
          details);
  return t2;
}

Texture Runner::texture_candidates(const Source& src, const fs::path& mesh, const fs::path& picture, Texture own) {
  // B2: texture-only TRELLIS replays (engine fork) with other seeds on the same geometry, each re-textured and
  // judged; agent_select.hpp decides. Every candidate is logged with its numbers and a back thumbnail.
  const int n = spec_.texture.candidates;
  if (n <= 1) return own;
  const fs::path session = !spec_.texture_session.empty() ? spec_.texture_session : src.run.empty() ? fs::path() : src.run / "texture.session";
  const fs::path input = src.run.empty() ? fs::path() : src.run / "input.png";
  std::string skip;
  if (src.native.empty()) skip = "no TRELLIS texture volume (native.post) for this mesh";
  else if (session.empty() || !fs::exists(session)) skip = "no texture session for this run (written by the texture-candidates engine; the generation engine did not write one)";
  else if (input.empty() || !fs::exists(input)) skip = "the run has no input.png for the engine";
  else if (!caps_.has("texture_replay")) skip = "the texture replay engine is missing or not verified";
  else if (!caps_.gpu_present || caps_.vram_mib < 2048) skip = "needs a GPU with at least 2 GiB";
  else if (spec_.limits.vram_mib && spec_.limits.vram_mib < 1536) skip = "limits.vram_mib is below the ~1.2 GiB a texture replay needs";
  if (!skip.empty()) {
    say("texture candidates: skipped — " + skip);
    log_.append({{"event", "texture_candidates_skipped"}, {"reason", skip}});
    return own;
  }
  const double yaw = own.summary.value("registration", json::object()).value("yaw", 0.0);
  std::vector<Texture> textures{own};
  std::vector<TextureCandidate> cands{{"run's own", own.verdict, seen_side_agreement(own.summary), true}};
  std::vector<json> rows{{{"name", "run's own"}, {"seed", nullptr}, {"verdict", own.verdict}, {"agreement_de", cands[0].agreement_de},
                          {"glb", to_utf8(own.glb)}}};
  thumbnail(own.glb, yaw + 180, out_ / "candidates" / "own-back.png");
  rows[0]["thumbnail"] = to_utf8(out_ / "candidates" / "own-back.png");
  log_.append({{"event", "texture_candidate"}, {"candidate", rows[0]}});
  for (int seed = 1; seed < n; ++seed) {
    if (busy() > 0.8 * spec_.limits.total_seconds) {
      say("texture candidates: stopped at " + std::to_string(seed) + " of " + std::to_string(n) + " (time budget)");
      log_.append({{"event", "texture_candidates_stopped"}, {"reason", "80% of limits.total_seconds used"}, {"made", seed}});
      break;
    }
    const auto post = work_ / ("tex-seed" + std::to_string(seed) + "-steps" + std::to_string(spec_.texture.flow_steps) + ".post");
    const auto r = step("texture_replay", "trellis-tex-replay (TRELLIS.2 texture flow + decoder only, seed " + std::to_string(seed) + ")",
                        caps_.tools["texture_replay"].path,
                        {to_utf8(input), to_utf8(fs::path(post).concat(L".unused.glb")), "--models", to_utf8(caps_.models_dir), "--model", "trellis",
                         "--backend", "CUDA", "--gpu", "0", "--res", "512", "--steps", "12", "--tex-steps", std::to_string(spec_.texture.flow_steps),
                         "--seed", std::to_string(seed), "--require-gpu", "--low-vram-4gb", "--no-fa", "--threads", "2", "--tex-replay",
                         to_utf8(session), "--post-from", to_utf8(src.native), "--dump-post", to_utf8(post)},
                        "texture_replay", {{"seed", seed}, {"flow_steps", spec_.texture.flow_steps}});
    if (r.exit_code != 0 || !fs::exists(post)) {
      rows.push_back({{"name", "seed " + std::to_string(seed)}, {"seed", seed}, {"error", "replay failed (exit " + std::to_string(r.exit_code) + ")"}});
      continue;
    }
    auto t = texture(mesh, picture, post, own.size, "candidate-seed" + std::to_string(seed));
    if (!t) {
      rows.push_back({{"name", "seed " + std::to_string(seed)}, {"seed", seed}, {"error", "the texture worker failed"}});
      continue;
    }
    const auto png = out_ / "candidates" / ("seed" + std::to_string(seed) + "-back.png");
    thumbnail(t->glb, yaw + 180, png);
    textures.push_back(*t);
    cands.push_back({"seed " + std::to_string(seed), t->verdict, seen_side_agreement(t->summary), false});
    const auto h = t->summary.value("candidates", json::array()).empty() ? json::object()
                                                                         : t->summary["candidates"][0].value("quality", json::object()).value("hidden_side", json::object());
    rows.push_back({{"name", cands.back().name}, {"seed", seed}, {"verdict", t->verdict}, {"agreement_de", cands.back().agreement_de},
                    {"unsupported_patch_fraction", h.value("unsupported_patch_fraction", -1.0)}, {"front_copy_correlation", h.value("front_copy_correlation", -1.0)},
                    {"chroma_ratio", h.value("chroma_ratio", -1.0)}, {"replay_seconds", r.seconds}, {"glb", to_utf8(t->glb)}, {"thumbnail", to_utf8(png)}});
    log_.append({{"event", "texture_candidate"}, {"candidate", rows.back()}});
  }
  const auto c = choose_texture_candidate(cands);
  for (size_t i = 0; i < rows.size(); ++i) rows[i]["chosen"] = false;
  size_t row = 0;
  for (size_t i = 0, k = 0; i < rows.size(); ++i)
    if (!rows[i].contains("error")) {
      if (k == c.index) row = i;
      ++k;
    }
  rows[row]["chosen"] = true;
  texture_candidates_ = rows;
  log_.append({{"event", "texture_candidates"}, {"candidates", texture_candidates_}});
  record({Action::Accept, "texture_candidates", c.reason});
  return c.index < textures.size() ? textures[c.index] : own;
}

json Runner::validate(const fs::path& delivered, const GlbInfo& info) {
  json m = json::object();
  // G0 transform hygiene (docs/agent/ASSET-QUALITY.md): mirrored / non-uniform nodes, winding vs normals, tangents.
  try {
    const auto a = audit_hygiene(read_glb(delivered));
    const auto v = judge_hygiene(a, 0.02);
    m["hygiene"] = a.to_json();
    m["hygiene_status"] = v.status;
    m["hygiene_reasons"] = v.reasons;
  } catch (const std::exception& e) {
    m["hygiene_status"] = "FAIL";
    m["hygiene_reasons"] = {std::string("unreadable: ") + e.what()};
  }
  // Khronos glTF validator.
  if (caps_.has("validator")) {
    const auto r = step("khronos", "Khronos glTF-Validator", caps_.tools["validator"].path, {to_utf8(delivered), "-o"}, "validate", {}, {}, 600);
    try {
      const auto rep = json::parse(read_text(last_stdout_));
      m["khronos_errors"] = rep["issues"]["numErrors"];
      m["khronos_warnings"] = rep["issues"]["numWarnings"];
    } catch (...) {
      human("Khronos validator output unreadable (exit " + std::to_string(r.exit_code) + ")");
    }
  }
  // Independent reload (tinygltf).
  if (caps_.has("glbcheck")) {
    step("reload", "pixel3d-glbcheck (tinygltf)", caps_.tools["glbcheck"].path, {to_utf8(delivered)}, "validate", {}, {}, 600);
    try {
      const auto rep = json::parse(read_text(last_stdout_));
      // tinygltf parsed the JSON, loaded every buffer and validated the accessors. glbcheck's further
      // rules are 3re's drawing-export contract (one node, its material), not the delivered file's.
      m["reload_ok"] = rep.value("loaded", false);
      m["reload_contract_notes"] = rep.value("problems", json::array());
    } catch (...) {
      m["reload_ok"] = false;
    }
  }
  // Real Godot headless import.
  const double target = spec_.scale_metres;
  auto axis_extent = [&](const std::array<double, 3>& s) { return spec_.scale_axis == ScaleAxis::Height ? s[1] : std::max({s[0], s[1], s[2]}); };
  if (caps_.has("godot")) {
    const auto proj = work_ / "godot";
    fs::create_directories(proj);
    write_text_new(proj / "project.godot", "config_version=5\n\n[application]\nconfig/name=\"3re_agent_check\"\n");
    write_text_new(proj / "check.gd", godot_check_script());
    fs::copy_file(delivered, proj / "asset.glb");
    const auto gd = caps_.tools["godot"].path;
    const auto r1 = step("godot_import", "Godot " + caps_.tools["godot"].version + " --headless --import", gd,
                         {"--headless", "--path", to_utf8(proj), "--import"}, "godot", {}, {}, 900);
    const auto r2 = step("godot_check", "Godot --headless --script check.gd", gd, {"--headless", "--path", to_utf8(proj), "--script", "res://check.gd"},
                         "godot", {}, {}, 900);
    json scene = json::object();
    std::istringstream lines(read_text(last_stdout_));
    for (std::string line; std::getline(lines, line);)
      if (line.rfind("P3D_RESULT ", 0) == 0) {
        try {
          const auto all = json::parse(line.substr(11));
          if (all.is_object() && all.contains("asset.glb") && all["asset.glb"].is_object()) scene = all["asset.glb"];
        } catch (...) {
        }
      }
    std::error_code ec;
    bool imported = false;
    for (const auto& e : fs::directory_iterator(proj / ".godot" / "imported", ec))
      if (e.path().filename().wstring().rfind(L"asset.glb-", 0) == 0) imported = true;
    const bool ok = r1.exit_code == 0 && r2.exit_code == 0 && imported && scene.value("loaded", false) && scene.value("meshes", 0) > 0;
    m["godot_status"] = ok ? "PASS" : (r1.timed_out || r2.timed_out) ? "UNVERIFIED" : "FAIL";
    m["godot_scene"] = scene;
    if (!ok) m["godot_detail"] = "import exit " + std::to_string(r1.exit_code) + ", check exit " + std::to_string(r2.exit_code) +
                                 (imported ? "" : ", no imported scene file");
    if (ok) {
      m["godot_triangles"] = scene["triangles"];
      std::array<double, 3> size{}, pos{};
      for (int k = 0; k < 3; ++k) {
        size[k] = scene["aabb_size"][k].get<double>();
        pos[k] = scene["aabb_position"][k].get<double>();
      }
      m["scale_error_pct"] = std::abs(axis_extent(size) - target) / target * 100.0;
      m["scale_measured_by"] = "Godot (imported scene AABB)";
      m["delivered_size_m"] = size;
      m["up_axis_ok"] = std::abs(pos[1]) <= 0.01 * size[1];
      m["materials"] = scene["materials"];
    }
  } else {
    m["godot_status"] = "UNVERIFIED";
    m["scale_error_pct"] = std::abs(axis_extent(info.size()) - target) / target * 100.0;
    m["scale_measured_by"] = "3re agent (GLB accessor bounds through the node hierarchy)";
    m["delivered_size_m"] = info.size();
    m["up_axis_ok"] = std::abs(info.min[1]) <= 0.01 * info.size()[1];
  }
  return m;
}

int Runner::finish(const std::string& status, const std::string& summary, json extra) {
  json report = {{"name", spec_.name}, {"status", status}, {"summary", summary}, {"agent", "3re-agent milestone 4 (deterministic, no LLM; asks a person when it cannot decide)"},
                 {"started_elapsed_seconds", elapsed()}, {"spec", to_json(spec_)}, {"capabilities", caps_.to_json()},
                 {"plan", plan_.to_json()}, {"decisions", decisions_}, {"human_review", human_}, {"steps", steps_},
                 {"paths", {{"output", to_utf8(out_)}, {"work", to_utf8(work_)}, {"log", to_utf8(log_.file())}}}};
  json d = json::array();
  for (const auto& [path, sha] : deliverables_) d.push_back({{"path", to_utf8(path)}, {"sha256", sha}, {"bytes", fs::file_size(path)}});
  report["deliverables"] = d;
  report["human_decisions"] = human_decisions_;
  report["pending_decisions"] = pending_decisions_;
  report["decision_status"] = !pending_decisions_.empty() ? "PENDING_DECISION" : !human_decisions_.empty() ? "DECIDED" : "NONE";
  report["decisions_mode"] = opt_.decisions.mode == AskMode::Ask ? "ask" : "headless";
  report["waited_for_person_seconds"] = waited_;
  if (!needs_search_.empty()) report["needs_search"] = needs_search_;
  if (!hidden_side_.empty()) report["hidden_side"] = hidden_side_;
  if (!spec_.views.empty() || !pictures_by_glb_.empty()) {
    // The pictures of the delivered texture (not of the last texture tried).
    const std::string delivered = extra.value("texture", json::object()).value("glb", std::string());
    const auto it = pictures_by_glb_.find(delivered);
    report["pictures"] = it != pictures_by_glb_.end() ? it->second : json::array();
  }
  if (!spec_.requested_style.empty())
    report["requested_style"] = {{"style", spec_.requested_style}, {"applied", false},
                                 {"note", "recorded only: style transfer is not implemented in 3re; the delivered texture comes from the pictures"}};
  report.update(extra);
  report["elapsed_seconds"] = elapsed();
  log_.append({{"event", "finished"}, {"status", status}, {"elapsed_seconds", elapsed()}});
  write_text_new(out_ / "report.json", report.dump(1));
  write_text_new(out_ / "report.md", render_report_markdown(report));
  say("RESULT " + status + " — " + summary);
  for (const auto& p : pending_decisions_) say("PENDING_DECISION " + p["question"].value("id", std::string()) + ": " + p["question"].value("title", std::string()));
  say("report: " + to_utf8(out_ / "report.md"));
  if (status == "PASS" || status == "ACCEPTED") return 0;
  if (status == "FAIL") return 2;
  if (status == "BLOCKED") return 3;
  return 1;
}

int Runner::run() {
  fs::create_directories(work_);
  say("3re agent — " + spec_.name + " (" + to_string(spec_.kind) + " input, " + to_string(spec_.type) + ", budget " +
      std::to_string(spec_.triangle_budget) + " triangles, " + std::to_string(spec_.scale_metres) + " m)");
  log_.append({{"event", "start"}, {"spec", to_json(spec_)}});
  if (!spec_.requested_style.empty()) say("requested style \"" + spec_.requested_style + "\": recorded, not applied (style transfer is not implemented)");
  if (!spec_.views.empty() && !prepare_views()) say("some extra pictures could not be read: see the report");

  // Capabilities.
  const auto here = exe_dir();
  config_ = !opt_.config.empty() ? opt_.config : !env("THREERE_TRELLIS_CONFIG").empty() ? pixel3d::path_from_utf8(env("THREERE_TRELLIS_CONFIG")) : here / "3re.config.json";
  ProbeInputs pin;
  pin.config = config_;
  pin.app_dir = opt_.app_dir.empty() ? here : opt_.app_dir;
  pin.workers_dir = opt_.workers.empty() ? here : opt_.workers.front();
  for (size_t i = 1; i < opt_.workers.size(); ++i) pin.extra_worker_dirs.push_back(opt_.workers[i]);
  pin.extra_worker_dirs.push_back(here);
  // 2.10.0: --godot, THREERE_GODOT, the Settings path, else the usual install places.
  const auto godot = locate_godot(opt_.godot);
  // Not usable: the named path (if any) goes to the probe, which records it as not found.
  pin.godot = godot.found() ? godot.exe : opt_.godot == fs::path("none") ? fs::path() : !opt_.godot.empty() ? opt_.godot
                                                                         : pixel3d::path_from_utf8(env("THREERE_GODOT"));
  json searched = json::array();
  for (const auto& d : godot.searched) searched.push_back(to_utf8(d));
  log_.append({{"event", "godot"}, {"found", godot.found()}, {"exe", to_utf8(godot.exe)}, {"version", godot.version}, {"source", godot.source},
               {"problem", godot.problem}, {"searched", searched}});
  say("delivery check: " + describe_godot(godot));
  pin.validator = !opt_.validator.empty() ? opt_.validator : here / "gltf_validator.exe";
  pin.disk_path = out_;
  pin.hash_weights = opt_.hash_weights;
  say("probing tools and machine" + std::string(pin.hash_weights ? " (hashing TRELLIS weights)" : ""));
  caps_ = probe_capabilities(pin);
  if (!caps_.tools.count("godot")) {  // nothing found and nothing named: the pre-flight still lists it
    ToolInfo t;
    t.note = describe_godot(godot);
    caps_.tools["godot"] = t;
  }
  log_.append({{"event", "capabilities"}, {"capabilities", caps_.to_json()}});
  write_text_new(out_ / "capabilities.json", caps_.to_json().dump(1));
  say("RAM free " + std::to_string(caps_.ram_free_mib) + " MiB, GPU " + (caps_.gpu_present ? caps_.gpu_name + " " + std::to_string(caps_.vram_mib) + " MiB" : "none") +
      ", weights: " + caps_.weights_note);
  for (const auto& [k, t] : caps_.tools)
    if (!t.present || !t.verified) say("  tool " + k + ": " + (t.present ? t.note : "not found"));

  // Plan.
  const auto facts = inspect_inputs(spec_);
  plan_ = make_plan(spec_, caps_, facts);
  log_.append({{"event", "plan"}, {"plan", plan_.to_json()}});
  for (const auto& s : plan_.steps) say("plan: " + s.stage + " — " + s.reason);
  for (const auto& n : plan_.notes) say("note: " + n);
  if (plan_.blocked) {
    say("BLOCKED: " + plan_.blocked_reason);
    human(plan_.blocked_reason);
    return finish("BLOCKED", plan_.blocked_reason, json::object());
  }

  // Source mesh.
  Source src;
  bool cleanup_done = false;
  if (spec_.kind == InputKind::Picture) {
    if (!generate(spec_.seed, src)) return finish("FAIL", "generation did not produce a model", json::object());
  } else if (spec_.kind == InputKind::ReuseRun) {
    src.run = spec_.reuse_run;
    src.geometry = src.run / "geometry.glb";
    src.raw = src.run / "raw.ply";
    src.native = spec_.native_texture;
    src.cutout = plan_.texture_picture;
    src.seed = spec_.seed;
    // The wizard's textured result on the full mesh: newest textured*.glb with its report.
    fs::file_time_type newest{};
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(src.run, ec)) {
      const auto name = e.path().filename().wstring();
      if (name.rfind(L"textured", 0) == 0 && e.path().extension() == L".glb" && fs::exists(fs::path(e.path()).concat(L".texture.json")) &&
          (src.textured.empty() || e.last_write_time() > newest)) {
        newest = e.last_write_time();
        src.textured = e.path();
        src.textured_json = fs::path(e.path()).concat(L".texture.json");
      }
    }
    if (plan_.has("cleanup")) cleanup_done = cleanup(src);
  } else {
    src.geometry = spec_.glb;
    if (facts.glb_textured) src.textured = spec_.glb;
  }

  // Geometry audit + UNSAFE recovery.
  Geometry geo = inspect(src.geometry, "source");
  const bool cleanup_outdated = plan_.has("cleanup") || (!facts.run_cleanup_sha256.empty() && caps_.tools.count("post_safe") &&
                                                         facts.run_cleanup_sha256 != caps_.tools["post_safe"].expected_sha256);
  while (geo.verdict == "UNSAFE") {
    rec_.set_elapsed(busy());
    const bool can_regen = caps_.can_generate() && facts.picture_exists;
    const auto d = rec_.on_unsafe(cleanup_outdated && !cleanup_done, fs::exists(src.raw) && caps_.has("post_safe"), can_regen, src.seed);
    record(d);
    if (d.action == Action::RerunCleanup) {
      cleanup_done = true;
      if (!cleanup(src)) continue;
      geo = inspect(src.geometry, "cleaned");
    } else if (d.action == Action::Regenerate) {
      Source fresh;
      if (!generate(d.seed, fresh)) {
        src.seed = d.seed;  // the next attempt uses the following seed
        continue;
      }
      src = fresh;
      cleanup_done = true;
      geo = inspect(src.geometry, "seed" + std::to_string(d.seed));
    } else break;
  }

  // Before numbers.
  json texture_before = json::object();
  std::string tex_verdict_before;
  if (!src.textured_json.empty()) {
    try {
      texture_before = read_json_file(src.textured_json);
      tex_verdict_before = texture_before.value("verdict", std::string());
    } catch (...) {
    }
  }
  GlbInfo source_info;
  try {
    source_info = inspect_glb(read_glb(src.textured.empty() ? src.geometry : src.textured));
  } catch (...) {
  }

  // Triangle budget.
  fs::path mesh = src.geometry;
  bool decimated = false;
  json dec_report;
  Geometry geo_after = geo;
  const fs::path picture = plan_.texture_picture.empty() ? src.cutout : plan_.texture_picture;
  if (geo.triangles > spec_.triangle_budget) {
    rec_.set_elapsed(busy());
    if (spec_.kind == InputKind::Glb && plan_.texture_source == "keep" && picture.empty()) {
      if (!caps_.has("decimate_gltf")) {
        record({Action::HumanReview, "over_budget",
                std::to_string(geo.triangles) + " triangles > budget " + std::to_string(spec_.triangle_budget) +
                    ": pixel3d-decimate-gltf is missing and decimating otherwise would discard this GLB's own materials; delivered as is"});
      } else {
        // A3: UV-preserving decimation per mesh, budget shared by area, the GLB's own materials kept; same fidelity check.
        record({Action::Decimate, "over_budget",
                std::to_string(geo.triangles) + " triangles > budget " + std::to_string(spec_.triangle_budget) +
                    ": UV-preserving decimation of every mesh (budget shared by surface area), materials and textures kept",
                spec_.triangle_budget});
        const auto out = work_ / ("decimated-gltf-" + std::to_string(spec_.triangle_budget) + ".glb");
        const auto r = step("decimate", "pixel3d-decimate-gltf (meshoptimizer simplifyWithAttributes, per mesh)", caps_.tools["decimate_gltf"].path,
                            {to_utf8(src.geometry), to_utf8(out), "--target", std::to_string(spec_.triangle_budget)}, "decimate");
        if (r.exit_code == 0 && fs::exists(out)) {
          try {
            dec_report = read_json_file(fs::path(out).concat(L".decimate.json"));
          } catch (...) {
            dec_report = json::object();
          }
          auto g2 = inspect(out, "decimated-gltf");
          auto f = fidelity(src.geometry, out, "multi-mesh decimation (materials kept)");
          const bool ok = f["judgement"].value("status", std::string()) == "PASS" && g2.game_verdict != "UNSAFE";
          decimation_candidates_ = json::array({{{"name", "UV-preserving per-mesh decimation"}, {"ran", true}, {"triangles", g2.triangles},
                                                 {"budget_met", dec_report.value("budget_met", false)}, {"fidelity", f.value("judgement", json::object())},
                                                 {"game_geometry", g2.game_verdict}, {"crossings", g2.crossings}, {"chosen", true}}});
          mesh = out;
          decimated = true;
          geo_after = g2;
          fidelity_geometry_ = f;
          src.textured = out;  // the decimated file carries the GLB's own materials: it is the deliverable
          record({ok ? Action::Accept : Action::HumanReview, "decimation_choice",
                  std::string(ok ? "kept" : "kept, but a person should look at it") + ": " + std::to_string(g2.triangles) + " triangles, fidelity " +
                      f["judgement"].value("status", std::string("?"))});
          // As for 3re meshes: when it fails, how many triangles does this file keep the limits at? (not delivered)
          if (!ok)
            for (double k : {1.25, 1.5, 2.0}) {
              const int t = int(spec_.triangle_budget * k);
              if (t >= geo.triangles || elapsed() > 0.5 * spec_.limits.total_seconds) break;
              const auto probe = work_ / ("decimated-gltf-probe-" + std::to_string(t) + ".glb");
              const auto rp = step("decimate", "pixel3d-decimate-gltf (what the file needs: " + std::to_string(t) + ")", caps_.tools["decimate_gltf"].path,
                                   {to_utf8(src.geometry), to_utf8(probe), "--target", std::to_string(t)}, "decimate");
              if (rp.exit_code != 0 || !fs::exists(probe)) break;
              const auto fp = fidelity(src.geometry, probe, "what the file needs: " + std::to_string(t) + " triangles");
              if (fp["judgement"].value("status", std::string()) == "PASS") {
                needs_triangles_ = int(inspect_glb(read_glb(probe)).triangles);
                human("the file keeps the fidelity limits at about " + std::to_string(needs_triangles_) +
                      " triangles (UV-preserving decimation): raise the budget or accept the change");
                break;
              }
            }
        } else human("pixel3d-decimate-gltf failed (exit " + std::to_string(r.exit_code) + "); the GLB stays over budget");
      }
    } else if (!caps_.has("decimate")) {
      record({Action::HumanReview, "over_budget", "over budget and pixel3d-decimate-guarded is missing"});
    } else if (spec_.intersections == Intersections::Report) {
      // A2: decimation candidates for a game target, judged on budget and fidelity (docs/agent/FIDELITY.md).
      record({Action::Decimate, "over_budget",
              std::to_string(geo.triangles) + " triangles > budget " + std::to_string(spec_.triangle_budget) +
                  ": decimation candidates (guarded QEM, then QEM without the crossing guard), each judged on budget and fidelity; "
                  "self-crossings are reported, not blocking (geometry.intersections = report)",
              spec_.triangle_budget});
      std::vector<DecimationCandidate> cands;
      std::vector<fs::path> outs;
      std::vector<json> reps, fids;
      std::vector<Geometry> geos;
      auto try_one = [&](const std::string& name, const std::string& mode) {
        json rep2;
        DecimationCandidate c{name};
        Geometry g;
        json f = json::object();
        fs::path out;
        if (auto o = decimate(src.geometry, spec_.triangle_budget, rep2, mode)) {
          out = *o;
          c.ran = true;
          g = inspect(out, "decimated-" + mode);
          c.triangles = g.triangles;
          c.budget_met = g.triangles > 0 && g.triangles <= spec_.triangle_budget;
          c.crossings = g.crossings;
          f = fidelity(src.geometry, out, name);
          c.fidelity_pass = f["judgement"].value("status", std::string()) == "PASS" && g.game_verdict != "UNSAFE";
          c.distance_p95_pct = f.value("distance", json::object()).value("p95_pct", 1e9);
        }
        cands.push_back(c);
        outs.push_back(out);
        reps.push_back(rep2);
        fids.push_back(f);
        geos.push_back(g);
        // Thumbnail (front, flat shaded) of every candidate, for the run viewer.
        const auto thumb = out_ / "candidates" / ("decimation-" + mode + ".png");
        if (c.ran) thumbnail(out, 0, thumb);
        log_.append({{"event", "decimation_candidate"}, {"name", name}, {"mode", mode}, {"ran", c.ran}, {"triangles", c.triangles},
                     {"budget_met", c.budget_met}, {"fidelity", f.value("judgement", json::object())}, {"game_geometry", g.game_verdict},
                     {"crossings", c.crossings}, {"distance", f.value("distance", json::object())},
                     {"silhouette_iou_min", f.value("silhouette_iou", json::object()).value("min", -1.0)},
                     {"thumbnail", fs::exists(thumb) ? json(to_utf8(thumb)) : json(nullptr)}});
      };
      thumbnail(src.geometry, 0, out_ / "candidates" / "decimation-original.png");
      log_.append({{"event", "decimation_original"}, {"triangles", geo.triangles}, {"budget", spec_.triangle_budget},
                   {"thumbnail", to_utf8(out_ / "candidates" / "decimation-original.png")}});
      try_one("guarded QEM", "guarded");
      if (!(cands[0].budget_met && cands[0].fidelity_pass)) try_one("QEM without the crossing guard", "no_guard");
      const auto choice = choose_decimation(cands, Intersections::Report);
      json table = json::array();
      for (size_t i = 0; i < cands.size(); ++i)
        table.push_back({{"name", cands[i].name}, {"ran", cands[i].ran}, {"triangles", cands[i].triangles}, {"budget_met", cands[i].budget_met},
                         {"fidelity", fids[i].value("judgement", json::object())}, {"distance", fids[i].value("distance", json::object())},
                         {"silhouette_iou_min", fids[i].value("silhouette_iou", json::object()).value("min", -1.0)},
                         {"normal_mean_deg", fids[i].value("normal_mean_deg", -1.0)}, {"game_geometry", geos[i].game_verdict},
                         {"crossings", cands[i].crossings}, {"chosen", i == choice.index}});
      decimation_candidates_ = table;
      log_.append({{"event", "decimation_candidates"}, {"candidates", table}});
      record({choice.index == SIZE_MAX ? Action::HumanReview : Action::Accept, "decimation_choice", choice.reason});
      if (choice.index != SIZE_MAX) {
        mesh = outs[choice.index];
        decimated = true;
        geo_after = geos[choice.index];
        dec_report = reps[choice.index];
        fidelity_geometry_ = fids[choice.index];
      } else human("the decimation worker failed; the mesh stays over budget");
      // M3 (1a): nothing met budget and fidelity. Measure the smallest triangle count that keeps fidelity (QEM
      // without the guard: ladder, then bisection), then ask a person: the budget with the measured visible loss,
      // or the count the shape needs. Headless: PENDING_DECISION, the measured result above stays.
      if (!std::any_of(cands.begin(), cands.end(), [](const DecimationCandidate& c) { return c.budget_met && c.fidelity_pass; }))
        budget_question(src, geo, cands, outs, table, choice.index, mesh, decimated, geo_after, dec_report);
    } else {
      const auto d = rec_.on_over_budget(int(geo.triangles), false);
      record(d);
      if (d.action == Action::Decimate) {
        if (auto out = decimate(src.geometry, d.target_triangles, dec_report)) {
          mesh = *out;
          decimated = true;
          geo_after = inspect(mesh, "decimated");
          if (!dec_report.value("budget_met", false)) {
            rec_.set_elapsed(busy());
            const auto d2 = rec_.on_over_budget(int(geo_after.triangles), true);
            record(d2);
            json touching_report;
            if (d2.action == Action::DecimateAllowTouching)
              if (auto out2 = decimate(src.geometry, d2.target_triangles, touching_report, "touching")) {
                auto g2 = inspect(*out2, "decimated-touching");
                // Keep it only when it is smaller and the audit does not rate it worse.
                const auto& allowed = spec_.acceptance.geometry_allowed;
                if (g2.triangles < geo_after.triangles && !g2.verdict.empty() &&
                    (verdict_rank(g2.verdict) <= verdict_rank(geo_after.verdict) ||
                     std::find(allowed.begin(), allowed.end(), g2.verdict) != allowed.end())) {
                  log_.append({{"event", "decimation_choice"}, {"kept", "touching"}, {"triangles", g2.triangles}, {"verdict", g2.verdict}});
                  mesh = *out2;
                  geo_after = g2;
                  dec_report = touching_report;
                } else log_.append({{"event", "decimation_choice"}, {"kept", "strict"}, {"reason", "the touching pass was not smaller or its audit is not accepted by the spec"}});
                if (!touching_report.value("budget_met", false) || mesh != *out2) record(rec_.on_over_budget(int(geo_after.triangles), true));
              }
          }
        } else human("the decimation worker failed; the mesh stays over budget");
      }
    }
  }

  // Texture.
  std::optional<Texture> tex;
  std::string tex_verdict;
  const bool foreign_keep = spec_.kind == InputKind::Glb && plan_.texture_source == "keep";
  fs::path textured = ((!decimated || foreign_keep) && !src.textured.empty()) ? src.textured : fs::path();
  if (!textured.empty()) tex_verdict = tex_verdict_before;
  const fs::path native = plan_.texture_source == "hybrid" ? src.native : fs::path();
  if ((decimated || src.textured.empty()) && plan_.texture_source != "keep" && !picture.empty()) {
    tex = texture(mesh, picture, native, spec_.texture_size, decimated ? "decimated" : "source");
    if (tex && native == src.native && !native.empty()) tex = texture_candidates(src, mesh, picture, *tex);
    // A1 appearance: the original mesh textured exactly as the delivered texture (same picture, same TRELLIS volume:
    // the chosen candidate's) vs the delivered re-texture — the effect of decimation alone.
    if (tex && decimated && spec_.intersections == Intersections::Report) {
      if (busy() < 0.7 * spec_.limits.total_seconds) {
        if (auto ref = texture(src.geometry, picture, tex->native, tex->size, "original-reference"))
          fidelity_appearance_ = fidelity(ref->glb, tex->glb, "appearance (original vs reduced, the delivered texture's source)");
      } else log_.append({{"event", "appearance_fidelity_skipped"}, {"reason", "time budget"}});
    }
    if (tex && decimated) {
      rec_.set_elapsed(busy());
      const auto before = tex_verdict_before.empty() ? tex->verdict : tex_verdict_before;
      const auto d = rec_.on_texture_compared(before, tex->verdict, int(geo.triangles), int(geo_after.triangles));
      record(d);
      if (d.action == Action::DecimateGentler && spec_.intersections == Intersections::Block) {
        json gentle_report;
        if (auto out = decimate(src.geometry, d.target_triangles, gentle_report)) {
          auto g2 = inspect(*out, "gentler");
          auto t2 = texture(*out, picture, native, spec_.texture_size, "gentler");
          if (t2) {
            record(rec_.on_texture_compared(before, t2->verdict, int(geo.triangles)));
            if (verdict_rank(t2->verdict) < verdict_rank(tex->verdict)) {
              tex = t2;
              mesh = *out;
              geo_after = g2;
              dec_report = gentle_report;
            }
          }
        }
      }
    }
    if (tex && !std::count(spec_.acceptance.texture_allowed.begin(), spec_.acceptance.texture_allowed.end(), tex->verdict) &&
        tex->verdict == "REVIEW") {
      rec_.set_elapsed(busy());
      const auto d = rec_.on_texture_review(tex->hybrid);
      record(d);
      if (d.action == Action::AlternativeTexture) {
        auto alt = texture(mesh, picture, fs::path(), tex->size, "alternative");
        if (alt && verdict_rank(alt->verdict) < verdict_rank(tex->verdict)) {
          log_.append({{"event", "texture_choice"}, {"kept", "alternative"}, {"reason", "better verdict " + alt->verdict + " vs " + tex->verdict}});
          tex = alt;
        } else log_.append({{"event", "texture_choice"}, {"kept", "first"}, {"reason", "the alternative was not better"}});
        if (tex->verdict == "REVIEW") record(rec_.on_texture_review(true));
      }
    }
    if (tex) {
      if (auto with_view = hidden_side_question(mesh, picture, *tex)) {
        // The appearance reference must be painted the same way (same pictures), or the colour comparison measures
        // the new picture instead of the decimation.
        if (decimated && !fidelity_appearance_.empty() && busy() < 0.8 * spec_.limits.total_seconds) {
          std::vector<std::pair<fs::path, std::string>> views;
          for (const auto& v : hidden_side_["picture"].value("views", json::array()))
            if (v.value("status", std::string()) == "accepted") views.push_back({work_ / (std::string("view-") + hidden_side_["picture"].value("label", std::string()) + ".png"),
                                                                                hidden_side_["picture"].value("label", std::string())});
          if (auto ref = texture(src.geometry, picture, with_view->native, with_view->size, "original-reference-with-view", views))
            fidelity_appearance_ = fidelity(ref->glb, with_view->glb, "appearance (original vs reduced, both with the given picture)");
        }
        tex = with_view;
      }
      textured = tex->glb;
      tex_verdict = tex->verdict;
    }
  } else if (decimated && plan_.texture_source != "keep") {
    human("the decimated mesh has no texture source (no cutout picture): delivered without texture");
  }
  if (spec_.kind == InputKind::Glb && plan_.texture_source == "keep" && !decimated) tex_verdict = "";

  // Deliver.
  const fs::path from = textured.empty() ? mesh : textured;
  const auto delivered = out_ / (spec_.name + ".glb");
  GlbInfo delivered_info;
  try {
    const auto g = read_glb(from);
    const auto info = inspect_glb(g);
    // A4: the picture's registered front onto +Z (glTF front; Godot, Unity and Unreal importers map it to their front).
    double rot = 0;
    if (spec_.front == Front::Picture && tex) {
      const auto reg = tex->summary.value("registration", json::object());
      if (reg.contains("yaw") && reg["yaw"].is_number()) {
        rot = front_rotation_deg(reg["yaw"].get<double>());
        front_ = {{"registered_yaw_deg", reg["yaw"]}, {"registration", reg.value("choice", std::string())}, {"registration_iou", reg.value("iou", -1.0)},
                  {"rotation_y_deg", rot}, {"rule", "R_y(-registered yaw): the picture's viewing direction becomes +Z"}};
      }
    }
    if (front_.is_null()) front_ = {{"rotation_y_deg", 0}, {"why", spec_.front == Front::Keep ? "spec front = keep" : "no picture registration (no 3re texture)"}};
    auto t = delivery_transform(info, spec_.scale_axis, spec_.scale_metres);
    if (rot != 0) {
      try {
        t = delivery_transform(g, spec_.scale_axis, spec_.scale_metres, rot);
      } catch (const std::exception& e) {
        human(std::string("front rotation not applied: ") + e.what());
        front_["rotation_y_deg"] = 0;
        front_["error"] = e.what();
      }
    }
    // Baked into the vertices when possible (one node, identity transform: 3re's importer and Smart
    // Retopology open it); otherwise a scaled root node (valid glTF for Godot, not for 3re's importer).
    auto baked = with_baked_transform(g, t);
    delivery_method_ = baked ? "baked" : "root_node";
    write_new_file(delivered, encode_glb(baked ? *baked : with_root_transform(g, t, "3re_agent_root")));
    if (!baked) human("delivery uses a scaled root node (the file has its own node hierarchy): Smart Retopology / 3re's importer cannot open it");
    delivered_info = inspect_glb(read_glb(delivered));
    const auto sha = file_sha256(delivered);
    deliverables_.push_back({delivered, sha});
    ++step_no_;
    log_.append({{"step", "deliver"}, {"tool", "3re-agent (in process)"}, {"input", to_utf8(from)}, {"output", to_utf8(delivered)},
                 {"method", delivery_method_}, {"scale", t.scale}, {"translation", t.translation}, {"rotation_y_deg", t.rotation_y_deg},
                 {"source_extent", t.source_extent}, {"sha256", sha}, {"exit_code", 0},
                 {"seconds", 0.0}, {"peak_memory_mib", 0}});
    say("delivered " + to_utf8(delivered) + " (scale x" + std::to_string(t.scale) + ", grounded, " +
        (delivery_method_ == "baked" ? "baked into the vertices" : "on a root node") + ")");
  } catch (const std::exception& e) {
    human(std::string("delivery failed: ") + e.what());
    return finish("FAIL", std::string("delivery failed: ") + e.what(), json::object());
  }

  // Validate + accept.
  json measured = validate(delivered, delivered_info);
  measured["triangles"] = delivered_info.triangles;
  if (spec_.intersections == Intersections::Report && !geo_after.game_verdict.empty()) {
    measured["geometry_verdict"] = geo_after.game_verdict;
    measured["geometry_audit_verdict"] = geo_after.verdict;  // printing-grade audit, reported
    measured["self_intersections"] = geo_after.reported;
  } else if (!geo_after.verdict.empty()) measured["geometry_verdict"] = geo_after.verdict;
  if (decimated && !fidelity_geometry_.empty()) {
    measured["fidelity_status"] = fidelity_geometry_.value("judgement", json::object()).value("status", std::string("UNVERIFIED"));
    json reasons = fidelity_geometry_.value("judgement", json::object()).value("reasons", json::array());
    if (!fidelity_appearance_.empty()) {
      const auto a = fidelity_appearance_.value("colour", json());
      if (a.is_object() && a.value("block_mean_delta_e", -1.0) > spec_.fidelity.max_colour_mean_de) {
        measured["fidelity_status"] = "FAIL";
        reasons.push_back("appearance: colour difference (8x8 block means) dE " + std::to_string(a.value("block_mean_delta_e", -1.0)).substr(0, 6) + " > " +
                          std::to_string(spec_.fidelity.max_colour_mean_de).substr(0, 5));
      }
      measured["fidelity_colour_block_de"] = a.is_object() ? a.value("block_mean_delta_e", -1.0) : -1.0;
    }
    measured["fidelity_reasons"] = reasons;
  }
  if (!tex_verdict.empty()) measured["texture_verdict"] = tex_verdict;
  else if (spec_.kind == InputKind::Glb && plan_.texture_source == "keep") measured["texture_verdict"] = nullptr;
  if (decimated && spec_.intersections == Intersections::Block) measured["deviation_p99_pct"] = dec_report["deviation"]["p99_pct"];
  auto criteria = evaluate_acceptance(spec_, measured);
  apply_human_acceptance(criteria, accepted_);  // M3: a person's answer, with who and when; numbers kept
  const auto status = overall_status(criteria);
  for (const auto& c : criteria)
    if (c.status != "PASS") human(c.name + " " + c.status + ": " + c.detail);
  if (spec_.kind == InputKind::Glb && plan_.texture_source == "keep")
    human("texture of a GLB from another generator is kept as delivered: the 3re texture checks need the source picture (not measured)");
  if (front_.value("rotation_y_deg", 0.0) == 0.0 && !front_.contains("registered_yaw_deg"))
    human("front direction: no picture registration, the generator's axes are kept: confirm the front in the engine");

  json before_after = {
      {"triangles", {{"before", geo.triangles}, {"after", delivered_info.triangles}}},
      {"geometry_verdict", {{"before", geo.verdict.empty() ? json(nullptr) : json(geo.verdict)},
                            {"after", geo_after.verdict.empty() ? json(nullptr) : json(geo_after.verdict)}}},
      {"texture_verdict", {{"before", tex_verdict_before.empty() ? json(nullptr) : json(tex_verdict_before)},
                           {"after", tex_verdict.empty() ? json(nullptr) : json(tex_verdict)}}},
      {"size", {{"before", source_info.size()}, {"after", measured.value("delivered_size_m", json(nullptr))}}}};
  json extra = {{"delivery", {{"method", delivery_method_}, {"front", front_}}}, {"measured", measured},
                {"decimation_candidates", decimation_candidates_}, {"texture_candidates", texture_candidates_},
                {"fidelity", {{"geometry", fidelity_geometry_}, {"appearance", fidelity_appearance_},
                              {"needs_triangles", needs_triangles_ > 0 ? json(needs_triangles_) : json(nullptr)},
                              {"decimator_stalls_at", needs_stalled_at_ > 0 ? json(needs_stalled_at_) : json(nullptr)}}}, {"acceptance", to_json(criteria)}, {"before_after", before_after},
                {"source", {{"run", to_utf8(src.run)}, {"geometry", to_utf8(src.geometry)}, {"textured", to_utf8(src.textured)},
                            {"native", to_utf8(src.native)}, {"cutout", to_utf8(picture)}, {"seed", src.seed}}},
                {"decimation", dec_report.is_null() ? json(nullptr) : dec_report}};
  if (tex) extra["texture"] = {{"glb", to_utf8(tex->glb)}, {"verdict", tex->verdict}, {"size", tex->size}, {"hybrid", tex->hybrid},
                               {"issues", tex->summary.value("issues", json::array())}};
  const std::string geo_word = measured.value("geometry_verdict", std::string("not measured")) +
                               (spec_.intersections == Intersections::Report && geo_after.crossings > 0
                                    ? " (" + std::to_string(geo_after.crossings) + " self-crossings reported)" : std::string());
  std::string summary = std::to_string(delivered_info.triangles) + " triangles (budget " + std::to_string(spec_.triangle_budget) + "), geometry " +
                        geo_word + (measured.contains("fidelity_status") ? ", fidelity " + measured["fidelity_status"].get<std::string>() : std::string()) +
                        ", texture " +
                        (tex_verdict.empty() ? "not measured" : tex_verdict) + ", Godot " + measured.value("godot_status", std::string("?"));
  for (const auto& [name, why] : accepted_) summary += "; " + name + " accepted by a person";
  if (!pending_decisions_.empty()) summary += "; " + std::to_string(pending_decisions_.size()) + " decision(s) PENDING_DECISION";
  if (!human_decisions_.empty()) summary += "; " + std::to_string(human_decisions_.size()) + " human decision(s)";
  return finish(status, summary, extra);
}

}  // namespace

int run_agent(const AgentOptions& options) {
  auto spec = load_spec(options.spec_file);
  if (!spec.spec) {
    for (const auto& e : spec.errors) std::cerr << "spec error: " << e << "\n";
    return 4;
  }
  const fs::path out = options.out_dir.empty() ? spec.spec->output_dir : options.out_dir;
  std::error_code ec;
  if (fs::exists(out, ec) && !(fs::is_directory(out, ec) && fs::is_empty(out, ec))) {
    std::cerr << "refusing: the output folder exists and is not empty (nothing is ever overwritten): " << to_utf8(out) << "\n";
    return 4;
  }
  fs::create_directories(out, ec);
  if (ec) {
    std::cerr << "cannot create the output folder " << to_utf8(out) << ": " << ec.message() << "\n";
    return 4;
  }
  try {
    Runner r(*spec.spec, options, out);
    return r.run();
  } catch (const std::exception& e) {
    std::cerr << "agent error: " << e.what() << "\n";
    return 2;
  }
}

}  // namespace p3dagent
