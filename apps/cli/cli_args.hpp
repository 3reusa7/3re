#pragma once
// Command line of pixel3d-cli (spec §14). Parsing only; no engine work here.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "pixel3d/engine/session.hpp"

namespace pixel3d {

struct CliArgs {
  std::string command;  // reconstruct | validate | info | version | help
  std::map<ViewRole, std::string> views;
  ReconstructMode mode = ReconstructMode::TrueSolid;
  std::optional<ToleranceProfile> profile;
  std::string project, out, report, renders;
  bool turntable = false;
  double pixels_per_meter = 32;
  // Export (M5): the format comes from --out's extension (.glb, .gltf, .obj).
  EnginePreset preset = EnginePreset::Generic;
  std::optional<ExportUnits> units;
  std::optional<bool> front_negative_z;
  bool unlit = false;
  // Input normalisation (--normalize on|off, --target-size, --bg-tolerance, --axis-tolerance).
  NormalizeSettings normalize;
  // Polished style (--style crisp|soft|lowpoly|clay).
  StylePreset style = StylePreset::Crisp;
  // One drawing (--single-view flat|thin|medium|thick|<depth px>): exactly one view.
  std::optional<std::string> single_view_preset;
  std::optional<double> single_view_depth;
  // LOW-POLY STUDIO (command lowpoly): settings applied before the build.
  LowpolyUpdate lowpoly;
  std::string parts_project;         // --parts: take the typed parts of this Low-Poly Studio project
  // --layer-parts all|a,b,c: the parts are the drawings' layers (.aseprite / .psd), all suggested
  // ones or the named ones, instead of the colour-based suggestion.
  std::optional<std::vector<std::string>> layer_parts;
  bool accept_needs_review = false;  // --accept-status NEEDS_REVIEW
  bool engine_files = false;         // --engine-files
};

Expected<CliArgs> parse_cli(const std::vector<std::string>& args);
std::string cli_usage();
// 0 PASS, 2 NEEDS_REVIEW, 1 FAIL or not validated.
int exit_code_for(ValidationStatus s);

}  // namespace pixel3d
