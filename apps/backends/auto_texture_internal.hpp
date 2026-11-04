#pragma once
// Shared by the bake and the quality checks: what the registered camera sees.
#include <algorithm>
#include <cmath>
#include <functional>
#include "auto_texture.hpp"

namespace p3dasset::autotex::detail {
inline double smoothstep(double a, double b, double x) {
  const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}
std::vector<vec3d> vertex_normals(const pixel3d::Mesh& m);  // area weighted, unit
pixel3d::Mesh render_geometry(const pixel3d::TexturedMesh& m);
double diagonal(const pixel3d::Mesh& m);

// Depth buffer of the mesh from the registered camera, at source resolution, plus the distance (px)
// to the nearest silhouette or depth discontinuity — samples there are unreliable.
struct View {
  int w = 0, h = 0;
  std::vector<float> depth, edge, silhouette;  // silhouette: distance (px) to the alpha outline only
  double pixel_world = 1, diag = 1;
  static View build(const pixel3d::Mesh& m, const Source& s, const Pose& p);
  struct Obs {
    std::array<float, 4> rgb{};  // linear rgb, [3] = source coverage
    float conf = 0;              // 0 unseen … 1 squarely observed
    float facing = 0;            // cos between face normal and the direction to the camera
    uint8_t stage = 0;           // 0 faces away, 1 outside the picture, 2 occluded, 3 seen
    float w_facing = 0, w_alpha = 0, w_edge = 0;
  };
  Obs observe(const Source& s, const Pose& p, vec3d pos, vec3d smooth_n, vec3d face_n, double c0, double c1) const;
};

// All registered pictures (front first) observing a surface point: per-picture confidence (facing, alpha,
// outline distance, optional grazing fade), blended by confidence, surface resolution, sharpness and distance
// from the picture's outline; extra pictures are harmonised to the front's colours. One picture = the front's
// own observation, unchanged.
struct Pictures {
  struct P {
    const Source* src = nullptr;
    Pose pose;
    View view;
    std::array<double, 3> gain{1, 1, 1}, offset{0, 0, 0};
    double weight = 1;
    std::string label;
    double edge_px = 0;  // extra pictures: no texels closer than this to a depth edge / outline (0 = front rules)
  };
  std::vector<P> p;
  static Pictures make(const pixel3d::Mesh& m, const Source& front, const Registration& reg, const std::vector<ExtraView>* extra);
  struct Combined {
    View::Obs o;                 // rgb = blended (harmonised), conf = best picture's confidence
    int dominant = -1;           // picture with the largest blend weight (-1: none sees the point)
    std::array<float, 4> sharp{};  // blended sharp-kernel colour (when requested)
    float best_other = 0;        // best confidence among the extra pictures
  };
  // graze: {cos0, cos1} fade, or nullptr. sharp: also blend Source::sample_sharp colours.
  Combined observe(vec3d pos, vec3d smooth_n, vec3d face_n, double c0, double c1, const double* graze, bool sharp) const;
  std::array<float, 4> harmonise(size_t k, const std::array<float, 4>& linear_rgb) const;
};

void chamfer(std::vector<float>& f, int w, int h);
// Unlit id render (face per pixel, depth, Oklab base colour, surface point), framed on a bounding sphere: the
// hidden-side gate and the regional fix (milestone 4, W4) map pixels of their own renders to faces with it.
struct Frame {
  int n = 0;
  std::vector<int32_t> face;  // -1 = background
  std::vector<float> depth;
  std::vector<std::array<float, 3>> lab;
  std::vector<vec3d> pos;  // surface point per object pixel
  double pixel_world = 0;
};
Frame render_ids(const pixel3d::Mesh& geo, const pixel3d::TexturedMesh& m, double yaw, double elevation, vec3d centre, double radius, int n);

// An extra picture's confidence at a point, including its keep-away from depth edges (as the bake uses it).
inline float extra_conf(const View& v, const ExtraView& e, vec3d p, vec3d n, vec3d fn) {
  auto o = v.observe(*e.source, e.reg.pose, p, n, fn, 0.12, 0.5);
  if (o.conf <= 0 || e.edge_px <= 0) return o.conf;
  const vec2d q = e.reg.pose.to_image(p);
  const double edge = v.edge[size_t(std::clamp(int(q.y), 0, v.h - 1)) * v.w + size_t(std::clamp(int(q.x), 0, v.w - 1))];
  return o.conf * float(smoothstep(0.5 * e.edge_px, e.edge_px, edge));
}  // two-pass distance transform in place (0 = feature)
// Calls f(x, y, b0, b1, b2) for texel centres of [0,size)² inside the UV triangle, rows [y0, y1) only.
void raster_uv(vec2d a, vec2d b, vec2d c, int w, int h, int y0, int y1, const std::function<void(int, int, double, double, double)>& f);
// Texel centres just outside the triangle (within `reach` texels), with barycentrics of the nearest point
// on the triangle: sliver faces smaller than a texel still own their texels (no foreign gutter colour).
void raster_uv_border(vec2d a, vec2d b, vec2d c, int w, int h, int y0, int y1, double reach, const std::function<void(int, int, double, double, double)>& f);
// Runs body(row_begin, row_end) on worker threads over [0, rows).
void parallel_rows(int rows, const std::function<void(int, int)>& body);
struct Symmetry {
  bool accepted = false;
  vec3d normal;
  double offset = 0, error = 1, quality = 0;
  std::vector<int32_t> partner;  // mirror vertex per vertex (-1: none close enough)
  nlohmann::json evidence;
};
Symmetry detect_symmetry(const pixel3d::Mesh& m, double diag, const Deadline& deadline);
Atlas build_planar_atlas(const pixel3d::Mesh& m, int size, double cone_deg, const Deadline& deadline,
                         const std::vector<float>* face_density = nullptr);
std::array<float, 4> sample_texture(const pixel3d::ImageRGBA& t, vec2d uv);  // bilinear, linear rgb + alpha
// The picture's colours in Oklab: its exact colours when it has a small palette (pixel art), else
// `fallback_k` k-means centres. Weighted by pixel count; transparent pixels ignored.
std::vector<std::array<float, 3>> picture_palette(const Source& s, size_t max_exact, int fallback_k, nlohmann::json& evidence);
size_t nearest_colour(const std::vector<std::array<float, 3>>& palette, const std::array<float, 3>& lab);
}  // namespace p3dasset::autotex::detail
namespace p3dasset::autotex {
// Hidden-side views (back, sides, back three-quarters) of any candidate: metrics only (auto_texture_gate.cpp).
nlohmann::json hidden_side_checks(const pixel3d::TexturedMesh& m, const Source& src, const Registration& reg,
                                  const std::vector<ExtraView>* extra_views = nullptr);
}  // namespace p3dasset::autotex
namespace p3dasset::autotex::detail {
}  // namespace p3dasset::autotex::detail
