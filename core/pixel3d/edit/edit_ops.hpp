#pragma once
// Geometry edits (spec M4 §1). An edit is a small value stored with the project
// inputs; applying the list in order to the reconstructed PartGeometry gives
// the edited model. Every op is exact CSG on closed solids, so the result stays
// a watertight TRUE SOLID. Ops that create parts carry the new part id, decided
// when the command was issued, so replaying the list is deterministic.
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/polygon/contour.hpp"
#include "pixel3d/reconstruct/part_geometry.hpp"

namespace pixel3d {

// Region of a view (aligned pixels) swept between two depths along the view.
// add: united into `part` (or into a new part `new_part` when part < 0).
// erase: carved from `part`, or from every part when part < 0.
struct VolumeEdit {
  std::string label = "volume";  // "add_volume" | "erase_volume" | "extrude"
  bool add = true;
  ViewRole view = ViewRole::Front;
  Polygons2D region;
  double depth_from = 0, depth_to = 1;
  int part = -1;
  int new_part = -1;
};

// A planar polygon (in the plane's u/v coordinates around `origin`) swept along
// the plane normal by `distance`: > 0 pulls material out (united into part),
// < 0 pushes in (carved from part, or from every part when part < 0).
struct SweepEdit {
  std::string label = "push_pull";
  int part = -1;
  std::array<double, 3> origin{}, u{}, v{}, normal{};
  Polygons2D polygon;
  double distance = 0;
};

// Axis-aligned plane x/y/z = at. keep: -1 the negative side, +1 the positive
// side, 0 both (the positive side becomes part new_part).
struct CutEdit {
  int part = -1;  // -1: every part (keep must be ±1)
  int axis = 0;
  double at = 0;
  int keep = 0;
  int new_part = -1;
};

struct MergeEdit {
  int into = -1;
  std::vector<int> parts;  // merged into `into`
};

// Mirror across the plane axis = at: in place, or as a copy (part new_part).
struct MirrorEdit {
  int part = -1;  // -1: every part (copy not allowed)
  int axis = 0;
  double at = 0;
  bool copy = false;
  int new_part = -1;
};

// Scale (uniform), then rotate about an axis, then translate, all about `pivot`.
// copy: the result becomes part new_part and the original stays (duplicate).
struct TransformEdit {
  int part = -1;
  std::array<double, 3> translate{0, 0, 0};
  int rotate_axis = 1;
  double rotate_deg = 0;
  double scale = 1;
  std::array<double, 3> pivot{0, 0, 0};
  bool copy = false;
  int new_part = -1;
};

struct DeletePartEdit {
  int part = -1;
};

// Every disconnected shell of `part` after the first becomes its own part.
struct SplitShellsEdit {
  int part = -1;
  std::vector<int> new_parts;  // ids for shells 2, 3, … (by decreasing volume)
};

// Depth Brush result: per pixel of a view, a depth offset relative to the
// surface under the pixel (> 0 raises the surface toward the camera, < 0
// pushes it in). Offsets are in source pixels, already snapped by the command.
struct DepthSample {
  int u = 0, v = 0;
  double offset = 0;
};
struct DepthStrokeEdit {
  ViewRole view = ViewRole::Front;
  std::vector<DepthSample> samples;
  int part = -1;  // restrict to one part (-1: the part under each pixel)
};

using EditOp = std::variant<VolumeEdit, SweepEdit, CutEdit, MergeEdit, MirrorEdit, TransformEdit, DeletePartEdit,
                            SplitShellsEdit, DepthStrokeEdit>;

std::string edit_name(const EditOp& op);
nlohmann::json edit_to_json(const EditOp& op);
Expected<EditOp> edit_from_json(const nlohmann::json& j);
// Range and size checks (shared by the API and project loading).
std::optional<Message> check_edit(const EditOp& op);
// Part ids an op creates (to keep the document's id counter ahead of them).
std::vector<int> created_parts(const EditOp& op);

// Applies one edit. Errors (unknown part, empty result) leave `g` unchanged.
// `notes` receives informational messages (e.g. an edit that changed nothing).
std::optional<Message> apply_edit(PartGeometry& g, const EditOp& op, Dims d, std::vector<Message>* notes);

}  // namespace pixel3d
