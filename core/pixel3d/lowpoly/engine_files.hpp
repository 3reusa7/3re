#pragma once
// Engine-side style files for a LOW-POLY STUDIO export (spec §11): a Godot 4
// toon shader with the cel ramp and an outline shader for its next_pass
// (inverted hull drawn by the engine: no exported triangles), and a Unity (URP)
// setup note. They read the same values the GLB carries in
// materials[0].extras.pixel3d_style.
#include <string>

#include "pixel3d/lowpoly/types.hpp"

namespace pixel3d::lowpoly {

struct EngineStyle {
  Style style = Style::CelOutline;
  int tones = 3;
  bool baked_cel = false;
  double outline_width_px = 1.4;
  std::string glb_name;  // file name of the GLB the notes refer to
};

std::string godot_toon_shader(const EngineStyle& s);
std::string godot_outline_shader(const EngineStyle& s);
std::string unity_setup_notes(const EngineStyle& s);

}  // namespace pixel3d::lowpoly
