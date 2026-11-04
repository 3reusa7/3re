#pragma once
// What the viewport shows and how: scene data taken from a SessionView, the
// orbit camera, what is highlighted, and picking. Shared by the OpenGL
// viewport and the software fallback.
#include <QMatrix4x4>
#include <QPointF>
#include <QString>
#include <QVector3D>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "pixel3d/engine/job_runner.hpp"

namespace p3dgui {

// Axis-aligned box in world units (1 unit = 1 source pixel).
struct Region3 {
  QVector3D lo, hi;
  QVector3D center() const { return (lo + hi) * 0.5f; }
  bool empty() const { return !(hi.x() > lo.x() || hi.y() > lo.y() || hi.z() > lo.z()); }
  bool operator==(const Region3& o) const { return lo == o.lo && hi == o.hi; }
};

// Per-triangle facts shown by the overlays.
enum TriFlag : uint8_t {
  kTriUndecided = 1,         // part whose existence the drawings leave open (amber)
  kTriEstimatedPart = 2,     // part placed by a prior (grid, continuation, fill) (cyan)
  kTriEstimatedSurface = 4,  // surface textured from art that does not show it (cyan)
};

// A pick ray in world space.
struct Ray {
  pixel3d::vec3d origin{0, 0, 0}, dir{0, 0, -1};
};

struct Scene {
  std::shared_ptr<const pixel3d::TexturedMesh> mesh;  // textured mesh, or an untextured solid
  bool textured = false;
  // Painted/photographic texture (Image to 3D): linear filtering with mipmaps. Pixel art keeps nearest.
  bool smooth_texture = false;
  bool styled = false;  // the polished model of a style (soft, lowpoly, clay) is shown
  pixel3d::Dims dims;
  std::map<pixel3d::ViewRole, std::shared_ptr<const pixel3d::ImageRGBA>> sources;  // aligned drawings
  uint64_t revision = 0;
  // One entry per triangle of `mesh` (triangle t = triangle t of the solid).
  std::vector<int> tri_part;
  std::vector<int> tri_region;  // planar region = "face"
  std::vector<uint8_t> tri_flags;
  // M4: the model's solid (face planes, push/pull), and per drawing the pixels
  // where model and drawing disagree (red = only in the model, blue = only drawn).
  std::shared_ptr<const pixel3d::Mesh> solid;
  std::map<pixel3d::ViewRole, std::shared_ptr<const pixel3d::ImageRGBA>> mismatch;
  bool single_view = false;  // one drawing: every depth is estimated
  bool lowpoly = false;      // the LOW-POLY STUDIO character is shown (posed: its test pose)
  // Polygon edges (xyz pairs, mesh coordinates): with the wireframe on, a quad mesh shows these edges
  // instead of its triangles' (Smart Retopology). Empty: the triangles.
  std::shared_ptr<const std::vector<float>> wire_edges;
  bool empty() const { return !mesh || mesh->indices.empty(); }
  size_t triangles() const { return mesh ? mesh->indices.size() / 3 : 0; }
};

// Builds the scene for a document (texture if projected, else the bare solid).
// LOW-POLY STUDIO projects show the built character (`lowpoly_pose`: the rig's test pose).
Scene scene_from(const pixel3d::Document& d, bool lowpoly_pose = false);

// Untextured triangle soup with face normals (edit previews).
std::shared_ptr<const pixel3d::TexturedMesh> flat_mesh(const pixel3d::Mesh& m);

// What the viewport emphasises; owned by the window, drawn by the viewport.
struct Highlight {
  std::set<int> parts;        // selected parts (accent)
  int region = -1;            // selected face (planar region id), accent
  int hover_part = -1;        // part under the cursor
  int hover_region = -1;      // face under the cursor (face mode)
  std::vector<Region3> issue_regions;  // amber outlines (open issue)
  std::vector<Region3> focus_regions;  // accent outlines (hovered candidate)
  // An edit being previewed (shown instead of the model), with a one-line summary.
  std::shared_ptr<const pixel3d::TexturedMesh> preview;
  bool operator==(const Highlight&) const = default;
};

struct ViewState {
  float yaw = 35, pitch = 22;  // degrees; yaw 0 = from the front (+Z), 90 = from the right (+X)
  float zoom = 1;
  QVector3D pan;  // world offset of the orbit target
  bool ortho = false;
  bool wireframe = false;
  bool lit = true;
  bool grid = true;
  bool overlay = true;          // blend the source drawing over a matching view preset
  bool show_issues = true;      // amber tint on undecided parts, outlines of open issues
  bool show_estimated = false;  // cyan tint on estimated parts and surfaces
  bool textured = true;         // the projected texture (off: plain grey)
  bool pixel_preview = false;   // about one rendered pixel per source pixel, enlarged
  bool normals = false;         // face normals as short lines
  std::optional<pixel3d::ViewRole> preset;  // the camera is exactly this source view

  void set_preset(pixel3d::ViewRole r);
  void set_iso();
  void orbit(float dyaw, float dpitch);  // leaves any preset
};

struct CameraMatrices {
  QMatrix4x4 view, proj;
  QVector3D eye, target;
};

CameraMatrices camera_matrices(const ViewState& v, pixel3d::Dims d, int width, int height);

// Ray through a widget position (logical pixels) for the OpenGL camera, and back.
Ray gl_ray_at(const ViewState& v, pixel3d::Dims d, int width, int height, QPointF pos);
QPointF gl_project(const ViewState& v, pixel3d::Dims d, int width, int height, pixel3d::vec3d p);
// The drawing pixel (u, v; fractional) a view-preset ray goes through.
pixel3d::vec2d view_pixel_of_ray(pixel3d::ViewRole r, pixel3d::Dims d, const Ray& ray);

// Pan and zoom that frame `r` from the current angle.
void frame_region(const ViewState& v, pixel3d::Dims d, const Region3& r, QVector3D* pan, float* zoom);

// Nearest triangle under a point in normalised device coordinates, or -1.
int pick_triangle(const Scene& s, const CameraMatrices& cam, QPointF ndc);
// Nearest triangle hit by origin + t·dir (t >= 0, both sides), or -1; *t gets t.
int ray_cast(const Scene& s, QVector3D origin, QVector3D dir, float* t = nullptr);

// Outline of a set of triangles: edges on its border or at a crease (> 20°),
// as line segments (x, y, z, x, y, z, ...).
std::vector<float> outline_edges(const Scene& s, const std::vector<bool>& member);

// Union of regions (empty Region3 if none).
Region3 bounds(const std::vector<Region3>& rs);
// World box of a set of triangles.
Region3 triangles_bounds(const Scene& s, const std::vector<bool>& member);

}  // namespace p3dgui
