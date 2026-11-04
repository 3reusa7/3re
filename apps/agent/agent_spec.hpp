#pragma once
// AssetSpec: what the user wants delivered (docs/agent/PLAN.md, component 1).
// Strictly validated: every problem is reported by name, nothing is guessed.
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace p3dagent {

namespace fs = std::filesystem;

// The primary input. A saved run wins over a GLB, a GLB over a picture; a picture given
// together with a saved run or a GLB is the re-texture / regeneration source.
enum class InputKind { Picture, Glb, ReuseRun };
enum class AssetType { Prop, Character };
enum class ScaleAxis { Height, Longest };

struct Acceptance {
  std::vector<std::string> geometry_allowed{"PASS", "REVIEW"};
  std::vector<std::string> texture_allowed{"PASS"};
  int khronos_max_errors = 0;
  bool require_godot_import = true;
  double max_deviation_pct = 1.0;  // decimation: p99 distance, % of the bounding-box diagonal
  double max_scale_error_pct = 1.0;
};

// Calibrated on the benchmark's M1 meshes before the M2 benchmark run (docs/agent/FIDELITY.md).
struct FidelityThresholds {
  double max_distance_p95_pct = 0.25, max_distance_mean_pct = 0.10, max_distance_max_pct = 1.5;
  double min_silhouette_iou = 0.97;
  double max_normal_mean_deg = 12;
  double max_colour_mean_de = 0.05;  // judged on the 8x8 block means
};

// Game targets: self-intersections are measured and reported, never blocking (engines draw touching and crossing
// layers); "block" keeps the milestone-1 rule (printing, booleans: no new crossing accepted).
enum class Intersections { Report, Block };
// Delivered front: turn the picture's registered front onto +Z (glTF front), or keep the generator's axes.
enum class Front { Picture, Keep };

struct TextureOptions {
  int candidates = 4;   // B2: TRELLIS texture candidates (the run's own + texture-only replays with seeds 1..N-1)
  int flow_steps = 12;  // texture flow sampler steps of the replays (12 or 24)
};

struct Limits {
  double total_seconds = 3600;
  double step_seconds = 1800;
  // Per-stage job memory limit in MiB (0 = none). Keys: generate, cleanup, decimate, texture, ...
  std::map<std::string, uint64_t> step_ram_mib;
  int max_regenerations = 2;
  uint64_t vram_mib = 0;  // GPU memory the agent may use for its own GPU steps (0 = no limit besides the card)
};

// Milestone 4: more pictures of the same object (multi-angle texture from the start). The label is the side the
// person or the chat proposed; the texture worker's registration verifies it on the mesh (or rejects the picture).
struct ViewPicture {
  fs::path picture;
  std::string label;  // front_left, left, back_left, back, back_right, right, front_right, auto
};

struct AssetSpec {
  std::string name;
  InputKind kind = InputKind::Picture;
  fs::path picture, glb, reuse_run, native_texture, texture_session;
  std::vector<ViewPicture> views;  // at most 5 (the worker's limit)
  // Milestone 4: a style the person asked for ("low-poly cel-shaded"). Recorded and reported, never applied:
  // style transfer is not implemented.
  std::string requested_style;
  uint32_t seed = 42;
  std::string engine = "godot";
  AssetType type = AssetType::Prop;
  int triangle_budget = 0;
  int texture_size = 2048;
  ScaleAxis scale_axis = ScaleAxis::Height;
  double scale_metres = 1.0;
  Acceptance acceptance;
  Limits limits;
  Intersections intersections = Intersections::Report;
  FidelityThresholds fidelity;
  TextureOptions texture;
  Front front = Front::Picture;
  std::string preset;  // A5 budget preset that filled triangle_budget / texture_size (docs/agent/BUDGETS.md), "" = none
  fs::path output_dir;
};

struct SpecResult {
  std::optional<AssetSpec> spec;
  std::vector<std::string> errors;
};

// Relative paths are resolved against base_dir (the spec file's folder).
SpecResult parse_spec(const nlohmann::json& j, const fs::path& base_dir);
SpecResult load_spec(const fs::path& file);
nlohmann::json to_json(const AssetSpec& s);
const char* to_string(InputKind k);
const char* to_string(AssetType t);
const char* to_string(ScaleAxis a);

}  // namespace p3dagent
