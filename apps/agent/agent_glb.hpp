#pragma once
// GLB reading/inspection and the delivery transform (PLAN.md, components 5 and 7).
// The binary chunk is carried through byte for byte, except the POSITION data with_baked_transform rewrites.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_spec.hpp"

namespace p3dagent {

// count elements of `elem` bytes, `stride` apart, starting at `off`, all inside [0, limit): overflow-safe.
inline bool span_ok(size_t off, size_t count, size_t stride, size_t elem, size_t limit) {
  if (count == 0) return off <= limit;
  size_t last = 0, end = 0;
  if (__builtin_mul_overflow(count - 1, stride, &last) || __builtin_add_overflow(off, last, &end) || __builtin_add_overflow(end, elem, &end))
    return false;
  return end <= limit;
}

struct Glb {
  nlohmann::json json;
  std::vector<uint8_t> bin;
};
Glb read_glb(const fs::path& file);               // throws std::runtime_error with the reason
Glb parse_glb(const std::vector<uint8_t>& bytes);  // same, from memory
std::vector<uint8_t> encode_glb(const Glb& glb);
void write_new_file(const fs::path& file, const std::vector<uint8_t>& bytes);  // refuses to overwrite

struct GlbInfo {
  int64_t triangles = 0;
  int meshes = 0, primitives = 0, materials = 0, textures = 0, images = 0, nodes = 0;
  bool has_uv = false, has_normals = false, textured = false;
  // World-space box of every mesh (accessor min/max through the node hierarchy).
  std::array<double, 3> min{0, 0, 0}, max{0, 0, 0};
  std::array<double, 3> size() const { return {max[0] - min[0], max[1] - min[1], max[2] - min[2]}; }
  nlohmann::json to_json() const;
};
GlbInfo inspect_glb(const Glb& glb);

// Uniform scale to the spec's metres along the axis, then translate so the model stands on y = 0,
// centred on x/z. glTF is Y-up; the Godot preset keeps glTF axes (front +Z).
// p' = scale * R_y(rotation_y_deg) * p + translation.
struct DeliveryTransform {
  double scale = 1;
  std::array<double, 3> translation{0, 0, 0};
  double source_extent = 0;   // the measured extent along the axis, model units (after the rotation)
  double rotation_y_deg = 0;  // about +Y, right-handed (A4: turns the picture's front onto +Z)
};
DeliveryTransform delivery_transform(const GlbInfo& info, ScaleAxis axis, double metres);
// Same, with a rotation about Y first; the box is measured on the rotated vertices (exact grounding and centring).
DeliveryTransform delivery_transform(const Glb& glb, ScaleAxis axis, double metres, double rotation_y_deg);
std::array<double, 3> rotate_y(const std::array<double, 3>& p, double deg);
// The rotation that turns a picture registered at `registered_yaw_deg` (camera direction (sin y, 0, cos y), the
// texture worker's convention) onto +Z, the glTF front that Godot, Unity and Unreal importers map to their front.
double front_rotation_deg(double registered_yaw_deg);

// A new root node carries the transform; every scene's old root nodes become its children.
// Vertices, accessors and the binary chunk stay untouched.
Glb with_root_transform(Glb glb, const DeliveryTransform& t, const std::string& root_name);

// The transform baked into the vertices: only for one mesh node without its own transform, used once, whose
// POSITION accessors are plain float VEC3 (every 3re output). The file stays one node with an identity
// transform, as 3re's importer (Smart Retopology, re-texturing) requires. Normals are unchanged by a uniform
// scale + translation. nullopt = not bakeable: deliver with_root_transform instead.
std::optional<Glb> with_baked_transform(Glb glb, const DeliveryTransform& t);

}  // namespace p3dagent
