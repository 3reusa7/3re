#include "pixel3d/lowpoly/engine_files.hpp"

#include <cstdio>

namespace pixel3d::lowpoly {
namespace {

std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return buf;
}

bool has_outline(Style s) { return s == Style::Outline || s == Style::CelOutline; }
bool has_cel(const EngineStyle& s) { return (s.style == Style::Cel || s.style == Style::CelOutline) && !s.baked_cel; }

}  // namespace

std::string godot_toon_shader(const EngineStyle& s) {
  std::string t;
  t += "// Pixel3D Low-Poly Studio: toon shading for " + s.glb_name + " (Godot 4).\n";
  t += "// Assign to the mesh's material override; set albedo_texture to the GLB's embedded texture.\n";
  t += "// Pixel art: the texture is sampled NEAREST, never filtered.\n";
  t += "shader_type spatial;\n";
  if (!has_cel(s)) t += "render_mode unshaded;\n";
  t += "\nuniform sampler2D albedo_texture : source_color, filter_nearest, repeat_disable;\n";
  t += "uniform int tones : hint_range(2, 3) = " + std::to_string(s.tones) + ";\n";
  t += "uniform float shadow_strength : hint_range(0.0, 1.0) = 0.45;\n\n";
  t += "void fragment() {\n  ALBEDO = texture(albedo_texture, UV).rgb;\n}\n";
  if (has_cel(s)) {
    t += "\n// Cel ramp: the same step function as the Pixel3D preview (2 or 3 tones of N.L).\n";
    t += "void light() {\n";
    t += "  float ndl = clamp(dot(NORMAL, LIGHT), 0.0, 1.0) * ATTENUATION;\n";
    t += "  float f = tones == 2 ? (ndl > 0.35 ? 1.0 : 1.0 - shadow_strength)\n";
    t += "                       : (ndl > 0.6 ? 1.0 : (ndl > 0.2 ? 1.0 - 0.5 * shadow_strength : 1.0 - shadow_strength));\n";
    t += "  DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR / 3.14159 * f;\n";
    t += "}\n";
  }
  return t;
}

std::string godot_outline_shader(const EngineStyle& s) {
  std::string t;
  t += "// Pixel3D Low-Poly Studio: ink outline for " + s.glb_name + " (Godot 4).\n";
  t += "// Use as next_pass of the toon material: an inverted hull drawn by the engine\n";
  t += "// (no triangles in the exported file). Width in screen pixels.\n";
  t += "shader_type spatial;\nrender_mode unshaded, cull_front;\n\n";
  t += "uniform vec4 outline_color : source_color = vec4(0.078, 0.071, 0.110, 1.0);\n";
  t += "uniform float outline_px = " + num(has_outline(s.style) ? s.outline_width_px : 0.0) + ";\n\n";
  t += "void vertex() {\n";
  t += "  vec4 clip = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);\n";
  t += "  vec3 n = normalize((MODELVIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);\n";
  t += "  vec2 offset = normalize((PROJECTION_MATRIX * vec4(n, 0.0)).xy) * outline_px * 2.0 / VIEWPORT_SIZE * clip.w;\n";
  t += "  clip.xy += offset;\n  POSITION = clip;\n}\n\n";
  t += "void fragment() {\n  ALBEDO = outline_color.rgb;\n}\n";
  return t;
}

std::string unity_setup_notes(const EngineStyle& s) {
  std::string t;
  t += "# " + s.glb_name + " in Unity (URP)\n\n";
  t += "Exported by Pixel3D Low-Poly Studio. The GLB carries the style in `materials[0].extras.pixel3d_style`.\n\n";
  t += "1. Import with glTFast (com.unity.cloud.gltfast). The character is one skinned mesh; bones follow the\n";
  t += "   Unity humanoid names (Hips, Spine, Head, LeftUpperLeg, ...), so the Humanoid avatar maps them.\n";
  t += "2. Texture: Filter Mode = Point (no filter), Compression = None, Generate Mip Maps = off.\n";
  if (s.baked_cel) {
    t += "3. Lighting is baked into the texture (baked_cel): use an Unlit material (Universal Render Pipeline/Unlit).\n";
  } else if (has_cel(s)) {
    t += "3. Toon shading: a 2/3-step ramp of N.L (" + std::to_string(s.tones) +
         " tones). In Shader Graph: Main Light direction -> Dot(Normal) -> Step/Comparison into " +
         std::to_string(s.tones) + " bands -> multiply the base colour.\n";
  } else {
    t += "3. Flat colours: use an Unlit material.\n";
  }
  if (has_outline(s.style))
    t += "4. Outline (" + num(s.outline_width_px) +
         " px): a second material pass with Cull Front that pushes vertices along the normal (inverted hull), or a\n"
         "   screen-space outline Renderer Feature. The outline is drawn by the engine; the file has no outline triangles.\n";
  return t;
}

}  // namespace pixel3d::lowpoly
