#pragma once
// Own glTF 2.0 writer (binary .glb, or .gltf + .bin + .png): one node, one
// mesh, one primitive (POSITION, NORMAL, TEXCOORD_0, indices), one opaque
// material with the PNG atlas sampled NEAREST/NEAREST, CLAMP_TO_EDGE, no
// mipmaps. Model axes are already glTF (+Y up, front +Z); the export frame
// moves the origin to the bottom centre, scales to the output unit and can
// turn the front to −Z (a 180° rotation about +Y, so winding stays valid).
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

// The unit one exported coordinate means.
enum class ExportUnits { Metres, Centimetres, Studs, Pixels };
std::string_view to_string(ExportUnits u);
std::optional<ExportUnits> export_units_from_string(std::string_view s);
inline constexpr double kMetresPerStud = 0.28;  // Roblox's documented physics scale

// Polished styles (step 8): a PBR material and its triangles.
struct StyleMaterial {
  std::string name;
  double roughness = 0.8;
  double metallic = 0;
  double emissive = 0;  // 0: no glow; > 0: emits its base colour with this strength (set by the user only)
};

// Polished export: one primitive per material (triangles grouped in material
// order), TEXCOORD_1 + TANGENT when the mesh has them, the mesh's normal and
// occlusion maps (texCoord 1). Crisp exports never set it.
struct StyleExport {
  std::vector<StyleMaterial> materials;
  std::vector<int> tri_material;  // per triangle; out of range → material 0
};

// Writer settings. The Session fills them from the project settings (scale) and
// the export request (units, front, unlit), so every client writes the same file.
struct ExportOptions {
  double pixels_per_meter = 32;
  bool center_bottom = true;  // origin at the bbox bottom centre
  ExportUnits units = ExportUnits::Metres;
  bool front_negative_z = false;  // the drawing's Front faces −Z (rotated 180° about +Y)
  bool unlit = false;             // KHR_materials_unlit
  // Polished styles: NEAREST up close, NEAREST_MIPMAP_LINEAR at a distance
  // (the atlas must be padded first, see pad_for_filtering). Crisp: off.
  bool mipmaps = false;
  const StyleExport* style = nullptr;  // polished styles only; nullptr = the crisp file, byte for byte
  // asset.generator; empty = "Pixel3D <this version>". Tests pin it to compare files across versions.
  std::string generator;
};

struct ExportFrame {
  vec3d origin;       // world point that becomes (0, 0, 0)
  double scale;       // output units per world unit (source pixel)
  bool flip = false;  // 180° about +Y
  vec3d apply(vec3d p) const {
    const vec3d q = (p - origin) * scale;
    return flip ? vec3d{-q.x, q.y, -q.z} : q;
  }
  vec3d apply_dir(vec3d n) const { return flip ? vec3d{-n.x, n.y, -n.z} : n; }
  vec3d invert(vec3d q) const {
    if (flip) q = {-q.x, q.y, -q.z};
    return q / scale + origin;
  }
};
ExportFrame export_frame(const TexturedMesh& m, const ExportOptions& opt);

std::vector<uint8_t> write_glb(const TexturedMesh& m, const ExportOptions& opt);

// .gltf (JSON) referencing `<base>.bin` and `<base>.png` beside it.
struct GltfFiles {
  std::string json;
  std::vector<uint8_t> bin, png;
  // Polished styles: the maps beside it, `<base><suffix>.png` (suffix "_normal", "_occlusion").
  std::vector<std::pair<std::string, std::vector<uint8_t>>> extra_png;
};
GltfFiles write_gltf(const TexturedMesh& m, const ExportOptions& opt, const std::string& base_name);

}  // namespace pixel3d
