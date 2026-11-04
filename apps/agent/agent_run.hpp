#pragma once
// The agent run: probe -> plan -> execute real workers -> evaluate -> bounded recovery ->
// deliver -> validate -> report (docs/agent/PLAN.md). Console + run.jsonl + report.json/md.
#include <string>
#include <vector>
#include "agent_decide.hpp"
#include "agent_spec.hpp"

namespace p3dagent {

struct AgentOptions {
  fs::path spec_file, out_dir;      // out_dir overrides the spec's output_dir
  fs::path config;                  // 3re.config.json (default: next to the agent, or THREERE_TRELLIS_CONFIG)
  fs::path app_dir;                 // folder of 3re-studio.exe (default: the agent's folder)
  std::vector<fs::path> workers;    // worker folders (default: the agent's folder)
  fs::path godot, validator;        // defaults: THREERE_GODOT / config "agent" block / next to the agent
  bool hash_weights = true;
  DecisionOptions decisions;        // milestone 3: headless (PENDING_DECISION) or ask and wait for a person
};

// Exit codes: 0 PASS or ACCEPTED (a person accepted a measured shortfall), 1 REVIEW or UNVERIFIED, 2 FAIL, 3 BLOCKED, 4 spec/usage/output-folder error.
int run_agent(const AgentOptions& options);

}  // namespace p3dagent
