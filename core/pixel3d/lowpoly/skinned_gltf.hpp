#pragma once
// Skinned glTF 2.0 (.glb) for LOW-POLY STUDIO. Separate from the existing
// writer so existing exports stay byte-identical. One mesh node with a skin,
// one joint node per bone (rest rotations identity, translations relative to
// the parent), inverseBindMatrices = inverse of each joint's rest transform,
// JOINTS_0 / WEIGHTS_0 per vertex. Primitive 0 is the textured character;
// primitive 1 (only with an inverted hull) is the black outline shell.
// The style travels as materials[0].extras.pixel3d_style; `flat` (and baked
// cel) use KHR_materials_unlit.
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/lowpoly/types.hpp"

namespace pixel3d::lowpoly {

struct SkinnedExportOptions {
  ExportOptions frame;            // units, scale, bottom-centre origin, front −Z
  Style style = Style::CelOutline;
  int tones = 3;
  bool baked_cel = false;
  double outline_width_px = 1.4;  // engine-side outline width hint
  nlohmann::json report;          // asset.extras.pixel3d_lowpoly
};

// Frame shared by the writer and the checks (origin at the bottom centre of the mesh).
ExportFrame skinned_frame(const SkinnedMesh& m, const ExportOptions& opt);

std::vector<uint8_t> write_skinned_glb(const SkinnedMesh& m, const Skeleton& s, const ImageRGBA& texture,
                                       const SkinnedExportOptions& opt);

struct SkinnedCheck {
  bool ok = false;
  std::vector<std::string> problems;
  int vertices = 0, triangles = 0, joints = 0, primitives = 0;
  double max_weight_sum_error = 0;
  double max_ibm_error = 0;    // |IBM · joint global − I|
  double max_attribute_error = 0;
};
nlohmann::json to_json(const SkinnedCheck& c);

// Reloads the GLB with tinygltf (independent of the writer) and compares every
// attribute, the skin and the joint hierarchy with what was meant.
SkinnedCheck check_skinned_glb(std::span<const uint8_t> glb, const SkinnedMesh& expected, const Skeleton& s,
                               const SkinnedExportOptions& opt);

}  // namespace pixel3d::lowpoly
