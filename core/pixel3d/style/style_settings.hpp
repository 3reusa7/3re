#pragma once
// Project settings of the polished styles (project format 4).
//
// crisp is today's pixel-exact model, byte for byte; the other presets build a
// polished model from it (rounded volumes inside the hull, smooth normals,
// shading bevel and ambient occlusion maps). Unset tuning values take the
// preset's value, so a project only stores what the user changed.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/common/diagnostics.hpp"

namespace pixel3d {

enum class StylePreset { Crisp, Soft, Lowpoly, Clay };
std::string_view to_string(StylePreset s);
std::optional<StylePreset> style_preset_from_string(std::string_view s);

// Per material family overrides of the PBR defaults (metal and glow are never guessed).
struct FamilyMaterial {
  int family = 0;
  std::optional<double> roughness;  // 0..1
  std::optional<double> metallic;   // 0..1
  std::optional<double> emissive;   // 0 = no glow; > 0 emits the base colour with this strength (0..100)
  bool operator==(const FamilyMaterial&) const = default;
};

struct StyleSettings {
  StylePreset preset = StylePreset::Crisp;
  std::optional<double> radius;        // corner radius, source px (0..8)
  std::optional<double> exponent;      // superellipse exponent: 2 round, 1 chamfer, > 2 squarer (1..8)
  std::optional<double> bevel;         // small bevel of corners that are not rounded, px (0..2)
  std::optional<double> smooth_angle;  // smooth normals below this angle, degrees (0 = flat shading, 0..90)
  std::optional<double> shade_radius;  // shading-only bevel (normal map), px (0 = off, 0..8)
  std::optional<bool> ao;              // bake ambient occlusion
  std::optional<bool> symmetry;        // mirror pixel-exact symmetric drawings
  std::vector<FamilyMaterial> materials;
  bool operator==(const StyleSettings&) const = default;
};

// Every value resolved (the preset's defaults where the user set nothing).
struct StyleParams {
  double radius = 0, exponent = 2, bevel = 0, smooth_angle = 0, shade_radius = 0;
  bool ao = false, symmetry = false;
};
StyleParams style_params(const StyleSettings& s);

std::optional<Message> check_style_settings(const StyleSettings& s);
nlohmann::json to_json(const StyleSettings& s);
// Missing members keep their defaults; wrong types or values are errors.
Expected<StyleSettings> style_settings_from_json(const nlohmann::json& j);

}  // namespace pixel3d
