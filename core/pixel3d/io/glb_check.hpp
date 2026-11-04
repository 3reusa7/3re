#pragma once
// Independent round-trip validation of an exported model: GLB and glTF are
// reloaded with tinygltf, OBJ with a separate reader; every attribute is
// compared with what was exported and the reloaded mesh is re-rendered.
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/views/view_role.hpp"

namespace pixel3d {

struct GlbCheck {
  bool ok = false;
  std::string format = "glb";
  std::vector<std::string> problems;
  int vertices = 0, triangles = 0;
  int image_w = 0, image_h = 0;
  bool sampler_nearest = false;
  bool material_opaque = false;
  bool unlit = false;
  bool rerender_identical = false;  // only checked when dims are given
  int rerender_diff_px = 0;          // pixels that differ after the float32 round trip
  double bbox_min_y = 0, bbox_height = 0, bbox_center_x = 0;  // metres
};

// `rerender_dims`: also render the reloaded mesh from Front at source
// resolution; it must match except for float32-level flips (≤ max(2, 0.2%)).
GlbCheck check_glb_roundtrip(std::span<const uint8_t> glb, const TexturedMesh& expected, const ExportOptions& opt,
                             std::optional<Dims> rerender_dims);
// A .gltf with its companion files given in memory (`<base>.bin`, `<base>.png`).
GlbCheck check_gltf_roundtrip(const std::string& gltf_json, std::span<const uint8_t> bin, std::span<const uint8_t> png,
                              const std::string& base_name, const TexturedMesh& expected, const ExportOptions& opt,
                              std::optional<Dims> rerender_dims);
// An .obj with its .mtl and texture, parsed by a reader independent of the writer.
GlbCheck check_obj_roundtrip(const std::string& obj, const std::string& mtl, std::span<const uint8_t> png,
                             const std::string& base_name, const TexturedMesh& expected, const ExportOptions& opt,
                             std::optional<Dims> rerender_dims);

nlohmann::json to_json(const GlbCheck& c);

}  // namespace pixel3d
