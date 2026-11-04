#pragma once
// Selection and picking (spec M4 §3), shared by the GUI and MCP: a pixel of a
// view or a 3D ray resolves to a triangle, its part and its planar face; the
// selection projects back into every view as a mask.
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/image/mask.hpp"
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/polygon/contour.hpp"
#include "pixel3d/views/view_role.hpp"

namespace pixel3d {

enum class SelectMode { Object, Part, Face };
std::string_view to_string(SelectMode m);
std::optional<SelectMode> select_mode_from_string(std::string_view s);

struct PickHit {
  int tri = -1;  // -1: nothing hit
  int part = -1;
  int face = -1;  // planar region
  vec3d point{0, 0, 0};
  double distance = 0;
};

// First triangle along the ray (both windings, so a camera inside still hits).
PickHit pick_ray(const Mesh& m, vec3d origin, vec3d dir);
// The surface a view shows at pixel (u, v): a ray from the camera side through the pixel centre.
PickHit pick_view_pixel(const Mesh& m, ViewRole r, Dims d, int u, int v);

// A planar face of the mesh as a polygon in its own plane: n = u × v.
struct FacePlane {
  vec3d origin{0, 0, 0}, u{1, 0, 0}, v{0, 1, 0}, normal{0, 0, 1};
  Polygons2D polygon;  // in (u, v) coordinates around origin
  int part = -1;       // the part most of its triangles belong to
  double area = 0;
};
std::optional<FacePlane> face_plane(const Mesh& m, int face);

struct Selection {
  SelectMode mode = SelectMode::Part;
  std::vector<int> parts;  // object mode: every part
  int face = -1;           // face mode
  std::optional<PickHit> hit;
  uint64_t revision = 0;   // document revision the selection was made on
  bool empty() const { return parts.empty() && face < 0; }
};

// The selection drawn into each view (triangles of the selected parts / face).
std::map<ViewRole, Mask2D> selection_masks(const Mesh& m, const Selection& s, Dims d, const std::vector<ViewRole>& roles);

nlohmann::json selection_json(const Selection& s, const Mesh& m, Dims d, const std::vector<ViewRole>& roles);

}  // namespace pixel3d
