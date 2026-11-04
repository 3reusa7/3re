#pragma once
// User part masks (spec M4 §2). Each user part has its own mask per view, in
// aligned-pixel coordinates; masks of different parts may overlap (in an
// orthographic view parts hide behind each other — fixture C's two front legs
// share their Side pixels). The source artwork is never touched. A user part
// either labels existing geometry (split off the parts it overlaps) or defines
// its geometry (intersection of its mask prisms with the visual hull) and
// claims the automatic candidates inside its masks.
#include <map>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/correspond/candidates.hpp"
#include "pixel3d/image/mask.hpp"
#include "pixel3d/polygon/simplify.hpp"
#include "pixel3d/reconstruct/part_geometry.hpp"
#include "pixel3d/views/source_view.hpp"

namespace pixel3d {

struct UserPart {
  bool define = false;                // define geometry (claims candidates) instead of labelling it
  std::optional<double> depth;        // SINGLE VIEW: depth of this part (default: the project depth)
  std::map<ViewRole, Mask2D> masks;   // aligned-view pixels belonging to the part
  bool operator==(const UserPart&) const = default;
};

struct PartMasks {
  std::map<int, UserPart> parts;  // user part ids (≥ kFirstUserPart)
  bool empty() const { return parts.empty(); }
  // Views where `part` has at least one pixel.
  std::vector<ViewRole> views_of(int part) const;
  bool operator==(const PartMasks&) const = default;
};

nlohmann::json part_masks_to_json(const PartMasks& m);  // masks run-length encoded
Expected<PartMasks> part_masks_from_json(const nlohmann::json& j);

// Mask polygons (feature-preserving, like the drawings).
Polygons2D mask_polygons(const Mask2D& m, const SimplifyOptions& opt);

// Candidates claimed by "define" parts: their projection in some view where
// the part has a mask lies (≥ 90 %) inside that mask.
std::vector<int> claimed_candidates(const PartMasks& masks, const CandidateSet& cs, Dims d);

// Adds the user parts to `g`: define parts first (mask prisms ∩ hull), then
// label parts (split off whatever geometry lies inside their mask prisms).
// `hull` is the intersection of every drawing's full prism.
void add_user_parts(PartGeometry& g, const PartMasks& masks, const manifold::Manifold& hull, Dims d,
                    const SimplifyOptions& opt, std::vector<Message>* notes);

// Per user part and view: mask pixels its geometry does not explain.
struct MaskConflict {
  int part = -1;
  ViewRole view = ViewRole::Front;
  int unmatched_px = 0;
  int mask_px = 0;
  Mask2D::Box box;
  Mask2D unmatched;
};
std::vector<MaskConflict> mask_conflicts(const PartMasks& masks, const Mesh& solid, Dims d);

// Projects `part`'s geometry into every view where it has no mask (only
// opaque pixels are written). Returns the views changed.
std::vector<ViewRole> sync_part_masks(PartMasks& masks, int part, const Mesh& solid, Dims d,
                                      const std::map<ViewRole, Mask2D>& opaque);

}  // namespace pixel3d
