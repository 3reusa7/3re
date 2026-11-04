#pragma once
// Parsing of untrusted JSON text (project files, MCP requests). Size and nesting
// depth are checked before the DOM is built — deep nesting would otherwise
// exhaust the stack when the value is later dumped — and no exception escapes.
#include <string_view>

#include <nlohmann/json.hpp>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/common/limits.hpp"

namespace pixel3d {

// True when no array/object nests deeper than max_depth (strings are skipped).
bool json_depth_within(std::string_view text, int max_depth);

// Errors: json.too_large, json.too_deep, json.parse_error (with byte offset).
Expected<nlohmann::json> parse_json_untrusted(std::string_view text, size_t max_bytes,
                                              int max_depth = limits::kMaxJsonDepth);

}  // namespace pixel3d
