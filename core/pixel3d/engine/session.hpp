#pragma once
// Session: the single engine API shared by the CLI, the MCP server and the GUI.
// Every document mutation is a transaction: it works on a copy of the current
// Document and is committed (and logged with its source) only on success, so a
// failed or cancelled call never changes the project. No exception escapes any
// public member: failures, including out-of-memory, come back as OpResult errors.
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "pixel3d/edit/brush.hpp"
#include "pixel3d/edit/selection.hpp"
#include "pixel3d/engine/history.hpp"
#include "pixel3d/engine/op_result.hpp"
#include "pixel3d/engine/pipeline.hpp"
#include "pixel3d/io/export_presets.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/render/beauty.hpp"

namespace pixel3d {

struct ViewImport {
  ViewRole role = ViewRole::Front;
  std::filesystem::path path;
  std::optional<bool> flip_x;  // force mirroring; unset = automatic
};

struct ReconstructOptions {
  ReconstructMode mode = ReconstructMode::TrueSolid;
};

struct PartEdit {
  std::vector<UserDecision> decisions;  // keep / remove candidate ids (see get_conflicts)
};

// Partial settings change; unset members keep their value.
// Changes to the input normalisation settings (unset = unchanged).
struct NormalizeUpdate {
  std::optional<bool> enabled;
  std::optional<int> target_size;
  std::optional<int> bg_tolerance;
  std::optional<double> axis_tolerance;
  bool empty() const { return !enabled && !target_size && !bg_tolerance && !axis_tolerance; }
  NormalizeSettings applied_to(NormalizeSettings s) const {
    if (enabled) s.enabled = *enabled;
    if (target_size) s.target_size = *target_size;
    if (bg_tolerance) s.bg_tolerance = *bg_tolerance;
    if (axis_tolerance) s.axis_tolerance = *axis_tolerance;
    return s;
  }
};

struct SettingsUpdate {
  std::optional<std::string> name;
  std::optional<double> pixels_per_meter;
  std::optional<ToleranceProfile> profile;
  NormalizeUpdate normalize;
  std::optional<StyleSettings> style;  // the whole style (preset, tuning, materials) or unchanged
};

struct RenderRequest {
  std::vector<ViewRole> views;  // empty = every source view
  int pixels_per_unit = 8;      // the image is view size × pixels_per_unit (≤ limits::kMaxRenderSide)
  bool lit = false;
  // Also return "<role>_compare": source vs 1× re-render. Grey = match, red =
  // rendered but not drawn, blue = drawn but not rendered, yellow = colour differs.
  bool compare = false;
  // Beauty: presentation renders (render_beauty) from each view's direction,
  // `beauty_size` px square; never used by any check. compare needs validation.
  RenderStyle style = RenderStyle::Validation;
  int beauty_size = 512;
};

struct OrbitRequest {
  double yaw_deg = 30;  // 0 = from the front (+Z), 90 = from the right (+X)
  double elevation_deg = 20;
  int size = 512;
  bool lit = true;
  RenderStyle style = RenderStyle::Validation;  // beauty ignores `lit`
};

struct TurntableRequest {
  int frames = 24;
  double elevation_deg = 20;
  int size = 256;
  bool contact_sheet = true;
  RenderStyle style = RenderStyle::Validation;
};

struct NamedImage {
  std::string name;
  ImageRGBA image;
};

// Validation renders written to disk. Every requested file is either written
// or reported as an error; the result is a failure if any write failed.
struct ArtifactRequest {
  std::filesystem::path dir;
  bool views = true;        // views/<role>_1x.png and views/<role>_8x.png
  bool rotations = true;    // rotations/yaw_090.png, yaw_180.png, yaw_270.png
  bool turntable = false;   // turntable/frame_XX.png + contact_sheet.png
};

// GLB export. Scale and orientation always come from the project settings, so
// every client exports the same file. The document is validated first; a model
// that is not PASS is exported only when the caller accepts its status
// explicitly (the result then carries a warning), so NEEDS_REVIEW or FAIL can
// never pass silently.
struct GlbExportRequest {
  std::filesystem::path path;  // empty = build and verify only
  bool allow_needs_review = false;
  bool allow_fail = false;
};

// Any format and engine preset (spec M5 §1). The format comes from `format`,
// else the path's extension, else the preset; units and front override the
// preset. glTF writes <name>.gltf + <name>.bin + <name>.png, OBJ <name>.obj +
// <name>.mtl + <name>.png, all in the path's folder. Same honesty rule as GLB.
struct ModelExportRequest {
  std::filesystem::path path;  // empty = build and verify only
  std::optional<ExportFormat> format;
  EnginePreset preset = EnginePreset::Generic;
  std::optional<ExportUnits> units;
  std::optional<bool> front_negative_z;
  bool unlit = false;
  bool allow_needs_review = false;
  bool allow_fail = false;
};

// ---- M4 editor requests -------------------------------------------------------

struct DepthPaintRequest {
  ViewRole view = ViewRole::Front;
  std::vector<DepthSample> samples;  // explicit per-pixel offsets, or …
  std::vector<vec2d> stroke;         // … stroke points turned into samples with `brush`
  BrushParams brush;
  int part = -1;                     // restrict to one part
};

struct PushPullRequest {
  int face = -1;                     // planar face of the current mesh (see selection), or …
  std::optional<ViewRole> view;      // … the face a view shows at pixel (u, v)
  int u = -1, v = -1;
  double distance = 1;               // > 0 pulls out, < 0 pushes in (source pixels)
  bool snap = true;                  // whole pixels
  bool respect_bounds = true;        // stop at the drawings' box so projections stay intact
};

struct TransformRequest {
  int part = -1;
  vec3d translate{0, 0, 0};
  int rotate_axis = 1;
  double rotate_deg = 0;
  double scale = 1;
  std::optional<vec3d> pivot;  // default: the part's bounding-box centre
  bool snap = true;            // translation in whole pixels
  bool copy = false;           // duplicate
};

struct MaskPaintRequest {
  int part = -1;
  ViewRole view = ViewRole::Front;
  std::vector<std::pair<int, int>> pixels;  // explicit pixels, or …
  std::vector<vec2d> stroke;                // … a brush stroke
  BrushParams brush;
  bool erase = false;
};

// ---- LOW-POLY STUDIO requests (spec 2026-10-12-lowpoly-studio-design.md §6) ----

// Partial change of the low-poly settings; unset members keep their value.
struct LowpolyUpdate {
  std::optional<bool> enabled;  // project mode lowpoly_studio (re-aligns the views)
  std::optional<int> tri_budget, joint_loops, tones;
  std::optional<lowpoly::Style> style;
  std::optional<lowpoly::Template> templ;
  std::optional<bool> baked_cel, hull, accept_inferred_depth;
  std::optional<double> hull_width;
  std::optional<double> depth;  // front only: total depth in source px; 0 = automatic (by part type)
  bool empty() const {
    return !enabled && !tri_budget && !joint_loops && !tones && !style && !templ && !baked_cel && !hull &&
           !accept_inferred_depth && !hull_width && !depth;
  }
  LowpolyProjectSettings applied_to(LowpolyProjectSettings s) const;
};

// Type and per-part depth of one user part.
struct PartTypeUpdate {
  int part = -1;
  std::optional<lowpoly::PartType> type;
  std::optional<double> depth_ratio;  // minimum depth/width (front only); 0 = the type's default
};

struct LowpolySuggestRequest {
  bool apply = false;    // create the suggested parts (one undo step); false = proposals only
  bool replace = false;  // apply over existing user parts (they are removed first)
};

// Layered sources (.aseprite, .psd): their visible raster layers as suggested parts.
struct LayerPartsRequest {
  bool apply = false;               // create the accepted layers as user parts (one undo step)
  bool replace = false;             // apply over existing user parts (they are removed first)
  std::vector<std::string> accept;  // layer names or paths to create; empty = every suggested layer
};

struct LowpolyRenderRequest {
  std::vector<double> yaws{0, 90, 180, 270};  // 0 = front, 90 = from the right
  double elevation_deg = 10;
  int size = 384;
  bool pose = false;                           // the template's test pose instead of the rest pose
  std::optional<lowpoly::Style> style;         // default: the project's (baked_cel shown as baked)
  bool wire = false;                           // triangle edges
  bool contact_sheet = false;                  // also one image with every frame in a row
};

// The low-poly GLB (built and round-tripped at lowpoly_build) written to `path`.
// PASS is written; NEEDS_REVIEW only with allow_needs_review or when the user
// accepted inferred depth (accept_inferred_depth); FAIL never.
struct LowpolyExportRequest {
  std::filesystem::path path;
  bool allow_needs_review = false;
  bool engine_files = false;  // also <stem>_toon.gdshader, <stem>_outline.gdshader, <stem>_unity.md
};

class Session {
 public:
  explicit Session(Source source = Source::Cli);

  OpResult new_project(const ProjectSettings& settings);
  OpResult update_settings(const SettingsUpdate& u);
  OpResult save(const std::filesystem::path& path);
  // Writes the project file without logging or changing anything (autosave).
  OpResult save_copy(const std::filesystem::path& path) const;
  OpResult load(const std::filesystem::path& path);
  // `normalize` changes the normalisation settings in the same step (one undo).
  OpResult import_views(const std::vector<ViewImport>& views, const NormalizeUpdate& normalize = {});
  OpResult analyze();
  OpResult auto_align();
  OpResult segment_parts();
  OpResult match_parts();
  OpResult reconstruct(const ReconstructOptions& opt, const JobContext& job = {});
  OpResult get_conflicts() const;
  // A successful result describing the current document: revision, stats,
  // status and validation summary (for read-only queries).
  OpResult current_status() const;
  OpResult get_inferred_geometry() const;
  OpResult set_depth(const DepthConstraint& c);
  OpResult edit_part(const PartEdit& e);
  OpResult cleanup(const CleanupOptions& opt);
  OpResult project_textures(const TextureOptions& opt);
  OpResult assign_texture(const TextureAssignment& a);
  OpResult validate(const ValidateOptions& opt, const JobContext& job = {});
  OpResult render_views(const RenderRequest& req, std::vector<NamedImage>* out) const;
  OpResult render_orbit(const OrbitRequest& req, std::vector<NamedImage>* out) const;
  OpResult render_turntable(const TurntableRequest& req, std::vector<NamedImage>* out, const JobContext& job = {}) const;
  OpResult write_artifacts(const ArtifactRequest& req, const JobContext& job = {}) const;
  OpResult export_glb(const GlbExportRequest& req);
  OpResult export_model(const ModelExportRequest& req);

  // ---- M4 editing: each call is one transaction and one undo step ----
  // Appends an edit (part ids for new parts may be given as -1: they are allocated).
  OpResult apply_edit(EditOp op);
  OpResult paint_depth(const DepthPaintRequest& r);
  OpResult add_volume(ViewRole view, const Polygons2D& region, double depth_from, double depth_to, int part);
  OpResult erase_volume(ViewRole view, const Polygons2D& region, double depth_from, double depth_to, int part);
  // Region of a view moved out of (distance > 0) or into (< 0) the surface under it.
  OpResult extrude(ViewRole view, const Polygons2D& region, double distance, int part);
  OpResult push_pull(const PushPullRequest& r);
  OpResult cut(int part, int axis, double at, int keep);           // keep −1 / +1
  OpResult split_part(int part, int axis, double at);              // by a plane
  OpResult split_part_shells(int part);                            // by disconnected pieces
  OpResult merge_parts(const std::vector<int>& parts);             // into the first
  OpResult mirror(int part, int axis, std::optional<double> at, bool copy);  // default plane: the box centre
  OpResult transform_part(const TransformRequest& r);
  OpResult delete_part(int part);
  // Parts and masks
  OpResult create_part(const std::string& name, bool define);
  OpResult rename_part(int part, const std::string& name);
  OpResult paint_part_mask(const MaskPaintRequest& r);
  OpResult sync_part_masks(int part);
  // Result of an edit without committing it: live checks + the mesh (if `mesh`).
  OpResult preview_edit(const EditOp& op, Mesh* mesh = nullptr) const;
  // Live previews of the interactive tools: exactly what paint_depth / push_pull would commit.
  OpResult preview_paint_depth(const DepthPaintRequest& r, Mesh* mesh = nullptr) const;
  OpResult preview_push_pull(const PushPullRequest& r, Mesh* mesh = nullptr) const;
  // Selection (not undoable, not saved)
  OpResult select_at_pixel(ViewRole view, int u, int v, SelectMode mode);
  OpResult select_at_ray(vec3d origin, vec3d dir, SelectMode mode);
  OpResult select_parts(const std::vector<int>& parts, SelectMode mode = SelectMode::Part);
  OpResult clear_selection();
  OpResult get_selection() const;
  const Selection& selection() const { return selection_; }
  // SINGLE VIEW
  OpResult set_single_view(const SingleViewSettings& s);
  OpResult set_single_view_depth(const std::string& preset, std::optional<double> depth);
  // Aseprite workflow: re-read every view from the file it was imported from.
  OpResult reload_views();
  // Layered sources (.aseprite, .psd): visible raster layers as suggested part masks, named
  // after the layer, mapped into the aligned view pixels. Proposals unless apply=true.
  OpResult suggest_layer_parts(const LayerPartsRequest& r);
  // LOW-POLY STUDIO
  OpResult set_lowpoly_settings(const LowpolyUpdate& u);
  OpResult set_part_type(const PartTypeUpdate& u);
  OpResult lowpoly_suggest_parts(const LowpolySuggestRequest& r);
  // Builds (or reuses) the low-poly character with `u` applied first (one undo step).
  OpResult lowpoly_build(const LowpolyUpdate& u = {}, const JobContext& job = {});
  OpResult lowpoly_render(const LowpolyRenderRequest& r, std::vector<NamedImage>* out) const;
  OpResult lowpoly_export(const LowpolyExportRequest& r);

  OpResult undo();
  OpResult redo();
  OpResult create_checkpoint(const std::string& name);
  OpResult restore_checkpoint(const std::string& name);
  std::vector<std::string> checkpoint_names() const { return history_.checkpoint_names(); }
  bool can_undo() const { return history_.can_undo(); }
  const std::vector<std::string>& history_steps() const { return history_.steps(); }
  size_t history_cursor() const { return history_.cursor(); }
  bool can_redo() const { return history_.can_redo(); }

  // The current state. The reference is valid only until the next Session call;
  // keep snapshot() (cheap: heavy members are shared) to hold a state longer.
  const Document& document() const { return history_.current(); }
  Document snapshot() const { return history_.current(); }
  const std::vector<LogEntry>& log() const { return log_; }
  Source source() const { return source_; }

 private:
  enum class Commit { Push, Replace };
  // Builds an edit from the current document (may fail), then commits it.
  OpResult commit_edit(const std::string& name, nlohmann::json params,
                       const std::function<Expected<EditOp>(Document&)>& build);
  OpResult preview_built(const std::function<Expected<EditOp>(Document&)>& build, Mesh* mesh) const;
  OpResult apply(const std::string& name, nlohmann::json params, Commit commit,
                 const std::function<OpResult(Document&)>& fn);
  // Re-runs the stages the document had reached before an input changed.
  std::optional<Message> refresh(Document& d, pipeline::Stage reached, pipeline::Stage at_least, OpResult& r) const;
  void finish(OpResult& r) const;
  void log_op(const std::string& name, nlohmann::json params, const OpResult& r);

  Source source_;
  History history_;
  std::vector<LogEntry> log_;
  int reconstruct_count_ = 0;
  Selection selection_;
};

// Writes <dir>/<name>.png for every image. Each written file is listed in
// r.data["files"]; each failed write is an io.write_failed error and clears r.success.
void write_named_images(const std::filesystem::path& dir, const std::vector<NamedImage>& images, OpResult& r);

}  // namespace pixel3d
