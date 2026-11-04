#include "pixel3d/io/export_presets.hpp"

namespace pixel3d {

std::string_view to_string(ExportFormat f) {
  switch (f) {
    case ExportFormat::Glb: return "glb";
    case ExportFormat::Gltf: return "gltf";
    case ExportFormat::Obj: return "obj";
  }
  return "glb";
}

std::optional<ExportFormat> export_format_from_string(std::string_view s) {
  if (s == "glb") return ExportFormat::Glb;
  if (s == "gltf") return ExportFormat::Gltf;
  if (s == "obj") return ExportFormat::Obj;
  return std::nullopt;
}

std::string_view extension(ExportFormat f) {
  switch (f) {
    case ExportFormat::Glb: return ".glb";
    case ExportFormat::Gltf: return ".gltf";
    case ExportFormat::Obj: return ".obj";
  }
  return ".glb";
}

std::string_view to_string(EnginePreset p) {
  switch (p) {
    case EnginePreset::Generic: return "generic";
    case EnginePreset::Godot: return "godot";
    case EnginePreset::Unity: return "unity";
    case EnginePreset::Roblox: return "roblox";
  }
  return "generic";
}

std::optional<EnginePreset> engine_preset_from_string(std::string_view s) {
  if (s == "generic" || s == "gltf") return EnginePreset::Generic;
  if (s == "godot") return EnginePreset::Godot;
  if (s == "unity") return EnginePreset::Unity;
  if (s == "roblox") return EnginePreset::Roblox;
  return std::nullopt;
}

PresetDefaults preset_defaults(EnginePreset p) {
  switch (p) {
    case EnginePreset::Generic:
      return {ExportFormat::Glb, ExportUnits::Metres, false, "glTF convention: metres, +Y up, the front faces +Z."};
    case EnginePreset::Godot:
      return {ExportFormat::Glb, ExportUnits::Metres, false,
              "Godot 4 keeps glTF axes; the front faces +Z, Godot's MODEL_FRONT."};
    case EnginePreset::Unity:
      return {ExportFormat::Glb, ExportUnits::Metres, false,
              "glTF importers for Unity convert handedness; the front stays Unity's forward (+Z)."};
    case EnginePreset::Roblox:
      return {ExportFormat::Obj, ExportUnits::Studs, true,
              "OBJ in studs (1 stud = 0.28 m) with the front facing -Z, a part's Front surface."};
  }
  return {};
}

}  // namespace pixel3d
