#pragma once
// Godot 4 for the delivery check (2.10.0): the console executable 3re-agent imports every delivery with.
// Found in this order: an explicit path (--godot), THREERE_GODOT, the path saved in Settings, then the usual
// install places (PATH, Program Files, WinGet, Scoop, Steam, Desktop/Downloads/Documents and their Godot folders).
#include <string>
#include <vector>
#include "agent_spec.hpp"

namespace p3dagent {

struct GodotLocation {
  fs::path exe;                    // empty = no usable Godot 4
  std::string version;             // first line of `--version` ("4.7.stable.official.…")
  std::string source;              // "--godot", "THREERE_GODOT", "Settings", "found automatically"
  std::string problem;             // why a named path is not usable (empty when none was named or it works)
  std::vector<fs::path> searched;  // the folders searched automatically
  bool found() const { return !exe.empty(); }
};

// The per-user agent settings file shared by 3re, 3re-chat and 3re-agent: %APPDATA%\3re\agent-settings.json
// (THREERE_PORTABLE_USERDATA\Settings\3re\agent-settings.json for a portable profile).
fs::path agent_settings_file();
fs::path saved_godot_path();                                   // empty when none is saved
bool save_godot_path(const fs::path& exe, std::string* error);  // an empty path clears it

// `exe --version` (first line, 60 s limit); "" when it does not answer.
std::string godot_version(const fs::path& exe);
// The console build next to a GUI build (Godot_v4.7-stable_win64.exe -> Godot_v4.7-stable_win64_console.exe):
// only the console build writes the check's result to standard output. Otherwise the path itself.
fs::path godot_console_for(const fs::path& exe);
// Godot executables in the usual places, best first (console builds, then the newest version in the name).
std::vector<fs::path> godot_candidates(std::vector<fs::path>* searched = nullptr);
GodotLocation locate_godot(const fs::path& explicit_exe = {});
// One sentence for people: where Godot was found, or why it was not and how to fix it.
std::string describe_godot(const GodotLocation& g);

// The scene check script run after `--import`: prints "P3D_RESULT {json}" with, per GLB, loaded, meshes,
// triangles, materials and the world AABB.
const char* godot_check_script();

// A real headless import of a one-triangle GLB in `work_dir` (the self-test's proof that delivery verification
// works on this machine).
struct GodotSmoke {
  bool ok = false;
  std::string detail;
  double seconds = 0;
};
GodotSmoke godot_smoke_test(const fs::path& exe, const fs::path& work_dir);

}  // namespace p3dagent
