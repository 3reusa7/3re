#pragma once
// Deterministic CPU rasterizer used for validation renders, previews and MCP
// images (no GPU needed). Vertices snap to a 1/256-pixel grid and coverage
// uses exact integer edge functions with the top-left rule, so triangles that
// share an edge never leave gaps or double-cover a pixel.
#include <array>
#include <memory>
#include <vector>

#include "pixel3d/image/image.hpp"
#include "pixel3d/image/mask.hpp"
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/render/camera.hpp"

namespace pixel3d {

// Render/export mesh: one UV and normal per vertex, glTF UV convention
// (u right, v down; (0,0) = top-left texel corner).
struct TexturedMesh {
  std::vector<vec3d> positions;
  std::vector<vec3d> normals;
  std::vector<vec2d> uvs;
  std::vector<uint32_t> indices;
  std::shared_ptr<const ImageRGBA> texture;
  std::vector<int> tri_part;
  // Polished styles only (empty for crisp): a second UV set where every
  // surface point owns its texels, chart tangents (xyz, handedness w), and the
  // maps baked into it. The validation renderer ignores all of these.
  std::vector<vec2d> uvs1;
  std::vector<std::array<double, 4>> tangents;
  std::shared_ptr<const ImageRGBA> normal_map;  // tangent space, RGB = (n + 1) / 2
  std::shared_ptr<const ImageRGBA> occlusion;   // R = ambient occlusion (glTF occlusionTexture)
};

struct RenderOptions {
  bool textured = true;
  bool smooth_texture = false;  // bilinear lookup (painted textures); default nearest (pixel art, validation)
  bool lit = false;
  Rgba8 background{0, 0, 0, 0};
  vec3d light_dir{-0.4, 0.8, 0.45};
};

struct RenderTarget {
  ImageRGBA color;
  std::vector<float> depth;  // +inf where empty
  Mask2D coverage;
  std::vector<int> tri_id;   // -1 where empty
};

RenderTarget render(const TexturedMesh& m, const Camera& cam, const RenderOptions& opt);
RenderTarget render_mesh_flat(const Mesh& m, const Camera& cam);  // coverage, depth, tri_id; grey colour

}  // namespace pixel3d
