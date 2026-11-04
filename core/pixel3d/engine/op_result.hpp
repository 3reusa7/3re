#pragma once
// Uniform result of every Session call; to_json gives the structure MCP returns.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/validate/quality_gate.hpp"

namespace pixel3d {

struct MeshStats {
  int triangles = 0;
  int vertices = 0;
  int parts = 0;
  double volume = 0;
  bool watertight = false;
};

struct OpResult {
  bool success = false;
  std::vector<Message> errors, warnings, info;
  std::vector<std::string> changed;  // document members touched, e.g. "views", "mesh", "texture"
  nlohmann::json changed_ids;        // what exactly changed (views, parts, candidates, constraints, ...); {} if nothing
  uint64_t revision = 0;             // document revision after the call
  MeshStats stats;
  nlohmann::json metrics;            // null unless the document is validated
  ValidationStatus status = ValidationStatus::NotValidated;
  // Why the status is what it is: failed critical checks and blocking conflicts
  // (null unless validated). A NEEDS_REVIEW always lists what needs review.
  nlohmann::json validation;
  nlohmann::json data;               // op-specific payload

  void add(const Message& m);        // routed by severity
  static OpResult fail(Message m);
};

nlohmann::json to_json(const OpResult& r);

}  // namespace pixel3d
