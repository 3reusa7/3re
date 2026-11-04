#pragma once
// Internals shared by the tool sources: JSON Schema builders and the render
// tool handlers (tools_render.cpp).
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "mcp/tools.hpp"

namespace pixel3d::mcp {

namespace schema {

using nlohmann::json;

inline json str(const std::string& d) { return {{"type", "string"}, {"description", d}}; }
inline json boolean(const std::string& d) { return {{"type", "boolean"}, {"description", d}}; }
inline json number(const std::string& d, double lo, double hi) {
  json j{{"type", "number"}, {"minimum", lo}, {"maximum", hi}};
  if (!d.empty()) j["description"] = d;
  return j;
}
inline json integer(const std::string& d, int lo, int hi) {
  return {{"type", "integer"}, {"minimum", lo}, {"maximum", hi}, {"description", d}};
}
inline json enumeration(const std::string& d, std::vector<std::string> values) {
  return {{"type", "string"}, {"enum", values}, {"description", d}};
}
inline json role_enum(const std::string& d) {
  return enumeration(d, {"front", "back", "side", "right", "top", "bottom"});
}
inline json array(json items, const std::string& d, int min_items = -1, int max_items = -1) {
  json j{{"type", "array"}, {"items", std::move(items)}, {"description", d}};
  if (min_items >= 0) j["minItems"] = min_items;
  if (max_items >= 0) j["maxItems"] = max_items;
  return j;
}
// The input-normalisation arguments shared by new_project, import_views and update_settings.
inline json with_normalize(json properties) {
  properties["normalize"] = boolean(
      "Normalise real-world images before alignment (default true): remove an opaque background, snap alpha, merge "
      "noise colours, detect the pixel scale and shrink oversize art. Clean pixel art is never changed. false = use "
      "every image exactly as given.");
  properties["target_size"] =
      integer("Longest model side in pixels for views that needed normalisation (default 128).", 16, 512);
  properties["bg_tolerance"] =
      integer("Largest per-channel distance from the border colour that still counts as background (default 24).", 0,
              255);
  properties["axis_tolerance"] = number(
      "Largest relative disagreement of shared axes between views before input.views_inconsistent (default 0.15).", 0, 1);
  return properties;
}

// The model style shared by new_project, new_single_view_project, update_settings and set_style.
inline json style_enum() {
  return enumeration(
      "Model style: crisp (default, the pixel-exact model), soft, lowpoly or clay (polished: rounded volumes "
      "inside the drawings' hull, every drawn pixel kept, smooth shading, normal and occlusion maps). The "
      "polished model is built at validate and is the one validated and exported. Not the render style "
      "beauty of render_view.",
      {"crisp", "soft", "lowpoly", "clay"});
}

inline json object(json properties, std::vector<std::string> required = {}) {
  json j{{"type", "object"}, {"properties", properties.is_null() ? json::object() : std::move(properties)},
         {"additionalProperties", false}};
  if (!required.empty()) j["required"] = required;
  return j;
}

}  // namespace schema

ToolOutput render_view(ToolContext& c, const Args& a);
StylePreset parse_style(const char* field, const std::string& v);  // tools.cpp
void append_edit_tools(std::vector<ToolDef>& tools);  // tools_edit.cpp
void append_lowpoly_tools(std::vector<ToolDef>& tools);  // tools_lowpoly.cpp
ToolOutput render_turntable(ToolContext& c, const Args& a);

}  // namespace pixel3d::mcp
