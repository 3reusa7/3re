#pragma once
// The polished model of a project (style soft, lowpoly or clay).
//
// Built from the crisp result, never instead of it: the crisp parts (after the
// user's edits) are rounded inside the hull, mirrored when the drawings are
// pixel-exact mirror images, assembled, textured from the crisp texture (one
// view per texel), shaded with smooth normals and given a second UV set with
// the shading-bevel normal map and ambient occlusion. Its materials are PBR
// per material family. This model is the one that is validated and exported.
#include <map>
#include <string>
#include <vector>

#include "pixel3d/image/palette.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/mesh/cleanup.hpp"
#include "pixel3d/reconstruct/part_geometry.hpp"
#include "pixel3d/style/normals.hpp"
#include "pixel3d/style/rounding.hpp"
#include "pixel3d/style/style_settings.hpp"
#include "pixel3d/style/symmetry.hpp"
#include "pixel3d/texture/projector.hpp"
#include "pixel3d/texture/unique_uv.hpp"

namespace pixel3d {

struct StyledModel {
  std::string key;  // style_input_hash of the document it was built from
  StylePreset preset = StylePreset::Crisp;
  StyleParams params;
  SolidResult solid;      // the polished solid (topology, volume)
  TextureResult texture;  // texture.mesh: the full polished mesh (smooth normals, UV1, tangents, maps)
  StyleExport materials;  // PBR material per family and the material of every triangle
  std::vector<PartRoundReport> rounding;
  SymmetryResult symmetry;
  MirrorReport mirror;
  NormalReport normals;
  UniqueUVReport uv;
  int atlas_scale = 1;          // the colour atlas was padded and enlarged by this factor (mipmap-safe)
  std::vector<Message> notes;   // what the style did; fallbacks are warnings
};

struct StyleInputs {
  const PartGeometry* parts = nullptr;      // crisp parts after the user's edits
  const Mesh* crisp = nullptr;              // crisp solid mesh (its tri_region labels planar faces)
  const TextureResult* crisp_texture = nullptr;
  const MaterialFamilies* families = nullptr;
  std::map<ViewRole, Mask2D> masks;         // drawn silhouettes (symmetry test)
  Dims dims;
  CleanupOptions cleanup;
  StyleSettings settings;
};

// Errors: style.empty_model (nothing left to build), style.not_polished (crisp asked).
Expected<StyledModel> build_styled_model(const StyleInputs& in);

// Polished export settings: per-family materials and mipmapped sampling.
void apply_style_export(const StyledModel& m, ExportOptions& eo);

// Summary for reports, inspect and the MCP results.
nlohmann::json to_json(const StyledModel& m);

}  // namespace pixel3d
