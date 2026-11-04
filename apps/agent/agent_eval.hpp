#pragma once
// Acceptance evaluation and the run log / report (PLAN.md, components 5 and 8).
// A criterion that could not be measured is UNVERIFIED, never PASS.
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_spec.hpp"

namespace p3dagent {

struct Criterion {
  std::string name, status, detail;  // status: PASS, REVIEW, FAIL, UNVERIFIED
};

// Measured keys (missing or null = not measured): triangles, geometry_verdict, texture_verdict,
// khronos_errors, reload_ok, godot_status ("PASS"/"FAIL"/"UNVERIFIED"), godot_triangles,
// scale_error_pct, up_axis_ok, deviation_p99_pct, materials.
std::vector<Criterion> evaluate_acceptance(const AssetSpec& spec, const nlohmann::json& measured);
// FAIL > REVIEW > UNVERIFIED > PASS.
std::string overall_status(const std::vector<Criterion>& criteria);
nlohmann::json to_json(const std::vector<Criterion>& criteria);

std::string now_iso();

// run.jsonl: one JSON object per line, flushed on every append, timestamp added.
class RunLog {
 public:
  explicit RunLog(const fs::path& file);
  void append(nlohmann::json entry);
  const fs::path& file() const { return file_; }
 private:
  fs::path file_;
  std::ofstream out_;
  std::mutex mutex_;
};

std::string render_report_markdown(const nlohmann::json& report);

}  // namespace p3dagent
