#pragma once
// The project state. A Document is a value: heavy members are immutable and
// shared (std::shared_ptr<const T>), so snapshots for undo/checkpoints are cheap.
// Members are filled stage by stage; a mutation clears everything downstream.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pixel3d/analysis/conflicts.hpp"
#include "pixel3d/analysis/recess.hpp"
#include "pixel3d/correspond/evidence.hpp"
#include "pixel3d/correspond/solve.hpp"
#include "pixel3d/edit/edit_ops.hpp"
#include "pixel3d/edit/part_masks.hpp"
#include "pixel3d/edit/single_view.hpp"
#include "pixel3d/image/normalize.hpp"
#include "pixel3d/image/palette.hpp"
#include "pixel3d/lowpoly/project_settings.hpp"
#include "pixel3d/mesh/cleanup.hpp"
#include "pixel3d/polygon/simplify.hpp"
#include "pixel3d/reconstruct/depth_constraint.hpp"
#include "pixel3d/reconstruct/true_solid.hpp"
#include "pixel3d/segment/regions.hpp"
#include "pixel3d/style/styled_model.hpp"
#include "pixel3d/texture/projector.hpp"
#include "pixel3d/validate/metrics.hpp"
#include "pixel3d/validate/quality_gate.hpp"
#include "pixel3d/views/align.hpp"

namespace pixel3d {

struct LowpolyBuild;  // engine/lowpoly_stage.hpp

enum class Source { Gui, Mcp, Cli };
std::string_view to_string(Source s);

enum class ReconstructMode { TrueSolid, Voxel };
std::string_view to_string(ReconstructMode m);

struct ProjectSettings {
  std::string name = "untitled";
  uint8_t alpha_threshold = 128;
  double pixels_per_meter = 32;
  ToleranceProfile profile = ToleranceProfile::Hand;
  SimplifyOptions simplify;
  NormalizeSettings normalize;  // input normalisation (image/normalize.hpp)
  StyleSettings style;          // polished styles (style/style_settings.hpp); crisp = unchanged
  LowpolyProjectSettings lowpoly;  // LOW-POLY STUDIO mode and build settings (lowpoly/project_settings.hpp)
};

struct Document {
  ProjectSettings settings;
  std::vector<SourceView> views;
  AlignOptions align_options;
  ReconstructMode mode = ReconstructMode::TrueSolid;
  CleanupOptions cleanup_options;
  TextureOptions texture_options;
  // User / agent decisions (small, copied).
  std::vector<UserDecision> decisions;
  std::vector<DepthConstraint> depth_constraints;
  std::vector<TextureAssignment> texture_assignments;
  // M4 editor inputs.
  std::vector<EditOp> edits;               // applied in order to the reconstructed parts
  PartMasks masks;                         // user parts drawn on the views
  std::map<int, std::string> part_names;   // display names (any part id)
  std::map<int, lowpoly::PartType> part_types;  // LOW-POLY STUDIO part types (user part ids)
  int next_part_id = kFirstUserPart;       // ids for parts created by the user or by edits
  SingleViewSettings single_view;
  // Derived, stage by stage.
  std::shared_ptr<const AlignResult> align;
  std::shared_ptr<const MaterialFamilies> families;
  std::shared_ptr<const std::map<ViewRole, RegionMap>> regions;
  std::shared_ptr<const BandSet> bands;
  std::shared_ptr<const CandidateSet> candidates;
  std::shared_ptr<const std::vector<CandidateEvidence>> evidence;
  std::shared_ptr<const CorrespondenceResult> correspondence;
  std::shared_ptr<const SolidResult> solid;
  // Editable geometry: base = reconstruction + user parts + depth constraints;
  // edited = base with edits[0 .. edits_applied) applied (solid is its mesh).
  std::shared_ptr<const PartGeometry> base_parts;
  std::shared_ptr<const PartGeometry> edited_parts;
  size_t edits_applied = 0;
  std::shared_ptr<const VoxelGrid> volume;  // kept cells after depth constraints
  std::shared_ptr<const std::vector<RecessFinding>> recesses;
  std::shared_ptr<const TextureResult> texture;
  // Live checks of the current mesh (after every texture stage, so after every edit).
  std::shared_ptr<const QualityMetrics> live;
  std::shared_ptr<const std::vector<MaskConflict>> mask_conflicts;
  std::shared_ptr<const QualityMetrics> metrics;
  std::shared_ptr<const std::vector<Conflict>> conflicts;
  std::shared_ptr<const GateResult> gate;
  // Polished model (style other than crisp), built at validation from the crisp
  // parts and texture; it is what is validated and exported. The last one built
  // is kept and reused while its style_input_hash matches (undo, style switches).
  std::shared_ptr<const StyledModel> styled;
  std::shared_ptr<const StyledModel> cached_styled;
  // Mesh cache restored from a project file; reused while mesh_input_hash matches.
  std::shared_ptr<const SolidResult> cached_solid;
  std::string cached_input_hash;
  // LOW-POLY STUDIO output (derived; see engine/lowpoly_stage.hpp). Dropped when its inputs change.
  std::shared_ptr<const LowpolyBuild> lowpoly;
  uint64_t revision = 0;
};

}  // namespace pixel3d
