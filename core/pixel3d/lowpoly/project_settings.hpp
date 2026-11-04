#pragma once
// Project settings of LOW-POLY STUDIO (project format 5): the project mode,
// the build settings and the per-part overrides (inputs.lowpoly), and the part
// types (inputs.part_types). A project with `enabled` is a Low-Poly Studio
// project: its views are aligned as Front with optional Side and Top, and its
// model is the low-poly character (lowpoly_build), not the visual hull.
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/lowpoly/types.hpp"

namespace pixel3d {

struct LowpolyProjectSettings {
  bool enabled = false;               // project mode lowpoly_studio
  lowpoly::Settings build;            // budget, joint loops, style, template, depth, ...
  std::map<int, double> depth_ratio;  // per part: minimum depth/width of its cross-sections (front only)
  bool operator==(const LowpolyProjectSettings&) const = default;
};

std::optional<Message> check_lowpoly_settings(const LowpolyProjectSettings& s);
nlohmann::json to_json(const LowpolyProjectSettings& s);
// Missing members keep their defaults; wrong types or values are errors (input.invalid).
Expected<LowpolyProjectSettings> lowpoly_settings_from_json(const nlohmann::json& j);

nlohmann::json part_types_to_json(const std::map<int, lowpoly::PartType>& t);
Expected<std::map<int, lowpoly::PartType>> part_types_from_json(const nlohmann::json& j);

}  // namespace pixel3d
