#pragma once
// Capabilities (tool registry) and the deterministic planner (PLAN.md, components 2 and 3).
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_spec.hpp"

namespace p3dagent {

struct ToolInfo {
  fs::path path;
  bool present = false;
  std::string sha256, expected_sha256;  // expected empty = no pin for this tool
  bool verified = false;                // present and (no pin or the pin matches)
  std::string version, note;
};

struct Capabilities {
  uint64_t ram_total_mib = 0, ram_free_mib = 0, disk_free_mib = 0, vram_mib = 0;
  std::string gpu_name;
  bool gpu_present = false;
  bool weights_verified = false;
  std::string weights_note;
  std::map<std::string, ToolInfo> tools;  // engine, post_safe, studio, auto_texture, inspect, audit, decimate, glbcheck, validator, godot,
                                          // texture_replay (engine fork: texture-only TRELLIS replays, milestone 2)
  fs::path models_dir;                    // TRELLIS Q4 weights (from the 3re config)
  bool has(const std::string& tool) const;
  // TRELLIS generation through the studio: engine + post-safe + studio verified, weights verified, a GPU.
  bool can_generate(std::string* why = nullptr) const;
  nlohmann::json to_json() const;
};

// What is known about the inputs before anything runs.
struct InputFacts {
  bool picture_exists = false, picture_has_alpha = false;
  bool glb_exists = false, glb_textured = false;
  bool run_exists = false, run_has_geometry = false, run_has_raw_ply = false, run_has_native = false,
       run_has_cutout = false;
  fs::path run_cutout;              // texture-source.png, or an RGBA input.png in older runs
  std::string run_cleanup_sha256;  // post_sha256 in the run's report.json
  std::string run_status;          // geometry verdict saved in the run's report.json
};

struct PlannedStep {
  std::string stage, reason;
};

struct Plan {
  bool blocked = false;
  std::string blocked_reason;
  std::vector<PlannedStep> steps;
  // Where the texture comes from: "hybrid" (picture + native.post), "picture", "keep" (the GLB's own), "none".
  std::string texture_source = "none";
  fs::path texture_picture;  // the cutout the texture worker gets
  std::vector<std::string> notes;
  bool has(const std::string& stage) const;
  nlohmann::json to_json() const;
};

Plan make_plan(const AssetSpec& spec, const Capabilities& caps, const InputFacts& facts);

}  // namespace p3dagent
