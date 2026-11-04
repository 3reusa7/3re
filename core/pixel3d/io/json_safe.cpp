#include "pixel3d/io/json_safe.hpp"

#include <string>

namespace pixel3d {

bool json_depth_within(std::string_view text, int max_depth) {
  int depth = 0;
  bool in_string = false, escaped = false;
  for (const char c : text) {
    if (in_string) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') in_string = false;
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '[' || c == '{') {
      if (++depth > max_depth) return false;
    } else if (c == ']' || c == '}') {
      --depth;
    }
  }
  return true;
}

Expected<nlohmann::json> parse_json_untrusted(std::string_view text, size_t max_bytes, int max_depth) {
  if (text.size() > max_bytes)
    return make_error("json.too_large", "JSON text is larger than " + std::to_string(max_bytes) + " bytes",
                      {{"bytes", text.size()}, {"limit", max_bytes}});
  if (!json_depth_within(text, max_depth))
    return make_error("json.too_deep", "JSON nests deeper than " + std::to_string(max_depth) + " levels",
                      {{"limit", max_depth}});
  try {
    return nlohmann::json::parse(text);
  } catch (const nlohmann::json::parse_error& e) {
    return make_error("json.parse_error", e.what(), {{"byte", e.byte}});
  } catch (const std::exception& e) {
    return make_error("json.parse_error", e.what());
  }
}

}  // namespace pixel3d
