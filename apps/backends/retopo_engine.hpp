#pragma once
// 3re Smart Retopology (native, no Blender): the QuadWild-BiMDF engine runs as two external programs
// (quadwild.exe, quad_from_patches.exe, GPL-3.0, tools/smartquad/QUADWILD.md); this side prepares its
// input, reads its quads back, measures the result against the source surface and moves the source
// colours onto the new mesh. Qt-free so the worker and the tests need no GUI.
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace p3dasset::retopo {
using pixel3d::vec3d;

// Polygon mesh as QuadWild writes it: quads, plus triangles where a patch needs one.
struct PolyMesh {
  std::vector<vec3d> positions;
  std::vector<std::vector<uint32_t>> faces;
};

PolyMesh read_obj(const std::filesystem::path& path);  // v / f only; throws on anything malformed
void write_obj(const PolyMesh& mesh, const std::filesystem::path& path);
PolyMesh to_poly(const pixel3d::Mesh& mesh);
pixel3d::Mesh triangulate(const PolyMesh& mesh);  // quad (a,b,c,d) -> (a,b,c) (a,c,d), as the editor draws it

// QuadWild needs connectivity. A triangle soup (no vertex shared) is welded at exactly equal positions;
// shared meshes only reconnect exact opposite boundary pairs inside the same connected shell.
// Disconnected shells and nonzero gaps remain separate; appearance UVs are not modified.
pixel3d::Mesh engine_input(const pixel3d::Mesh& mesh);

// Quadric-error edge collapse (Garland-Heckbert) down to about `target` triangles: open borders are kept
// (heavily weighted), collapses that would fold a face over or break manifold connectivity are refused.
// Only the engine's input is simplified (QuadWild re-samples it to ~10-30k faces anyway); the result is
// always measured against the full model.
pixel3d::Mesh simplify(const pixel3d::Mesh& mesh, size_t target);

// Handles (tunnels) of a mesh: (2 * components - (V - E + F)) / 2 over its edges, for a closed mesh its genus.
int handles(const pixel3d::Mesh& mesh);

// Outer closed shell by morphological closing at radius `edge`: the outside is flood-filled from far away
// without coming closer than `edge` to the surface, its border surface (the model dilated by `edge`) is built
// as a manifold level set and moved back in by `edge`. Tunnels and gaps narrower than about 2 x `edge`
// close, internal sheets and cavities disappear, the outer shape comes back within about edge / 2 (concave
// corners round off by `edge`). Used for engine input only, when a generated mesh has many tiny tunnels
// (TRELLIS: hundreds) that QuadWild cannot trace patches around.
pixel3d::Mesh closed_shell(const pixel3d::Mesh& mesh, double edge);

// Closest point on a triangle mesh (uniform grid over the triangles).
class SurfaceIndex {
 public:
  explicit SurfaceIndex(const pixel3d::Mesh& mesh);
  struct Hit {
    int tri = -1;
    std::array<double, 3> bary{};
    double distance = 0;
  };
  Hit closest(vec3d p) const;
  double diagonal() const { return diagonal_; }

 private:
  const pixel3d::Mesh& mesh_;
  vec3d lo_, cell_size_;
  std::array<int, 3> dims_{};
  std::vector<uint32_t> start_, items_;
  double diagonal_ = 1;
  size_t cell(int x, int y, int z) const { return (size_t(z) * dims_[1] + y) * dims_[0] + x; }
};

// Counts, quad share, open/non-manifold edges and the two-sided surface distance (mean, p95, max,
// relative to the source bounding-box diagonal) on `samples` points per side.
nlohmann::json measure(const pixel3d::Mesh& source, const PolyMesh& result, size_t samples = 60000);

// Target quad size for QuadWild's flow stage: the edge-length factor that gives about `target_faces`
// quads on a surface of `area` whose prepared mesh has average edge `edge`.
double quad_scale(double area, double edge, int target_faces);

// UV atlas over the new mesh and its base colour taken from the closest point of the source surface.
// No source appearance: a plain light-grey texture (the geometry is still exported and checked).
pixel3d::TexturedMesh transfer_appearance(const pixel3d::Mesh& target, const pixel3d::TexturedMesh* source, int size,
                                          double seconds, nlohmann::json& evidence, bool nearest = false);

}  // namespace p3dasset::retopo
