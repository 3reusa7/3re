#pragma once
// LOW-POLY STUDIO (spec 2026-10-12-lowpoly-studio-design.md): a game-ready,
// rig-friendly low-poly character built part by part from the drawings. The
// triangle budget is a hard cap on the exported triangles; nothing is ever
// decimated. Geometry carries the silhouette and the joints, the texture
// carries the detail. Depth from one view, hidden sides and template joints
// are INFERRED and reported as such.
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/common/math.hpp"
#include "pixel3d/image/image.hpp"
#include "pixel3d/image/mask.hpp"
#include "pixel3d/image/palette.hpp"
#include "pixel3d/views/source_view.hpp"
#include "pixel3d/views/view_role.hpp"

namespace pixel3d::lowpoly {

enum class PartType { Head, Torso, Limb, Accessory, Cloth };
std::string_view to_string(PartType t);
std::optional<PartType> part_type_from_string(std::string_view s);

enum class Template { Auto, CloakedCharacter, Humanoid, GenericChain };
std::string_view to_string(Template t);
std::optional<Template> template_from_string(std::string_view s);

enum class Style { Flat, Cel, Outline, CelOutline };
std::string_view to_string(Style s);
std::optional<Style> style_from_string(std::string_view s);

// One part as the builder sees it: a user part (masks in aligned view pixels)
// with its type. Masks of different parts may overlap.
struct Part {
  int id = 0;
  PartType type = PartType::Torso;
  std::string name;
  std::map<ViewRole, Mask2D> masks;
  std::optional<double> depth;        // front only: the user's depth for this part (source pixels)
  std::optional<double> depth_ratio;  // front only: minimum depth/width of its cross-sections (default: by type)
};

// Smart Pipeline hook (a): the subject (character) mask of a view may come from
// outside, e.g. a cutout model. Default: the drawing's alpha.
struct SubjectMask {
  Mask2D mask;
  std::string provenance = "alpha";
};

// Smart Pipeline hook (b): a relative thickness cue for one part, in the
// aligned pixels of `view` (values in [0, 1], 1 = thickest). It modulates the
// distance-transform radius: r = r_dt · lerp(1, cue / mean_cue, weight).
// Evidence of shape, not an observation: depth from a cue stays INFERRED.
struct DepthCue {
  ViewRole view = ViewRole::Front;
  int w = 0, h = 0;
  std::vector<float> values;
  double weight = 1.0;
  std::string provenance;
};

struct Settings {
  int tri_budget = 1000;  // hard cap on exported triangles (kMinBudget … kMaxBudget)
  int joint_loops = 2;    // edge loops per joint (1–3)
  Style style = Style::CelOutline;
  int tones = 3;          // cel ramp tones (2 or 3)
  Template templ = Template::Auto;
  bool baked_cel = false;  // bake the cel ramp into the texture (fixed light; for engines without shaders)
  bool hull = false;       // inverted-hull outline shells (count toward the budget)
  double hull_width = 0.6; // source pixels
  // Front only: total depth in source pixels; 0 = automatic (each part by its type's
  // depth/width minimum). A user depth clamps the inflation.
  double depth = 0;
  bool user_depth = false;  // the depth was chosen by the user (front only: lifts lowpoly.inferred_depth)
  // Front only: the user accepts inferred depth for batch work and export. The status stays
  // NEEDS_REVIEW (inferred depth is never evidence); the acceptance is written in the report.
  bool accept_inferred_depth = false;
  int min_sides = 4, max_sides = 8;
  // Opposite projection on head and cloth faces (the back of a head would show the face).
  bool opposite_projection_head_cloth = false;
  int atlas_max = 1024;
  bool operator==(const Settings&) const = default;
};

inline constexpr int kMinBudget = 500;
inline constexpr int kMaxBudget = 5000;
inline constexpr int kLowBudgetWarning = 750;

struct Input {
  std::vector<SourceView> views;  // aligned
  Dims dims;
  uint8_t alpha_threshold = 128;
  std::vector<Part> parts;
  Settings settings;
  std::map<ViewRole, SubjectMask> subject_masks;  // optional, per view
  std::map<int, DepthCue> depth_cues;              // optional, per part id
};

// ---- results ----------------------------------------------------------------

struct Bone {
  std::string name;
  int parent = -1;
  vec3d head, tail;  // rest pose, world units
  int part = -1;     // owning part id
};

struct Skeleton {
  std::vector<Bone> bones;
  int find(std::string_view name) const;
};

// Render/export mesh with skinning. World units; glTF axes (+Y up, front +Z).
struct SkinnedMesh {
  std::vector<vec3d> positions, normals;
  std::vector<vec2d> uvs;  // glTF convention (0,0) = top-left
  std::vector<uint32_t> indices;
  std::vector<std::array<uint16_t, 4>> joints;
  std::vector<std::array<float, 4>> weights;
  std::vector<int> vertex_shell;   // tube / shell index per vertex
  std::vector<int> vertex_part;    // owning part id per vertex
  std::vector<bool> vertex_joint_loop;  // the vertex lies on a joint loop
  std::vector<int> tri_shell;
  bool hull_appended = false;
  size_t hull_first_index = 0;     // first index of the inverted hull (when appended)
  size_t triangle_count() const { return indices.size() / 3; }
};

struct TubeReport {
  std::string name;
  std::vector<int> parts;
  bool branch = false;
  int parent_tube = -1;
  int sides = 0, rings = 0, joints = 0, joint_loops = 0, triangles = 0;
  double profile_error_px = 0;
  std::string depth_source;  // "side", "top", "inflation", "partner"
};

struct PartReport {
  int part = 0;
  std::string name;
  PartType type = PartType::Torso;
  int triangles = 0;
  double importance = 0;
  double depth_ratio = 0;    // median depth/width of the part's cross-sections
  std::string depth_source;  // side, top, inflation, mirror:...
};

}  // namespace pixel3d::lowpoly
