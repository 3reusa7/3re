#pragma once
// Layout: from typed parts to tubes, chains, branches and a skeleton.
//
// The trunk is one vertical loft through the body (torso/cloth) and the head,
// with neck loops. Adjacent limb parts form chains (one continuous loft each:
// shoulder → hand, hip → foot), attached to the trunk. Accessories and extra
// cloth are branches: separate closed shells that overlap their parent and are
// rigid on the parent's bone. Skinning ownership (segments, bones) is separate
// from build parts: in the humanoid and cloaked templates a one-part limb is
// split into two bones at its narrowest point (an INFERRED joint).
#include <string>
#include <vector>

#include "pixel3d/lowpoly/loft.hpp"
#include "pixel3d/lowpoly/profile.hpp"
#include "pixel3d/lowpoly/spine.hpp"
#include "pixel3d/lowpoly/types.hpp"

namespace pixel3d::lowpoly {

struct Segment {
  int part = -1;
  int bone = -1;
  double s0 = 0, s1 = 0;  // spine arc length range
};

struct JointSpec {
  double s = 0;                // centre of the joint along the spine
  int parent_bone = -1, child_bone = -1;
  bool attachment = false;     // a branch's base (the loops sit just outside the parent)
  bool inferred = false;       // placed by the template, not by a part boundary
  double width = 0;            // cross-section width at the joint (px)
};

// A round part (head) fitted with one symmetric ellipsoid; its rings are placed on
// regular latitudes instead of profile stations (no slice banding). World units.
struct RoundFit {
  int part = -1;
  bool ok = false;     // the Front mask is ellipse-like enough (IoU ≥ 0.88)
  double x0 = 0, y0 = 0, z0 = 0;
  double rx = 0, ry = 0, rz = 0;
  double iou = 0;
};

struct TubeSpec {
  std::string name;
  std::vector<int> parts;      // build parts in spine order
  PartType type = PartType::Torso;
  bool trunk = false, branch = false, chain = false;
  int parent_tube = -1;
  int partner = -1;            // symmetric tube (left/right) for depth fallback
  Spine spine;
  std::vector<Station> dense;  // every 0.5 px of spine, s from −extension to the tip
  double extension = 0;        // spine length inside the parent (branches)
  std::string depth_source;
  std::vector<Segment> segments;
  std::vector<JointSpec> joints;
  int rigid_bone = -1;         // tubes without bones of their own (accessories)
  double area_share = 0;       // share of the subject's Front pixels
  double type_weight = 1;
  vec2d attach;                // image point where a branch meets its parent
  std::vector<RoundFit> round_fits;
  std::vector<double> error_floor;  // per dense station, by its owner's type
};

struct Layout {
  Template templ = Template::GenericChain;
  std::vector<TubeSpec> tubes;
  Skeleton skeleton;
  std::vector<Message> messages;
  std::map<ViewRole, Mask2D> subject;  // subject masks used (alpha or external)
};

double type_weight(PartType t);
double type_roundness(PartType t);
// Minimum depth/width of a part's cross-sections when depth is inferred: head 0.95,
// torso and cloth 0.7, limbs 1.0 (round), elongated accessories (horns, cones) 1.0,
// flat accessories 0.3.
double type_min_depth_ratio(PartType t, bool elongated);
// Profile deviation (px) not worth a ring: cloth 1.0, torso 0.8, limb 0.4, head and
// accessories 0.35 — spare triangles go where they show.
double type_error_floor(PartType t);

// Errors: lowpoly.no_parts, lowpoly.empty_part.
Expected<Layout> build_layout(const Input& in);

// Bone that owns arc length s of a tube (rigid bone for accessories; the
// parent's bone inside a branch's extension).
int bone_at(const Layout& l, const TubeSpec& t, double s);

}  // namespace pixel3d::lowpoly
