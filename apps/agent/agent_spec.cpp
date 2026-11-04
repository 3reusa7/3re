#include "agent_spec.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#include "pixel3d/common/path_utf8.hpp"

namespace p3dagent {
namespace {

using nlohmann::json;

// Always absolute and normalised: a path handed to a worker can never start with '-' (an option).
fs::path resolve(const json& v, const fs::path& base) {
  const fs::path p = pixel3d::path_from_utf8(v.get<std::string>());
  std::error_code ec;
  const fs::path full = p.is_absolute() ? p : base / p;
  const fs::path abs = fs::absolute(full, ec);
  return (ec ? full : abs).lexically_normal();
}

bool reserved_device_name(std::string n) {
  for (auto& c : n) c = char(std::toupper(static_cast<unsigned char>(c)));
  if (n == "CON" || n == "PRN" || n == "AUX" || n == "NUL") return true;
  return n.size() == 4 && (n.rfind("COM", 0) == 0 || n.rfind("LPT", 0) == 0) && n[3] >= '1' && n[3] <= '9';
}

bool is_string(const json& j, const char* key) { return j.contains(key) && j[key].is_string() && !j[key].get<std::string>().empty(); }

void read_strings(const json& j, const char* key, std::vector<std::string>& out, std::vector<std::string>& errors,
                  const std::string& where) {
  if (!j.contains(key)) return;
  if (!j[key].is_array()) {
    errors.push_back(where + "." + key + " must be a list of verdicts");
    return;
  }
  out.clear();
  for (const auto& v : j[key]) {
    const auto s = v.is_string() ? v.get<std::string>() : std::string();
    if (s != "PASS" && s != "REVIEW" && s != "FAIL") errors.push_back(where + "." + key + ": unknown verdict " + v.dump());
    else out.push_back(s);
  }
}

}  // namespace

const char* to_string(InputKind k) { return k == InputKind::Picture ? "picture" : k == InputKind::Glb ? "glb" : "reuse_run"; }
const char* to_string(AssetType t) { return t == AssetType::Prop ? "prop" : "character"; }
const char* to_string(ScaleAxis a) { return a == ScaleAxis::Height ? "height" : "longest"; }

namespace {
SpecResult parse_spec_checked(const json& j, const fs::path& base);
}

SpecResult parse_spec(const json& j, const fs::path& base) {
  try {
    return parse_spec_checked(j, base);
  } catch (const json::exception& ex) {
    return {std::nullopt, {std::string("spec has a field of the wrong type: ") + ex.what()}};
  }
}

namespace {
SpecResult parse_spec_checked(const json& j, const fs::path& base) {
  SpecResult r;
  auto& e = r.errors;
  AssetSpec s;
  if (!j.is_object()) {
    e.push_back("spec must be a JSON object");
    return r;
  }
  if (j.value("schema", std::string()) != "3re.asset-spec/1") e.push_back("schema must be \"3re.asset-spec/1\"");
  s.name = j.value("name", std::string());
  if (s.name.empty() || s.name.size() > 80 || s.name.find_first_of("/\\:*?\"<>|") != std::string::npos ||
      s.name.front() == '-' || s.name.front() == '.' || s.name.back() == '.' || s.name.back() == ' ' ||
      reserved_device_name(s.name) ||
      std::any_of(s.name.begin(), s.name.end(), [](char c) { return static_cast<unsigned char>(c) < 32; }))
    e.push_back("name must be 1-80 characters without / \\ : * ? \" < > |, control characters, a leading '-' or '.', "
                "a trailing '.' or space, or a Windows device name");

  const json in = j.value("input", json::object());
  if (!in.is_object()) e.push_back("input must be an object");
  else {
    if (is_string(in, "picture")) s.picture = resolve(in["picture"], base);
    if (is_string(in, "glb")) s.glb = resolve(in["glb"], base);
    if (is_string(in, "reuse_run")) s.reuse_run = resolve(in["reuse_run"], base);
    if (is_string(in, "native_texture")) s.native_texture = resolve(in["native_texture"], base);
    if (is_string(in, "texture_session")) s.texture_session = resolve(in["texture_session"], base);
    if (in.contains("seed")) {
      if (!in["seed"].is_number_integer() || in["seed"].get<int64_t>() < 0 || in["seed"].get<int64_t>() > 2147483647)
        e.push_back("input.seed must be an integer 0..2147483647");
      else s.seed = in["seed"].get<uint32_t>();
    }
    if (in.contains("views")) {
      static const char* kLabels[] = {"front_left", "left", "back_left", "back", "back_right", "right", "front_right", "auto"};
      if (!in["views"].is_array() || in["views"].size() > 5) e.push_back("input.views must be a list of at most 5 {picture, label}");
      else
        for (const auto& v : in["views"]) {
          const std::string label = v.is_object() ? v.value("label", std::string()) : std::string();
          if (!v.is_object() || !is_string(v, "picture") ||
              std::none_of(std::begin(kLabels), std::end(kLabels), [&](const char* l) { return label == l; })) {
            e.push_back("input.views: each entry needs picture and label (front_left, left, back_left, back, back_right, right, front_right, auto)");
            break;
          }
          s.views.push_back({resolve(v["picture"], base), label});
        }
    }
    if (!s.glb.empty() && !s.reuse_run.empty()) e.push_back("input: give glb or reuse_run, not both");
    if (s.picture.empty() && s.glb.empty() && s.reuse_run.empty()) e.push_back("input needs picture, glb or reuse_run");
    s.kind = !s.reuse_run.empty() ? InputKind::ReuseRun : !s.glb.empty() ? InputKind::Glb : InputKind::Picture;
  }

  const json target = j.value("target", json::object());
  s.engine = target.is_object() ? target.value("engine", std::string("godot")) : std::string();
  if (s.engine != "godot") e.push_back("target.engine: only \"godot\" is supported in this milestone");

  const auto type = j.value("asset_type", std::string());
  if (type == "prop") s.type = AssetType::Prop;
  else if (type == "character") s.type = AssetType::Character;
  else e.push_back("asset_type must be \"prop\" or \"character\"");

  // A5 presets (docs/agent/BUDGETS.md): fill the budget and texture size the spec does not give.
  struct Preset { const char* name; int triangles, texture; };
  static const Preset kPresets[] = {{"mobile_character", 5000, 1024}, {"mobile_prop", 1500, 1024}, {"pc_character", 30000, 2048},
                                    {"pc_prop", 8000, 1024}, {"hero_character", 50000, 4096}};
  const Preset* preset = nullptr;
  if (j.contains("preset")) {
    const auto name = j["preset"].is_string() ? j["preset"].get<std::string>() : std::string();
    for (const auto& p : kPresets)
      if (name == p.name) preset = &p;
    if (!preset) e.push_back("preset must be one of mobile_character, mobile_prop, pc_character, pc_prop, hero_character");
    else s.preset = name;
  }
  if (!j.contains("triangle_budget") && preset) s.triangle_budget = preset->triangles;
  else if (!j.contains("triangle_budget") || !j["triangle_budget"].is_number_integer() ||
      j["triangle_budget"].get<int64_t>() < 100 || j["triangle_budget"].get<int64_t>() > 2000000)
    e.push_back("triangle_budget must be an integer 100..2000000");
  else s.triangle_budget = j["triangle_budget"].get<int>();

  s.texture_size = j.value("texture_size", preset ? preset->texture : 2048);
  if (s.texture_size != 1024 && s.texture_size != 2048 && s.texture_size != 4096)
    e.push_back("texture_size must be 1024, 2048 or 4096 (the texture worker's atlas sizes)");

  const json scale = j.value("scale", json::object());
  const auto axis = scale.is_object() ? scale.value("axis", std::string("height")) : std::string();
  if (axis == "height") s.scale_axis = ScaleAxis::Height;
  else if (axis == "longest") s.scale_axis = ScaleAxis::Longest;
  else e.push_back("scale.axis must be \"height\" or \"longest\"");
  if (!scale.is_object() || !scale.contains("metres") || !scale["metres"].is_number() ||
      !(scale["metres"].get<double>() >= 0.01 && scale["metres"].get<double>() <= 1000))
    e.push_back("scale.metres must be a number 0.01..1000");
  else s.scale_metres = scale["metres"].get<double>();

  if (j.contains("acceptance")) {
    const json& a = j["acceptance"];
    if (!a.is_object()) e.push_back("acceptance must be an object");
    else {
      read_strings(a, "geometry_allowed", s.acceptance.geometry_allowed, e, "acceptance");
      read_strings(a, "texture_allowed", s.acceptance.texture_allowed, e, "acceptance");
      s.acceptance.khronos_max_errors = a.value("khronos_max_errors", 0);
      s.acceptance.require_godot_import = a.value("require_godot_import", true);
      s.acceptance.max_deviation_pct = a.value("max_deviation_pct", 1.0);
      s.acceptance.max_scale_error_pct = a.value("max_scale_error_pct", 1.0);
      if (s.acceptance.khronos_max_errors < 0) e.push_back("acceptance.khronos_max_errors must be >= 0");
      if (!(s.acceptance.max_deviation_pct > 0)) e.push_back("acceptance.max_deviation_pct must be > 0");
    }
  }
  if (j.contains("limits")) {
    const json& l = j["limits"];
    if (!l.is_object()) e.push_back("limits must be an object");
    else {
      s.limits.total_seconds = l.value("total_seconds", s.limits.total_seconds);
      s.limits.step_seconds = l.value("step_seconds", s.limits.step_seconds);
      s.limits.max_regenerations = l.value("max_regenerations", s.limits.max_regenerations);
      if (l.contains("vram_mib")) {
        if (!l["vram_mib"].is_number_integer() || l["vram_mib"].get<int64_t>() < 256 || l["vram_mib"].get<int64_t>() > 262144)
          e.push_back("limits.vram_mib must be an integer 256..262144 (MiB)");
        else s.limits.vram_mib = l["vram_mib"].get<uint64_t>();
      }
      if (!(s.limits.total_seconds >= 10 && s.limits.total_seconds <= 86400)) e.push_back("limits.total_seconds must be 10..86400");
      if (!(s.limits.step_seconds >= 5 && s.limits.step_seconds <= 86400)) e.push_back("limits.step_seconds must be 5..86400");
      if (s.limits.max_regenerations < 0 || s.limits.max_regenerations > 2) e.push_back("limits.max_regenerations must be 0..2");
      if (l.contains("step_ram_mib")) {
        if (!l["step_ram_mib"].is_object()) e.push_back("limits.step_ram_mib must be an object of MiB per stage");
        else
          for (const auto& [k, v] : l["step_ram_mib"].items()) {
            if (!v.is_number_integer() || v.get<int64_t>() < 16) e.push_back("limits.step_ram_mib." + k + " must be >= 16");
            else s.limits.step_ram_mib[k] = v.get<uint64_t>();
          }
      }
    }
  }
  if (j.contains("geometry")) {
    const json& g = j["geometry"];
    const auto mode = g.is_object() ? g.value("intersections", std::string("report")) : std::string();
    if (mode == "report") s.intersections = Intersections::Report;
    else if (mode == "block") s.intersections = Intersections::Block;
    else e.push_back("geometry.intersections must be \"report\" (game engines) or \"block\" (printing, booleans)");
  }
  if (j.contains("fidelity")) {
    const json& f = j["fidelity"];
    if (!f.is_object()) e.push_back("fidelity must be an object of limits");
    else {
      auto read = [&](const char* k, double& v, double lo, double hi) {
        if (!f.contains(k)) return;
        if (!f[k].is_number() || !(f[k].get<double>() >= lo && f[k].get<double>() <= hi))
          e.push_back(std::string("fidelity.") + k + " must be a number " + std::to_string(lo).substr(0, 4) + ".." + std::to_string(hi).substr(0, 5));
        else v = f[k].get<double>();
      };
      read("max_distance_p95_pct", s.fidelity.max_distance_p95_pct, 0.001, 50);
      read("max_distance_mean_pct", s.fidelity.max_distance_mean_pct, 0.001, 50);
      read("max_distance_max_pct", s.fidelity.max_distance_max_pct, 0.001, 100);
      read("min_silhouette_iou", s.fidelity.min_silhouette_iou, 0, 1);
      read("max_normal_mean_deg", s.fidelity.max_normal_mean_deg, 0.1, 90);
      read("max_colour_mean_de", s.fidelity.max_colour_mean_de, 0.001, 1);
    }
  }
  if (j.contains("texture")) {
    const json& t = j["texture"];
    if (!t.is_object()) e.push_back("texture must be an object");
    else {
      if (t.contains("candidates")) {
        if (!t["candidates"].is_number_integer() || t["candidates"].get<int64_t>() < 1 || t["candidates"].get<int64_t>() > 8)
          e.push_back("texture.candidates must be an integer 1..8 (1 = the run's own texture only)");
        else s.texture.candidates = t["candidates"].get<int>();
      }
      if (t.contains("flow_steps")) {
        if (!t["flow_steps"].is_number_integer() || (t["flow_steps"].get<int64_t>() != 12 && t["flow_steps"].get<int64_t>() != 24))
          e.push_back("texture.flow_steps must be 12 or 24");
        else s.texture.flow_steps = t["flow_steps"].get<int>();
      }
    }
  }
  if (j.contains("front")) {
    const auto f = j["front"].is_string() ? j["front"].get<std::string>() : std::string();
    if (f == "picture") s.front = Front::Picture;
    else if (f == "keep") s.front = Front::Keep;
    else e.push_back("front must be \"picture\" (turn the picture's front onto +Z) or \"keep\"");
  }
  if (j.contains("requested_style")) {
    if (!j["requested_style"].is_string() || j["requested_style"].get<std::string>().size() > 200)
      e.push_back("requested_style must be text of at most 200 characters");
    else s.requested_style = j["requested_style"].get<std::string>();
  }
  if (is_string(j, "output_dir")) s.output_dir = resolve(j["output_dir"], base);
  else e.push_back("output_dir is required (a new or empty folder; nothing is ever overwritten)");

  if (e.empty()) r.spec = s;
  return r;
}
}  // namespace

SpecResult load_spec(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return {std::nullopt, {"cannot read spec file " + pixel3d::to_utf8(file)}};
  std::stringstream ss;
  ss << in.rdbuf();
  json j;
  try {
    j = json::parse(ss.str());
  } catch (const std::exception& ex) {
    return {std::nullopt, {std::string("spec is not valid JSON: ") + ex.what()}};
  }
  std::error_code ec;
  const auto abs = fs::absolute(file, ec);
  return parse_spec(j, (ec ? file : abs).parent_path());
}

json to_json(const AssetSpec& s) {
  auto p = [](const fs::path& x) { return x.empty() ? json(nullptr) : json(pixel3d::to_utf8(x)); };
  json ram = json::object();
  for (const auto& [k, v] : s.limits.step_ram_mib) ram[k] = v;
  json views = json::array();
  for (const auto& v : s.views) views.push_back({{"picture", pixel3d::to_utf8(v.picture)}, {"label", v.label}});
  return {{"name", s.name},
          {"input", {{"kind", to_string(s.kind)}, {"picture", p(s.picture)}, {"glb", p(s.glb)}, {"reuse_run", p(s.reuse_run)},
                     {"native_texture", p(s.native_texture)}, {"texture_session", p(s.texture_session)}, {"seed", s.seed},
                     {"views", views}}},
          {"requested_style", s.requested_style.empty() ? json(nullptr) : json(s.requested_style)},
          {"target", {{"engine", s.engine}}},
          {"asset_type", to_string(s.type)},
          {"triangle_budget", s.triangle_budget},
          {"texture_size", s.texture_size},
          {"scale", {{"axis", to_string(s.scale_axis)}, {"metres", s.scale_metres}}},
          {"acceptance", {{"geometry_allowed", s.acceptance.geometry_allowed}, {"texture_allowed", s.acceptance.texture_allowed},
                          {"khronos_max_errors", s.acceptance.khronos_max_errors},
                          {"require_godot_import", s.acceptance.require_godot_import},
                          {"max_deviation_pct", s.acceptance.max_deviation_pct},
                          {"max_scale_error_pct", s.acceptance.max_scale_error_pct}}},
          {"limits", {{"total_seconds", s.limits.total_seconds}, {"step_seconds", s.limits.step_seconds},
                      {"step_ram_mib", ram}, {"max_regenerations", s.limits.max_regenerations}, {"vram_mib", s.limits.vram_mib}}},
          {"geometry", {{"intersections", s.intersections == Intersections::Report ? "report" : "block"}}},
          {"fidelity", {{"max_distance_p95_pct", s.fidelity.max_distance_p95_pct}, {"max_distance_mean_pct", s.fidelity.max_distance_mean_pct},
                        {"max_distance_max_pct", s.fidelity.max_distance_max_pct}, {"min_silhouette_iou", s.fidelity.min_silhouette_iou},
                        {"max_normal_mean_deg", s.fidelity.max_normal_mean_deg}, {"max_colour_mean_de", s.fidelity.max_colour_mean_de}}},
          {"texture", {{"candidates", s.texture.candidates}, {"flow_steps", s.texture.flow_steps}}},
          {"front", s.front == Front::Picture ? "picture" : "keep"},
          {"preset", s.preset.empty() ? json(nullptr) : json(s.preset)},
          {"output_dir", p(s.output_dir)}};
}

}  // namespace p3dagent
