#pragma once
// The MCP tool catalog. Each tool decodes its arguments (mcp/args.hpp), makes
// exactly one Session call and returns the core OpResult unchanged — no
// modelling logic lives here.
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mcp/args.hpp"
#include "pixel3d/engine/session.hpp"

namespace pixel3d::mcp {

struct ToolContext {
  Session& session;
  const PathPolicy& paths;
  JobContext job;
};

struct ToolOutput {
  OpResult result;
  std::vector<NamedImage> images;  // returned inline as PNG image content
};

struct ToolDef {
  std::string name, title, description;
  nlohmann::json input_schema;
  bool read_only = false;    // never changes the document
  bool destructive = false;  // may discard state (new/open project, restore, undo)
  bool idempotent = false;
  std::function<ToolOutput(ToolContext&, const Args&)> run;
};

const std::vector<ToolDef>& tool_catalog();
const ToolDef* find_tool(const std::string& name);

// OpResult for read-only answers: success, the given data, and the document's
// current revision / status / validation summary.
OpResult describe(const Session& s, nlohmann::json data);

}  // namespace pixel3d::mcp
