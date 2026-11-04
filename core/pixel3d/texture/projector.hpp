#pragma once
// Pixel-perfect texture projection (spec §5.7). Every face takes UVs by exact
// orthographic projection from the most frontal source view that actually sees
// it, so each surface point samples the very source pixel it came from. Faces no
// view sees are INFERRED and get, in order: Opposite Projection (only when the
// pixels it would use are opaque art of the face's own material), Edge Extend
// (stretch the edge texels of a seen neighbour), or Flat Palette Color.
// No new artwork is ever painted.
#include <map>
#include <string_view>
#include <vector>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/render/rasterizer.hpp"
#include "pixel3d/texture/atlas.hpp"

namespace pixel3d {

enum class SurfaceMode { Seen, OppositeProjection, EdgeExtend, FlatPalette, Manual };
std::string_view to_string(SurfaceMode m);

struct TextureOptions {
  bool allow_opposite_projection = true;
  int visibility_samples = 16;       // samples per face for material/opacity checks
  // Share of a planar region's projected area a view must show. Half, not all: a
  // face partly hidden under another part (B's cushion under the slats) is still
  // that view's surface; its hidden strip is invisible in every supplied view.
  double visibility_fraction = 0.5;
};

// Client override for a set of triangles (indices into Mesh::tris). Triangle
// indices only mean something for one mesh: `mesh_key` (the mesh input hash at
// assignment time) binds them to it, and they are ignored once the mesh changes.
struct TextureAssignment {
  std::vector<int> tris;
  ViewRole view = ViewRole::Front;
  SurfaceMode mode = SurfaceMode::Manual;  // FlatPalette → swatch; anything else → projection from `view`
  std::string mesh_key;                     // set by Session::assign_texture
};

struct FaceTexture {
  ViewRole view = ViewRole::Front;
  SurfaceMode mode = SurfaceMode::Seen;
  bool inferred = false;
};

struct TextureResult {
  TexturedMesh mesh;               // face f of `mesh` is triangle f of the input Mesh
  Atlas atlas;
  std::vector<FaceTexture> faces;
  std::map<SurfaceMode, double> area_by_mode;
  double texel_density_min = 0, texel_density_max = 0;  // texels per unit over projected faces
  std::vector<Message> messages;
};

// Manual assignments naming a view without source art, or triangles that do not
// exist, are skipped and reported (texture_assignment.skipped).
TextureResult project_textures(const Mesh& mesh, const std::vector<SourceView>& views, const MaterialFamilies& fam,
                               Dims d, const TextureOptions& opt, const std::vector<TextureAssignment>& manual,
                               uint8_t alpha_threshold = 128);

}  // namespace pixel3d
