#pragma once
// Independent generated-asset contract. Never changes a drawing Session.
#include <filesystem>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace p3dasset {
struct SavedPreviewMemory {
  uint64_t required_free_bytes = 5ull*1024*1024*1024/2;
  uint64_t preview_free_bytes = 1024ull*1024*1024;
  uint64_t encoded_bytes = 0, decoded_image_bytes = 0, vertices = 0, triangles = 0;
  bool bounded_profile = false;
};
// Metadata/image-header inspection only. No texture decode or mesh mutation.
SavedPreviewMemory saved_preview_memory(const std::filesystem::path& path);
enum class Validation { Unknown, Unsafe, Review, Pass };
struct MeshAsset {
  std::filesystem::path geometry_glb;
  std::filesystem::path raw_glb;
  std::filesystem::path report;
  std::shared_ptr<const pixel3d::Mesh> geometry;
  std::shared_ptr<const pixel3d::TexturedMesh> appearance;
  bool geometry_only = true;
  // Source projection is approximate appearance, never a generated PBR claim.
  nlohmann::json materials = {{"mode", "geometry_only"}, {"pbr_validated", false}};
  Validation validation = Validation::Unknown;
};
pixel3d::Mesh load_geometry_glb(const std::filesystem::path& path);
// Compare oriented F32 triangle-position multisets, independently of UV seam
// vertices and index order. Does not weld, discard or mutate any geometry.
bool geometrically_identical(const pixel3d::Mesh& a, const pixel3d::Mesh& b);
std::shared_ptr<const pixel3d::TexturedMesh> load_generated_appearance(const std::filesystem::path& path, bool allow_mipmap_filter = false);
// Describes retained GLB material data; the native preview displays base color
// only. Unsupported appearance combinations throw rather than silently change.
nlohmann::json material_info(const std::filesystem::path& path, bool allow_mipmap_filter = false);
void write_geometry_binary(const pixel3d::Mesh& mesh, const std::filesystem::path& path);
pixel3d::Mesh read_geometry_binary(const std::filesystem::path& path);
nlohmann::json inspect_geometry(const pixel3d::Mesh& mesh);
Validation classify_validation(const nlohmann::json& topology, const nlohmann::json& intersections);
const char* validation_name(Validation status);
}  // namespace p3dasset
