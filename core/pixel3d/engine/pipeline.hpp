#pragma once
// Pipeline stages over a Document. Each stage assumes its prerequisites exist;
// ensure() runs whatever is missing up to a target stage. Algorithms live in
// their modules — this file only wires them together.
#include <functional>
#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

#include "pixel3d/engine/document.hpp"

namespace pixel3d {

struct JobContext {
  std::stop_token stop;
  std::function<void(float, std::string_view)> progress;
};

struct ValidateOptions {
  std::optional<ToleranceProfile> profile;  // defaults to the project setting
  bool glb_roundtrip = true;
};

namespace pipeline {

enum class Stage { Empty, Imported, Aligned, Segmented, Matched, Reconstructed, Textured, Validated };
std::string_view to_string(Stage s);

Stage stage_of(const Document& d);
// Clears the members computed by `s` and every later stage.
void clear_from(Document& d, Stage s);

using Notes = std::vector<Message>;
std::optional<Message> align(Document& d, Notes& notes);
std::optional<Message> segment(Document& d, Notes& notes);
std::optional<Message> match(Document& d, Notes& notes);
std::optional<Message> reconstruct(Document& d, Notes& notes);
std::optional<Message> texture(Document& d, Notes& notes);
std::optional<Message> validate(Document& d, const ValidateOptions& opt, Notes& notes);
// Builds the polished model of a style other than crisp (d.styled), or reuses
// d.cached_styled when it was built from exactly these inputs. Run by validate.
std::optional<Message> style(Document& d, Notes& notes);
// The model that is validated, rendered and exported: the polished one when it
// exists, else the crisp textured mesh (requires d.texture).
const TexturedMesh& model_mesh(const Document& d);

// Builds the editable geometry (reconstruction + user parts + depth
// constraints) and replays every edit; sets base_parts, edited_parts, solid.
std::optional<Message> build_parts(Document& d, Notes& notes);
// Applies d.edits.back(): incrementally when the cached geometry is exactly one
// edit behind, otherwise by a full rebuild. Leaves the document at Reconstructed.
std::optional<Message> apply_last_edit(Document& d, Notes& notes);
// Every conflict of the document (drawings, correspondence, recesses, texture,
// single view, user edits, mask conflicts). `metrics` adds silhouette mismatches.
std::vector<Conflict> document_conflicts(const Document& d, const QualityMetrics* metrics);

// Runs the missing stages up to `target` (validation only if target == Validated).
std::optional<Message> ensure(Document& d, Stage target, const JobContext& job, Notes& notes,
                              const ValidateOptions& vopt = {});

std::map<ViewRole, Mask2D> masks_of(const Document& d);

}  // namespace pipeline
}  // namespace pixel3d
