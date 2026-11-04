#pragma once
// Export formats and engine presets (spec M5 §1). A preset is a documented
// bundle of format, units and front direction; every field can be overridden.
#include <optional>
#include <string_view>

#include "pixel3d/io/glb_writer.hpp"

namespace pixel3d {

enum class ExportFormat { Glb, Gltf, Obj };
std::string_view to_string(ExportFormat f);
std::optional<ExportFormat> export_format_from_string(std::string_view s);
std::string_view extension(ExportFormat f);  // ".glb", ".gltf", ".obj"

enum class EnginePreset { Generic, Godot, Unity, Roblox };
std::string_view to_string(EnginePreset p);
std::optional<EnginePreset> engine_preset_from_string(std::string_view s);

struct PresetDefaults {
  ExportFormat format = ExportFormat::Glb;
  ExportUnits units = ExportUnits::Metres;
  bool front_negative_z = false;
  std::string_view note;  // one sentence: what the preset assumes about the engine
};
PresetDefaults preset_defaults(EnginePreset p);

}  // namespace pixel3d
