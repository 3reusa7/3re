#include "agent_godot.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include "agent_exec.hpp"
#include "agent_glb.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"

namespace p3dagent {

using nlohmann::json;

namespace {

std::wstring wenv(const wchar_t* name) {
  wchar_t buf[32768];
  const DWORD n = GetEnvironmentVariableW(name, buf, 32768);
  return n && n < 32768 ? std::wstring(buf, n) : std::wstring();
}

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = wchar_t(towlower(c));
  return s;
}

bool is_godot_exe(const fs::path& p) {
  const auto n = lower(p.filename().wstring());
  return n.rfind(L"godot", 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == L".exe" && n.find(L"export") == std::wstring::npos &&
         n.find(L"templates") == std::wstring::npos;
}

// Version digits in a file or folder name ("Godot_v4.6.3-stable" -> {4, 6, 3}); empty when none.
std::vector<int> name_version(const std::wstring& name) {
  std::vector<int> v;
  for (size_t i = 0; i < name.size(); ++i) {
    if (!iswdigit(name[i]) || (i && (iswdigit(name[i - 1]) || iswalpha(name[i - 1])) && name[i - 1] != L'v')) continue;
    size_t j = i;
    while (j < name.size() && v.size() < 4) {
      int x = 0;
      size_t k = j;
      while (k < name.size() && iswdigit(name[k])) x = x * 10 + (name[k++] - L'0');
      if (k == j) break;
      v.push_back(x);
      if (k < name.size() && name[k] == L'.' && k + 1 < name.size() && iswdigit(name[k + 1])) j = k + 1;
      else break;
    }
    if (v.size() >= 2) return v;
    v.clear();
  }
  return v;
}

json read_settings() {
  const auto bytes = pixel3d::read_file(agent_settings_file(), 1u << 20);
  if (!bytes) return json::object();
  try {
    const auto j = json::parse(bytes->begin(), bytes->end());
    return j.is_object() ? j : json::object();
  } catch (...) {
    return json::object();
  }
}

std::string read_text(const fs::path& f) {
  std::ifstream in(f, std::ios::binary);
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

}  // namespace

fs::path agent_settings_file() {
  auto portable = wenv(L"THREERE_PORTABLE_USERDATA");
  if (portable.empty()) portable = wenv(L"PIXEL3D_PORTABLE_USERDATA");
  if (!portable.empty()) return fs::path(portable) / "Settings" / "3re" / "agent-settings.json";
  auto appdata = wenv(L"APPDATA");
  if (appdata.empty()) appdata = wenv(L"USERPROFILE") + L"\\AppData\\Roaming";
  return fs::path(appdata) / "3re" / "agent-settings.json";
}

fs::path saved_godot_path() { return pixel3d::path_from_utf8(read_settings().value("godot", std::string())); }

bool save_godot_path(const fs::path& exe, std::string* error) {
  auto s = read_settings();
  if (exe.empty()) s.erase("godot");
  else s["godot"] = pixel3d::to_utf8(exe);
  const auto file = agent_settings_file();
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const auto r = pixel3d::write_file_atomic(file, s.dump(1));
  if (r.code != "ok") {
    if (error) *error = "cannot write " + pixel3d::to_utf8(file) + ": " + r.text;
    return false;
  }
  return true;
}

std::string godot_version(const fs::path& exe) {
  std::error_code ec;
  if (exe.empty() || !fs::is_regular_file(exe, ec)) return {};
  const auto dir = fs::temp_directory_path() / "3re-agent";
  fs::create_directories(dir, ec);
  const auto out = dir / ("version-" + std::to_string(GetCurrentProcessId()) + "-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
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

fs::path godot_console_for(const fs::path& exe) {
  const auto stem = exe.stem().wstring();
  if (lower(stem).find(L"console") != std::wstring::npos) return exe;
  std::error_code ec;
  const auto console = exe.parent_path() / (stem + L"_console.exe");
  return fs::is_regular_file(console, ec) ? console : exe;
}

std::vector<fs::path> godot_candidates(std::vector<fs::path>* searched) {
  std::vector<fs::path> roots;   // files directly inside, and inside their sub-folders named *godot*
  std::vector<fs::path> flat;    // files directly inside only (PATH entries, WinGet links)
  auto add = [](std::vector<fs::path>& v, const std::wstring& p) {
    if (!p.empty()) v.push_back(fs::path(p));
  };
  for (std::wstringstream path(wenv(L"PATH")); path.good();) {
    std::wstring d;
    std::getline(path, d, L';');
    add(flat, d);
  }
  const auto local = wenv(L"LOCALAPPDATA"), home = wenv(L"USERPROFILE");
  for (const auto& pf : {wenv(L"ProgramFiles"), wenv(L"ProgramFiles(x86)"), local.empty() ? std::wstring() : local + L"\\Programs"}) add(roots, pf);
  if (!local.empty()) {
    add(flat, local + L"\\Microsoft\\WinGet\\Links");
    add(roots, local + L"\\Microsoft\\WinGet\\Packages");
  }
  if (!home.empty()) {
    add(roots, home + L"\\scoop\\apps");
    for (const wchar_t* d : {L"\\Desktop", L"\\Downloads", L"\\Documents", L"\\OneDrive\\Desktop", L"\\OneDrive\\Documents", L""})
      add(roots, home + d);
  }
  const auto pf86 = wenv(L"ProgramFiles(x86)");
  if (!pf86.empty()) add(roots, pf86 + L"\\Steam\\steamapps\\common");
  add(roots, L"C:\\Godot");
  add(roots, L"C:\\Tools");

  std::vector<fs::path> found;
  std::error_code ec;
  auto files_in = [&](const fs::path& d) {
    for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
      if (is_godot_exe(it->path()) && it->is_regular_file(ec)) found.push_back(it->path());
  };
  for (const auto& d : flat) {
    if (!fs::is_directory(d, ec)) continue;
    files_in(d);
    if (searched) searched->push_back(d);
  }
  for (const auto& d : roots) {
    if (!fs::is_directory(d, ec)) continue;
    if (searched) searched->push_back(d);
    files_in(d);
    // Godot ships as a zip: <root>\Godot_v4.x\, <root>\Godot\, WinGet's GodotEngine.GodotEngine_…\, Scoop's godot\current\.
    for (fs::directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (!it->is_directory(e2) || lower(it->path().filename().wstring()).find(L"godot") == std::wstring::npos) continue;
      files_in(it->path());
      files_in(it->path() / "current");
      for (fs::directory_iterator sub(it->path(), fs::directory_options::skip_permission_denied, e2), end2; !e2 && sub != end2; sub.increment(e2)) {
        std::error_code e3;
        if (sub->is_directory(e3) && lower(sub->path().filename().wstring()).find(L"godot") != std::wstring::npos) files_in(sub->path());
      }
    }
  }
  for (auto& f : found) {
    const auto c = fs::weakly_canonical(f, ec);
    if (!ec) f = c;
  }
  std::sort(found.begin(), found.end());
  found.erase(std::unique(found.begin(), found.end()), found.end());
  auto rank = [](const fs::path& p) {
    const auto n = lower(p.filename().wstring());
    auto v = name_version(n);
    if (v.empty()) v = name_version(lower(p.parent_path().filename().wstring()));
    const bool v4 = v.empty() || v[0] == 4;
    return std::make_tuple(v4, n.find(L"console") != std::wstring::npos, v);
  };
  std::stable_sort(found.begin(), found.end(), [&](const fs::path& a, const fs::path& b) { return rank(a) > rank(b); });
  return found;
}

GodotLocation locate_godot(const fs::path& explicit_exe) {
  GodotLocation g;
  auto usable = [&](const fs::path& named, const std::string& source) {
    const auto exe = godot_console_for(named);
    std::error_code ec;
    if (!fs::is_regular_file(exe, ec)) {
      g.problem = source + " names " + pixel3d::to_utf8(named) + ", which does not exist";
      return false;
    }
    const auto v = godot_version(exe);
    if (v.rfind("4.", 0) != 0) {
      g.problem = source + " names " + pixel3d::to_utf8(exe) + ", which is not a Godot 4 console executable (--version: '" + v + "')";
      return false;
    }
    g.exe = exe;
    g.version = v;
    g.source = source;
    return true;
  };
  // A path someone named on purpose is used or reported, never silently replaced by another Godot.
  if (explicit_exe == fs::path("none")) {  // tests and diagnosis: run without Godot on purpose
    g.problem = "--godot none: the delivery check is switched off for this run";
    return g;
  }
  if (!explicit_exe.empty()) {
    usable(explicit_exe, "--godot");
    return g;
  }
  if (const auto e = wenv(L"THREERE_GODOT"); !e.empty()) {
    usable(fs::path(e), "THREERE_GODOT");
    return g;
  }
  std::string saved_problem;
  if (const auto s = saved_godot_path(); !s.empty()) {
    if (usable(s, "Settings")) return g;
    saved_problem = g.problem;
  }
  int tried = 0;
  for (const auto& c : godot_candidates(&g.searched)) {
    if (++tried > 8) break;
    if (usable(c, "found automatically")) {
      g.problem = saved_problem;
      return g;
    }
  }
  g.problem = saved_problem;
  return g;
}

std::string describe_godot(const GodotLocation& g) {
  if (g.found())
    return "Godot " + g.version + " (" + g.source + "): " + pixel3d::to_utf8(g.exe) + (g.problem.empty() ? "" : " — note: " + g.problem);
  std::string s = "Godot 4 was not found";
  if (!g.problem.empty()) s += " (" + g.problem + ")";
  s += ": the delivery cannot be imported and checked, so no asset can PASS. Install Godot 4 (godotengine.org) or choose its "
       "console executable in 3re Settings > Delivery check (Godot).";
  return s;
}

const char* godot_check_script() {
  return R"GD(extends SceneTree
func _count(n, parent_xf, acc):
	var xf = parent_xf
	if n is Node3D:
		xf = parent_xf * n.transform
	if n is MeshInstance3D and n.mesh:
		acc.meshes += 1
		for s in n.mesh.get_surface_count():
			var arr = n.mesh.surface_get_arrays(s)
			var idx = arr[Mesh.ARRAY_INDEX]
			acc.triangles += (idx.size() / 3) if idx != null else (arr[Mesh.ARRAY_VERTEX].size() / 3)
			if n.get_active_material(s) != null:
				acc.materials += 1
		var b = xf * n.get_aabb()
		if acc.has_box:
			acc.box = acc.box.merge(b)
		else:
			acc.box = b
			acc.has_box = true
	for c in n.get_children():
		_count(c, xf, acc)
func _init():
	var out = {}
	for f in DirAccess.get_files_at("res://"):
		if not f.ends_with(".glb"):
			continue
		var res = load("res://" + f)
		var acc = {"loaded": res != null, "meshes": 0, "triangles": 0, "materials": 0, "has_box": false, "box": AABB()}
		if res is PackedScene:
			var node = res.instantiate()
			_count(node, Transform3D.IDENTITY, acc)
			node.free()
		var b = acc.box
		out[f] = {"loaded": acc.loaded, "meshes": acc.meshes, "triangles": acc.triangles, "materials": acc.materials,
			"aabb_position": [b.position.x, b.position.y, b.position.z], "aabb_size": [b.size.x, b.size.y, b.size.z]}
	print("P3D_RESULT " + JSON.stringify(out))
	quit()
)GD";
}

GodotSmoke godot_smoke_test(const fs::path& exe, const fs::path& work_dir) {
  GodotSmoke s;
  const auto t0 = std::chrono::steady_clock::now();
  std::error_code ec;
  fs::remove_all(work_dir, ec);
  fs::create_directories(work_dir, ec);
  if (ec) {
    s.detail = "cannot create " + pixel3d::to_utf8(work_dir);
    return s;
  }
  // One triangle, 1 m tall, standing on y = 0.
  Glb g;
  const float v[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  g.bin.resize(sizeof v);
  std::memcpy(g.bin.data(), v, sizeof v);
  g.json = {{"asset", {{"version", "2.0"}}},
            {"scene", 0},
            {"scenes", json::array({{{"nodes", {0}}}})},
            {"nodes", json::array({{{"mesh", 0}}})},
            {"meshes", json::array({{{"primitives", json::array({{{"attributes", {{"POSITION", 0}}}}})}}})},
            {"accessors", json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"},
                                        {"min", {0, 0, 0}}, {"max", {1, 1, 0}}}})},
            {"bufferViews", json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 36}}})},
            {"buffers", json::array({{{"byteLength", 36}}})}};
  try {
    write_new_file(work_dir / "probe.glb", encode_glb(g));
  } catch (const std::exception& e) {
    s.detail = e.what();
    return s;
  }
  pixel3d::write_file_atomic(work_dir / "project.godot", std::string_view("config_version=5\n\n[application]\nconfig/name=\"3re_self_test\"\n"));
  pixel3d::write_file_atomic(work_dir / "check.gd", std::string_view(godot_check_script()));
  ProcessSpec imp;
  imp.exe = exe;
  imp.args = {"--headless", "--path", pixel3d::to_utf8(work_dir), "--import"};
  imp.timeout_seconds = 300;
  imp.stdout_file = work_dir / "import.log";
  const auto r1 = run_process(imp);
  ProcessSpec chk = imp;
  chk.args = {"--headless", "--path", pixel3d::to_utf8(work_dir), "--script", "res://check.gd"};
  chk.stdout_file = work_dir / "check.log";
  const auto r2 = run_process(chk);
  s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  json scene;
  std::istringstream lines(read_text(chk.stdout_file));
  for (std::string line; std::getline(lines, line);)
    if (line.rfind("P3D_RESULT ", 0) == 0) try {
        const auto all = json::parse(line.substr(11));
        if (all.is_object() && all.contains("probe.glb")) scene = all["probe.glb"];
      } catch (...) {
      }
  if (!r1.started || !r2.started) s.detail = "Godot did not start: " + (r1.started ? r2.error : r1.error);
  else if (r1.timed_out || r2.timed_out) s.detail = "Godot did not finish within 5 minutes";
  else if (!scene.is_object()) s.detail = "Godot ran (import exit " + std::to_string(r1.exit_code) + ", check exit " + std::to_string(r2.exit_code) +
                                          ") but printed no result";
  else if (!scene.value("loaded", false) || scene.value("triangles", 0) != 1)
    s.detail = "Godot imported the test GLB wrongly: " + scene.dump();
  else {
    s.ok = true;
    s.detail = "imported a test GLB headless: 1 triangle";
  }
  return s;
}

}  // namespace p3dagent
