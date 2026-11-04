#include "agent_eval.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <sstream>

#include "pixel3d/common/path_utf8.hpp"

namespace p3dagent {

using nlohmann::json;

namespace {

bool num(const json& m, const char* k) { return m.contains(k) && m[k].is_number(); }
bool str(const json& m, const char* k) { return m.contains(k) && m[k].is_string() && !m[k].get<std::string>().empty(); }
bool allowed(const std::vector<std::string>& list, const std::string& v) { return std::find(list.begin(), list.end(), v) != list.end(); }
std::string fmt(double v, int digits = 2) {
  std::ostringstream s;
  s.setf(std::ios::fixed);
  s.precision(digits);
  s << v;
  return s.str();
}

}  // namespace

std::vector<Criterion> evaluate_acceptance(const AssetSpec& spec, const json& m) {
  std::vector<Criterion> c;
  const auto unverified = [&](const char* name, const char* why) { c.push_back({name, "UNVERIFIED", why}); };

  if (num(m, "triangles")) {
    const auto t = m["triangles"].get<int64_t>();
    c.push_back({"triangle_budget", t <= spec.triangle_budget ? "PASS" : "FAIL",
                 std::to_string(t) + " triangles, budget " + std::to_string(spec.triangle_budget)});
  } else unverified("triangle_budget", "triangle count not measured");

  if (str(m, "geometry_verdict")) {
    const auto v = m["geometry_verdict"].get<std::string>();
    c.push_back({"geometry", v == "UNSAFE" || v == "FAIL" ? "FAIL" : allowed(spec.acceptance.geometry_allowed, v) ? "PASS" : "REVIEW",
                 "geometry audit " + v});
  } else unverified("geometry", "geometry audit not run");

  if (str(m, "texture_verdict")) {
    const auto v = m["texture_verdict"].get<std::string>();
    c.push_back({"texture", allowed(spec.acceptance.texture_allowed, v) ? "PASS" : v == "FAIL" ? "FAIL" : "REVIEW",
                 "texture checks " + v});
  } else unverified("texture", "texture checks not run");

  if (str(m, "fidelity_status")) {  // only when the mesh was decimated (milestone 2, docs/agent/FIDELITY.md)
    const auto v = m["fidelity_status"].get<std::string>();
    std::string why = v == "PASS" ? "the reduced model looks like the original within the fidelity limits" : "";
    for (const auto& r : m.value("fidelity_reasons", json::array())) why += (why.empty() ? "" : "; ") + r.get<std::string>();
    c.push_back({"fidelity", v == "PASS" ? "PASS" : v == "UNVERIFIED" ? "UNVERIFIED" : "FAIL", why});
  }
  if (num(m, "deviation_p99_pct")) {
    const double d = m["deviation_p99_pct"].get<double>();
    c.push_back({"shape_deviation", d <= spec.acceptance.max_deviation_pct ? "PASS" : "REVIEW",
                 "decimation moved the surface by " + fmt(d, 3) + " % of the diagonal (p99), limit " +
                     fmt(spec.acceptance.max_deviation_pct, 2) + " %"});
  }  // only present when the mesh was decimated

  if (m.contains("khronos_errors") && m["khronos_errors"].is_number()) {
    const auto e = m["khronos_errors"].get<int>();
    c.push_back({"khronos_validator", e <= spec.acceptance.khronos_max_errors ? "PASS" : "FAIL", std::to_string(e) + " errors"});
  } else unverified("khronos_validator", "Khronos validator not run");

  if (m.contains("reload_ok") && m["reload_ok"].is_boolean())
    c.push_back({"independent_reload", m["reload_ok"].get<bool>() ? "PASS" : "FAIL", "tinygltf reload of the delivered GLB"});
  else unverified("independent_reload", "independent reload not run");

  if (num(m, "scale_error_pct")) {
    const double e = m["scale_error_pct"].get<double>();
    c.push_back({"scale", e <= spec.acceptance.max_scale_error_pct ? "PASS" : "FAIL",
                 "delivered size differs from " + fmt(spec.scale_metres, 3) + " m by " + fmt(e, 3) + " %"});
  } else unverified("scale", "delivered size not measured");

  if (str(m, "hygiene_status")) {  // G0 (docs/agent/ASSET-QUALITY.md)
    std::string why;
    for (const auto& r : m.value("hygiene_reasons", json::array())) why += (why.empty() ? "" : "; ") + r.get<std::string>();
    c.push_back({"transform_hygiene", m["hygiene_status"].get<std::string>(), why.empty() ? "no mirrored or non-uniform node, winding agrees with normals" : why});
  } else unverified("transform_hygiene", "transform hygiene not measured");

  if (m.contains("up_axis_ok") && m["up_axis_ok"].is_boolean())
    c.push_back({"orientation", m["up_axis_ok"].get<bool>() ? "PASS" : "REVIEW", "Y-up, standing on y = 0"});
  else unverified("orientation", "orientation not measured");

  const auto godot = m.value("godot_status", std::string());
  if (godot == "PASS") {
    if (num(m, "godot_triangles") && num(m, "triangles") && m["godot_triangles"].get<int64_t>() != m["triangles"].get<int64_t>())
      c.push_back({"godot_import", "FAIL",
                   "Godot imported " + std::to_string(m["godot_triangles"].get<int64_t>()) + " triangles, the GLB has " +
                       std::to_string(m["triangles"].get<int64_t>())});
    else c.push_back({"godot_import", "PASS", "imported and instantiated by Godot headless"});
  } else if (godot == "FAIL") c.push_back({"godot_import", "FAIL", m.value("godot_detail", std::string("Godot import failed"))});
  else if (spec.acceptance.require_godot_import) unverified("godot_import", "Godot import not run: Godot 4 not found (3re Settings > Delivery check)");
  return c;
}

std::string overall_status(const std::vector<Criterion>& c) {
  auto any = [&](const char* s) { return std::any_of(c.begin(), c.end(), [&](const Criterion& k) { return k.status == s; }); };
  if (any("FAIL")) return "FAIL";
  if (any("REVIEW")) return "REVIEW";
  if (any("UNVERIFIED") || c.empty()) return "UNVERIFIED";
  if (any("ACCEPTED")) return "ACCEPTED";  // a person accepted a measured shortfall (milestone 3)
  return "PASS";
}

json to_json(const std::vector<Criterion>& c) {
  json a = json::array();
  for (const auto& k : c) a.push_back({{"criterion", k.name}, {"status", k.status}, {"detail", k.detail}});
  return a;
}

std::string now_iso() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm{};
  localtime_s(&tm, &t);
  char buf[40];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
  char out[48];
  std::snprintf(out, sizeof out, "%s.%03d", buf, int(ms));
  return out;
}

RunLog::RunLog(const fs::path& file) : file_(file), out_(file, std::ios::binary | std::ios::app) {
  if (!out_) throw std::runtime_error("cannot open run log " + pixel3d::to_utf8(file));
}

void RunLog::append(json entry) {
  std::lock_guard lock(mutex_);
  json line = {{"timestamp", now_iso()}};
  line.update(entry);
  out_ << line.dump() << '\n';
  out_.flush();
}

std::string render_report_markdown(const json& r) {
  std::ostringstream md;
  md << "# 3re agent report — " << r.value("name", std::string("asset")) << "\n\n";
  md << "**Result: " << r.value("status", std::string("UNVERIFIED")) << "**";
  if (r.contains("summary")) md << " — " << r["summary"].get<std::string>();
  md << "\n\n";
  if (r.contains("spec")) {
    const auto& s = r["spec"];
    md << "Spec: " << s.value("asset_type", std::string()) << ", budget " << s.value("triangle_budget", 0) << " triangles, texture "
       << s.value("texture_size", 0) << ", " << s["scale"].value("metres", 0.0) << " m (" << s["scale"].value("axis", std::string())
       << "), engine " << s["target"].value("engine", std::string()) << ".\n\n";
  }
  if (r.contains("requested_style"))
    md << "Requested style: \"" << r["requested_style"].value("style", "") << "\" — **not applied** ("
       << r["requested_style"].value("note", "") << ").\n\n";
  if (r.contains("pictures") && !r["pictures"].empty()) {
    md << "## Extra pictures (labels verified by registration on the mesh)\n\n| picture | label | status | fit | reason |\n|---|---|---|---|---|\n";
    for (const auto& p : r["pictures"])
      md << "| " << p.value("name", "") << " | " << p.value("label", "") << " | " << p.value("status", "") << " | "
         << fmt(p.value("fit", -1.0), 3) << " | " << p.value("reason", "") << " |\n";
    md << "\n";
  }
  if (r.contains("acceptance")) {
    md << "## Acceptance\n\n| criterion | status | detail |\n|---|---|---|\n";
    for (const auto& c : r["acceptance"]) md << "| " << c.value("criterion", "") << " | " << c.value("status", "") << " | " << c.value("detail", "") << " |\n";
    md << "\n";
  }
  if (r.contains("before_after")) {
    md << "## Before / after\n\n| measure | before | after |\n|---|---|---|\n";
    for (const auto& [k, v] : r["before_after"].items())
      md << "| " << k << " | " << (v.contains("before") ? v["before"].dump() : "") << " | " << (v.contains("after") ? v["after"].dump() : "")
         << " |\n";
    md << "\n";
  }
  if (r.contains("plan")) {
    md << "## Plan\n\n";
    for (const auto& s : r["plan"].value("steps", json::array())) md << "1. `" << s.value("stage", "") << "` — " << s.value("reason", "") << "\n";
    for (const auto& n : r["plan"].value("notes", json::array())) md << "- note: " << n.get<std::string>() << "\n";
    md << "\n";
  }
  md << "## Decisions\n\n";
  if (!r.contains("decisions") || r["decisions"].empty()) md << "No recovery was needed.\n\n";
  else {
    md << "| # | rule | action | reason |\n|---|---|---|---|\n";
    int i = 0;
    for (const auto& d : r["decisions"])
      md << "| " << ++i << " | " << d.value("rule", "") << " | " << d.value("action", "") << " | " << d.value("reason", "") << " |\n";
    md << "\n";
  }
  if (r.contains("human_decisions") && !r["human_decisions"].empty()) {
    md << "## Human decisions\n\n| question | asked | answer | who | when | outcome |\n|---|---|---|---|---|---|\n";
    for (const auto& d : r["human_decisions"]) {
      const auto q = d.value("question", json::object());
      const auto a = d.value("answer", json::object());
      md << "| " << q.value("id", "") << " " << q.value("title", "") << " | " << q.value("asked_at", "") << " | " << a.value("choice", "") << " | "
         << a.value("who", "") << " | " << a.value("when", "") << " (" << a.value("source", "") << ") | " << d.value("outcome", "") << " |\n";
    }
    md << "\n";
  }
  if (r.contains("pending_decisions") && !r["pending_decisions"].empty()) {
    md << "## PENDING_DECISION\n\nNot answered (questions are never answered automatically); the result above is the measured one.\n\n";
    for (const auto& d : r["pending_decisions"]) {
      const auto q = d.value("question", json::object());
      md << "- **" << q.value("id", "") << " — " << q.value("title", "") << "**: " << q.value("why", "") << "\n";
      for (const auto& o : q.value("options", json::array()))
        md << "  - `" << o.value("id", "") << "`: " << o.value("label", "") << " — " << o.value("detail", "") << "\n";
    }
    md << "\n";
  }
  md << "## Needs a person\n\n";
  if (!r.contains("human_review") || r["human_review"].empty()) md << "Nothing.\n\n";
  else for (const auto& h : r["human_review"]) md << "- " << h.get<std::string>() << "\n";
  md << "\n## Deliverables\n\n";
  for (const auto& d : r.value("deliverables", json::array()))
    md << "- `" << d.value("path", "") << "` — SHA-256 `" << d.value("sha256", "") << "`\n";
  if (r.contains("steps")) {
    md << "\n## Steps run\n\n| step | tool | exit | seconds | peak MiB |\n|---|---|---|---|---|\n";
    for (const auto& s : r["steps"])
      md << "| " << s.value("step", "") << " | " << s.value("tool", "") << " | " << (s.contains("exit_code") ? s["exit_code"].dump() : "")
         << " | " << (s.contains("seconds") ? fmt(s["seconds"].get<double>(), 1) : "") << " | "
         << (s.contains("peak_memory_mib") ? s["peak_memory_mib"].dump() : "") << " |\n";
  }
  md << "\nFull log: `run.jsonl` next to this report.\n";
  return md.str();
}

}  // namespace p3dagent
