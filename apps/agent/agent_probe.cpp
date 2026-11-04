#include "agent_probe.hpp"

#include <windows.h>

#include <fstream>
#include <sstream>

#include "agent_exec.hpp"
#include "agent_glb.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/image.hpp"

namespace p3dagent {

using nlohmann::json;

namespace {

json read_json(const fs::path& file, uint64_t max_bytes = 16u << 20) {
  auto bytes = pixel3d::read_file(file, max_bytes);
  if (!bytes) throw std::runtime_error("cannot read " + pixel3d::to_utf8(file));
  return json::parse(bytes->begin(), bytes->end());
}

fs::path rel(const fs::path& base, const json& block, const char* key) {
  const auto v = block.value(key, std::string());
  if (v.empty()) return {};
  const auto p = pixel3d::path_from_utf8(v);
  return (p.is_absolute() ? p : base / p).lexically_normal();
}

ToolInfo tool(const fs::path& path, const std::string& expected = {}) {
  ToolInfo t;
  t.path = path;
  std::error_code ec;
  t.present = !path.empty() && fs::is_regular_file(path, ec);
  if (!t.present) {
    t.note = "not found";
    return t;
  }
  t.expected_sha256 = expected;
  t.sha256 = file_sha256(path);
  t.verified = expected.empty() || t.sha256 == expected;
  if (!t.verified) t.note = "SHA-256 does not match the pin";
  else if (expected.empty()) t.note = "no pin (hash recorded)";
  return t;
}

// `exe --version` (first line), with a short time limit.
std::string version_of(const fs::path& exe) {
  const auto dir = fs::temp_directory_path() / "3re-agent";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const auto out = dir / ("version-" + std::to_string(GetCurrentProcessId()) + ".txt");
  fs::remove(out, ec);
  ProcessSpec p;
  p.exe = exe;
  p.args = {"--version"};
  p.timeout_seconds = 60;
  p.stdout_file = out;
  const auto r = run_process(p);
  std::ifstream in(out);
  std::string line;
  std::getline(in, line);
  in.close();
  fs::remove(out, ec);
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  return r.exit_code == 0 ? line : std::string();
}

}  // namespace

Capabilities probe_capabilities(const ProbeInputs& in) {
  Capabilities c;
  const auto sys = probe_system(in.disk_path);
  c.ram_total_mib = sys.ram_total_mib;
  c.ram_free_mib = sys.ram_free_mib;
  c.disk_free_mib = sys.disk_free_mib;
  c.vram_mib = sys.vram_mib;
  c.gpu_name = sys.gpu_name;
  c.gpu_present = sys.gpu_present;

  json trellis = json::object();
  fs::path base = in.config.parent_path();
  try {
    trellis = read_json(in.config).value("trellis", json::object());
  } catch (const std::exception& e) {
    c.weights_note = std::string("3re config unreadable: ") + e.what();
  }
  const auto runtime = rel(base, trellis, "runtime_dir");
  c.tools["engine"] = tool(runtime.empty() ? fs::path() : runtime / "trellis-cli.exe", trellis.value("engine_sha256", std::string()));
  c.tools["post_safe"] = tool(rel(base, trellis, "post_executable"), trellis.value("post_sha256", std::string()));
  for (const auto& [key, pin] : {std::pair<const char*, const char*>{"engine", "engine_sha256"}, {"post_safe", "post_sha256"}})
    if (c.tools[key].present && trellis.value(pin, std::string()).empty()) {
      c.tools[key].verified = false;  // both are always pinned in a 3re config
      c.tools[key].note = std::string("the config has no ") + pin;
    }
  // Texture-only TRELLIS replays (milestone 2, B2): the engine fork next to the pinned engine, pinned the same way.
  c.tools["texture_replay"] = tool(runtime.empty() ? fs::path() : runtime / "trellis-tex-replay.exe", trellis.value("texture_replay_sha256", std::string()));
  if (c.tools["texture_replay"].present && trellis.value("texture_replay_sha256", std::string()).empty()) {
    c.tools["texture_replay"].verified = false;
    c.tools["texture_replay"].note = "the config has no texture_replay_sha256";
  }
  c.models_dir = rel(base, trellis, "models_dir");
  c.tools["studio"] = tool(in.app_dir / "3re-studio.exe");
  for (const auto& [key, exe] : std::vector<std::pair<std::string, std::string>>{
           {"auto_texture", "pixel3d-auto-texture.exe"}, {"inspect", "pixel3d-asset-inspect.exe"},
           {"audit", "pixel3d-geometry-audit.exe"}, {"decimate", "pixel3d-decimate-guarded.exe"},
           {"decimate_gltf", "pixel3d-decimate-gltf.exe"},
           {"glbcheck", "pixel3d-glbcheck.exe"}})
  {
    fs::path found = in.workers_dir / exe;
    std::error_code ec;
    for (const auto& d : in.extra_worker_dirs)
      if (!fs::is_regular_file(found, ec) && fs::is_regular_file(d / exe, ec)) found = d / exe;
    c.tools[key] = tool(found);
  }
  if (!in.validator.empty()) c.tools["validator"] = tool(in.validator);
  if (!in.godot.empty()) {
    auto g = tool(in.godot);
    if (g.present) {
      g.version = version_of(in.godot);
      if (g.version.rfind("4.", 0) != 0) {
        g.verified = false;
        g.note = "not a Godot 4 console executable (--version: '" + g.version + "')";
      }
    }
    c.tools["godot"] = g;
  }

  // Weights: size + SHA-256 of every manifest file (the LFS oid), as tools/stores.ps1 does.
  const auto manifest_file = rel(base, trellis, "weights_manifest");
  const auto models = rel(base, trellis, "models_dir");
  if (manifest_file.empty() || models.empty()) {
    if (c.weights_note.empty()) c.weights_note = "the config names no weights manifest / models folder";
    return c;
  }
  try {
    const auto manifest = read_json(manifest_file);
    // Manifest entries are "q4/<file>", relative to the folder holding models_dir (models	rellis).
    const auto root = models.parent_path();
    int checked = 0;
    std::string problem;
    for (const auto& f : manifest.value("files", json::array())) {
      const auto relpath = f.value("path", std::string());
      const auto oid = f.contains("lfs") ? f["lfs"].value("oid", std::string()) : std::string();
      const auto size = f.value("size", uint64_t(0));
      const auto relp = pixel3d::path_from_utf8(relpath);
      if (relpath.empty() || relpath.find("..") != std::string::npos || relp.is_absolute() || relp.has_root_name() ||
          relp.has_root_directory()) {
        problem = "manifest entry with an unsafe path";
        break;
      }
      const auto file = root / pixel3d::path_from_utf8(relpath);
      std::error_code ec;
      if (!fs::is_regular_file(file, ec)) {
        problem = relpath + ": missing";
        break;
      }
      if (fs::file_size(file, ec) != size) {
        problem = relpath + ": size differs from the manifest";
        break;
      }
      if (in.hash_weights && file_sha256(file) != oid) {
        problem = relpath + ": SHA-256 mismatch";
        break;
      }
      ++checked;
    }
    if (problem.empty() && checked == 0) problem = "manifest lists no files";
    // Without hashing only sizes were compared: not a verification, so generation stays unavailable.
    c.weights_verified = problem.empty() && in.hash_weights;
    c.weights_note = !problem.empty() ? problem
                     : in.hash_weights ? std::to_string(checked) + " files verified (size + SHA-256)"
                                       : std::to_string(checked) + " files present with the manifest sizes (SHA-256 not checked: --no-hash-weights)";
  } catch (const std::exception& e) {
    c.weights_note = std::string("weights manifest unreadable: ") + e.what();
  }
  return c;
}

bool picture_has_alpha(const fs::path& picture) {
  auto bytes = pixel3d::read_file(picture, 256u << 20);
  if (!bytes) return false;
  auto img = pixel3d::decode_image(*bytes, pixel3d::to_utf8(picture.filename()));
  if (!img) return false;
  // Real transparency: at least 0.5 % of the pixels see-through (an RGBA file that is fully opaque has none).
  size_t clear = 0;
  for (const auto& p : img->pixels()) clear += p.a < 128;
  return clear * 200 >= img->pixels().size();
}

InputFacts inspect_inputs(const AssetSpec& spec) {
  InputFacts f;
  std::error_code ec;
  if (!spec.picture.empty() && fs::is_regular_file(spec.picture, ec)) {
    f.picture_exists = true;
    f.picture_has_alpha = picture_has_alpha(spec.picture);
  }
  if (!spec.glb.empty() && fs::is_regular_file(spec.glb, ec)) {
    f.glb_exists = true;
    try {
      f.glb_textured = inspect_glb(read_glb(spec.glb)).textured;
    } catch (...) {
    }
  }
  if (!spec.reuse_run.empty() && fs::is_directory(spec.reuse_run, ec)) {
    f.run_exists = true;
    f.run_has_geometry = fs::is_regular_file(spec.reuse_run / "geometry.glb", ec);
    f.run_has_raw_ply = fs::is_regular_file(spec.reuse_run / "raw.ply", ec);
    if (fs::is_regular_file(spec.reuse_run / "texture-source.png", ec)) f.run_cutout = spec.reuse_run / "texture-source.png";
    else if (fs::is_regular_file(spec.reuse_run / "input.png", ec) && picture_has_alpha(spec.reuse_run / "input.png"))
      f.run_cutout = spec.reuse_run / "input.png";
    f.run_has_cutout = !f.run_cutout.empty();
    f.run_has_native = !spec.native_texture.empty() && fs::is_regular_file(spec.native_texture, ec);
    try {
      const auto rep = read_json(spec.reuse_run / "report.json");
      f.run_cleanup_sha256 = rep.value("post_sha256", std::string());
      f.run_status = rep.value("status", std::string());
    } catch (...) {
    }
  }
  return f;
}

}  // namespace p3dagent
