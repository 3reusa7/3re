#pragma once
// 3re Auto Texture: automatic, object-agnostic texturing of a generated mesh from its one source
// picture (docs/superpowers/specs/2026-10-20-auto-texture-design.md). Geometry is never changed:
// the logical triangles are kept as separate accessors next to the UV-split render mesh.
// Qt-free: pixel3d core + xatlas only, so the worker and the tests need no GUI.
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "pixel3d/image/image.hpp"
#include "pixel3d/mesh/mesh.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace p3dasset::autotex {
using pixel3d::vec2d;
using pixel3d::vec3d;

// Stops a stage that would overrun the worker's time budget; the caller keeps the best result so far.
struct Deadline {
  std::chrono::steady_clock::time_point end;
  static Deadline in_seconds(double s);
  double remaining() const;
  void check(const char* stage) const;  // throws BudgetExceeded
};
struct BudgetExceeded : std::runtime_error { using std::runtime_error::runtime_error; };

// Source picture in linear light with straight alpha; `crisp` = pixel art (nearest sampling).
struct Source {
  int width = 0, height = 0;
  std::vector<std::array<float, 4>> px;
  bool crisp = false;
  static Source from(const pixel3d::ImageRGBA& image, bool crisp);
  float alpha(int x, int y) const { return px[size_t(y) * width + x][3]; }
  // Bilinear, alpha-weighted (never pulls background colour in). rgb + coverage.
  std::array<float, 4> sample(double x, double y) const;
  // Sharper (clamped Catmull-Rom) for picture-owned texels; alpha as `sample`. Pixel art: nearest.
  std::array<float, 4> sample_sharp(double x, double y) const;
};

// Orthographic camera looking at the mesh from (yaw, elevation), mapped onto source pixels.
// yaw 0 = camera on +Z (the TRELLIS front), 90 = on +X; elevation > 0 looks down.
struct Pose {
  double yaw = 0, elevation = 0;
  double scale = 1;    // source pixels per world unit
  vec2d offset;        // source pixel of the world origin
  vec3d right, up, forward;
  static Pose make(double yaw_deg, double elevation_deg);
  vec2d to_image(vec3d p) const { return {offset.x + scale * pixel3d::dot(p, right), offset.y - scale * pixel3d::dot(p, up)}; }
  double depth(vec3d p) const { return pixel3d::dot(p, forward); }
};

struct Registration {
  Pose pose;
  double iou = 0, front_iou = 0;
  bool silhouette_usable = true;  // false: no real transparency → front prior, low confidence
  bool ambiguous = false;         // another clearly different view fits about as well
  nlohmann::json evidence;
};
// Where to look first. Default (no prior): the TRELLIS front (yaw 0, elevation 0), kept unless another view is
// clearly better. An extra picture (multi-angle import) is searched around its labelled angle and always keeps
// that local fit; the best fit anywhere is reported (evidence "global_best") so a wrong label can be caught.
struct RegisterPrior {
  double yaw = 0, elevation = 0;
  double max_drift = 12;   // degrees the refinement may move away from the prior
  bool keep_prior = false; // true: never switch to a better fit elsewhere (it is reported instead)
};
Registration register_source(const pixel3d::Mesh& mesh, const Source& source, const Deadline& deadline,
                             const RegisterPrior* prior = nullptr);

// UV atlas over the logical triangles (same face order). attempt 0 = default charts; later attempts
// use cheaper/safer chart options. Throws when xatlas cannot produce a single complete sheet.
struct Atlas {
  pixel3d::TexturedMesh mesh;           // render mesh, face f == logical face f
  std::vector<uint32_t> logical_vertex; // render vertex -> logical vertex
  int size = 0;
  nlohmann::json metrics;
};
// face_density (optional, per logical face, >= 1): relative texel density; planar charts take the area-weighted
// mean of their faces (xatlas ignores it).
Atlas build_atlas(const pixel3d::Mesh& mesh, int size, int attempt, const Deadline& deadline,
                  const std::vector<float>* face_density = nullptr);
// Relative atlas density per face from the picture (2.3): surface the picture shows in detail (faces, hands,
// emblems: high local contrast) gets up to `max_ratio` times the linear texel density of hidden surface.
std::vector<float> picture_detail_density(const pixel3d::Mesh& mesh, const Source& source, const Registration& reg,
                                          double max_ratio, nlohmann::json& evidence);

// TRELLIS's own texture for the whole object (docs/superpowers/specs/2026-10-20-hybrid-texture-design.md):
// the sparse PBR voxel volume the engine dumps (TRELLIS_DUMP_POST), read-only. Base colour only.
struct NativeVolume {
  int res = 0;
  std::unordered_map<uint64_t, uint32_t> index;  // packed voxel coordinate -> row
  std::vector<std::array<float, 3>> linear;      // base colour per voxel, linear light
  std::vector<float> alpha;                      // opacity per voxel (empty: opaque)
  bool translucent = false;                      // median opacity < 0.8: glass-like object
  nlohmann::json evidence;
  // `max_bytes` bounds the file; throws on any inconsistency (truncated, wrong sizes, no PBR rows).
  static NativeVolume load_post(const std::string& utf8_path, uint64_t max_bytes = 512ull << 20);
  static NativeVolume from_voxels(int res, const std::vector<std::array<int, 3>>& coords, const std::vector<std::array<float, 3>>& srgb);
  // Low-pass over active voxels (Gaussian, sigma in voxels, radius 2): the decoded volume alternates at
  // voxel scale, which a surface cutting diagonally through the shell turns into a checker pattern.
  void smooth(double sigma_voxels);
  // GLB-space point (the engine writes TRELLIS (x, y, z) as (x, z, -y)). Engine sampler rules: trilinear
  // over active voxels, else the mean of the nearest occupied shell up to 3 voxels away. false = no voxel.
  bool sample(vec3d glb_point, std::array<float, 3>& linear_rgb, bool* direct = nullptr) const;
  // Opacity at a GLB-space point (trilinear over active voxels, else nearest within 2 voxels); 1 when unknown.
  float sample_alpha(vec3d glb_point) const;
};

// Native -> picture colour map in Oklab, fitted where the picture sees the surface squarely and applied to
// the whole native layer (hidden side included). Several fits are tried; the one with the best held-out
// score (ΔE plus contrast/chroma distribution error) wins.
struct ColourMap {
  std::string method = "identity";
  std::array<double, 3> gain{1, 1, 1}, offset{0, 0, 0};  // affine part per Oklab channel (a, b; L when no quantiles)
  std::vector<float> l_from, l_to;                       // monotone L quantile map (empty: affine L)
  int lut_n = 0;                                         // residual LUT over native Oklab (lut_n^3 nodes)
  std::array<float, 3> lut_lo{}, lut_hi{};
  std::vector<std::array<float, 3>> lut;
  std::array<float, 3> apply(const std::array<float, 3>& native_lab) const;
};
ColourMap fit_colour_map(const std::vector<std::array<float, 3>>& native_lab, const std::vector<std::array<float, 3>>& picture_lab,
                         const std::vector<double>& weight, nlohmann::json& evidence);

// Generated views of the whole object (MV-Adapter image + geometry -> six orthographic views) from the
// picture's camera: the control images it needs, and the generated views as observations for the bake.
struct MultiViewSet {
  struct Camera {
    vec3d right, up, forward;  // GLB frame
    double scale = 1, azimuth = 0, elevation = 0, yaw = 0;
    int n = 512;
    vec2d to_image(vec3d p) const { return {(dot(p, right) * scale + 0.55) / 1.1 * n, (0.55 - dot(p, up) * scale) / 1.1 * n}; }
    double depth(vec3d p) const { return dot(p, forward) * scale; }
  };
  struct View {
    Camera cam;
    int index = 0;
    double pixel_world = 1;
    std::vector<float> depth, edge;
    std::vector<std::array<float, 3>> rgb;  // linear
  };
  std::vector<View> views;
  double diag = 1;
  nlohmann::json evidence;
  static std::vector<Camera> rig(const pixel3d::Mesh& m, const Registration& reg, int n);
  // pos_k.png / nrm_k.png (k = 0..5) and mv.json, in the adapter's own conventions.
  static nlohmann::json export_controls(const pixel3d::Mesh& m, const Registration& reg, const std::string& utf8_dir, int n = 512);
  // view_k.png generated for the same rig (missing views are skipped).
  static MultiViewSet load(const pixel3d::Mesh& m, const Registration& reg, const std::string& utf8_dir);
  // Blend of the views that see p (z-tested, facing and outline-distance confidence). false = no view sees it.
  bool sample(vec3d p, vec3d smooth_n, vec3d face_n, std::array<float, 3>& linear_rgb, float& weight, float& best_conf) const;
};

// Pictures of the same object from other angles (multi-angle import, 2.3; spec 2026-10-21-multi-angle-design).
// Geometry still comes from the front picture; each extra picture is registered to the mesh around its
// labelled angle, checked (outline fit, label, colour agreement) and harmonised to the front's colours.
struct ExtraInput {
  std::string name;        // file name, for the report
  std::string label;       // front_left, left, back_left, back, back_right, right, front_right, auto
  double yaw = 0;          // camera yaw relative to the front picture's camera (degrees, + = towards +X)
  bool yaw_given = false;  // the user set the angle explicitly (else from the label)
  const Source* source = nullptr;
};
struct ExtraView {
  std::string name, label;
  const Source* source = nullptr;
  Registration reg;
  std::array<double, 3> gain{1, 1, 1}, offset{0, 0, 0};  // Oklab correction (L gain/offset, a/b offset) to the front
  double weight = 1;                                      // relative image sharpness
  // Inner misalignment (px): the geometry comes from the front picture, so where parts overlap in this view
  // (an arm over the body) the picture's edges and the mesh's depth edges disagree by about this much; the
  // picture stays this far away from the mesh's depth edges and outline.
  double edge_px = 8;
  nlohmann::json evidence;
};
// Label -> camera yaw relative to the front (front_left 45, left 90, back_left 135, back 180, back_right 225,
// right 270, front_right 315); false for an unknown label. "auto" searches every angle.
bool label_yaw(const std::string& label, double& yaw);
// Registers, checks and harmonises the extra pictures; rejected ones are listed with the reason in `report`.
std::vector<ExtraView> register_extra_views(const pixel3d::Mesh& mesh, const Source& front, const Registration& front_reg,
                                            const std::vector<ExtraInput>& inputs, const Deadline& deadline, nlohmann::json& report);

struct BakeOptions {
  double observe_cos = 0.12, observe_full_cos = 0.5;  // facing ramp for observed confidence
  // Texels only, with a TRELLIS layer: the picture fades out between these cosines (stretch 1/cos 2.5x .. 1.33x).
  double grazing_cos0 = 0.40, grazing_cos1 = 0.75;
  const NativeVolume* native = nullptr;               // hybrid: TRELLIS texture under the picture
  const MultiViewSet* multiview = nullptr;            // generated views: over the native texture where they see the surface
  double multiview_strength = 1.0;
  bool material_lock = true;  // hidden TRELLIS colours stay within picture materials connected to where the picture shows them
  bool material_match = true;  // material lock: each material's hidden part takes its seen part's mean colour (TRELLIS detail kept)
  double streak_radius = 0.008;  // material lock: hidden stripes of a material narrower than 2x this (x diagonal) are islands
  const std::vector<ExtraView>* extra_views = nullptr;  // other registered pictures: blended per texel with the front
};
struct Bake {
  pixel3d::TexturedMesh mesh;          // atlas mesh + base colour
  pixel3d::ImageRGBA provenance;       // green observed, yellow blended, orange reconstructed, red fallback
  nlohmann::json evidence;
};
Bake bake(const pixel3d::Mesh& logical, const Atlas& atlas, const Source& source, const Registration& reg,
          const BakeOptions& options, const Deadline& deadline);

// Quality checks for ANY candidate texture on this geometry (auto, embedded/Generated PBR, legacy).
// Returns metrics plus "verdict" (PASS / REVIEW / FAIL), "issues" and a selection "score".
// With extra pictures, surface they show counts as seen (not inferred) in the hidden-side checks.
nlohmann::json evaluate(const pixel3d::Mesh& logical, const pixel3d::TexturedMesh& candidate, const Source& source,
                        const Registration& reg, const std::vector<ExtraView>* extra_views = nullptr);

// GLB with the render mesh (POSITION/NORMAL/TEXCOORD_0), linear-filtered base colour (the gutter is fully
// filled, so engines may add mipmaps safely), and the logical
// topology as extra accessors (asset.extras.pixel3d_topology) so geometry identity is checkable.
std::vector<uint8_t> write_glb(const pixel3d::Mesh& logical, const pixel3d::TexturedMesh& mesh, const nlohmann::json& provenance);

// Lit validation render (supersampled), yaw/elevation as in Pose, framed on the bounding sphere.
pixel3d::ImageRGBA render_view(const pixel3d::TexturedMesh& mesh, double yaw, double elevation, int size);

// Oklab helpers shared by the stages (linear rgb in, Lab out).
std::array<float, 3> oklab(const std::array<float, 4>& linear_rgb);
std::array<float, 4> linear_from_oklab(const std::array<float, 3>& lab);
float linear_from_srgb8(uint8_t v);
uint8_t srgb8_from_linear(float v);
}  // namespace p3dasset::autotex
