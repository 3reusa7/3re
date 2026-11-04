#pragma once
// MCP resources (read-only views of the session) and prompts (the agent workflow).
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "pixel3d/engine/session.hpp"

namespace pixel3d::mcp {

nlohmann::json resource_list();
nlohmann::json resource_templates();
// `contents` array for resources/read, or nullopt if the URI is unknown.
std::optional<nlohmann::json> read_resource(const Session& s, const std::string& uri);

nlohmann::json prompt_list();
std::optional<nlohmann::json> get_prompt(const std::string& name, const nlohmann::json& arguments);

std::string server_instructions();
std::string_view skill_markdown();  // skills/pixel3d-reconstruct/SKILL.md, embedded at build time

std::string base64(std::string_view bytes);

}  // namespace pixel3d::mcp
