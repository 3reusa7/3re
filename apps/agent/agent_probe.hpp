#pragma once
// Capability probe: the machine and every tool the agent may call, SHA-verified where 3re pins
// a hash (PLAN.md, component 2), and the facts about the spec's inputs.
#include "agent_plan.hpp"

namespace p3dagent {

struct ProbeInputs {
  fs::path config;       // 3re.config.json (trellis block: runtime, models, post-safe, manifest, pins)
  fs::path app_dir;      // folder of 3re-studio.exe (generation through the wizard)
  fs::path workers_dir;  // folder of pixel3d-* workers (normally app_dir)
  std::vector<fs::path> extra_worker_dirs;  // searched after workers_dir (development trees)
  fs::path validator;    // gltf_validator.exe (optional)
  fs::path godot;        // Godot 4.x console executable (optional)
  fs::path disk_path;    // where outputs go (free space)
  bool hash_weights = true;
};
Capabilities probe_capabilities(const ProbeInputs& in);

// Reads headers and small files only (PNG colour type, run folder listing, report.json).
InputFacts inspect_inputs(const AssetSpec& spec);
bool picture_has_alpha(const fs::path& picture);

}  // namespace p3dagent
