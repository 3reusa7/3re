#pragma once
// Test-only helpers: build consistent orthographic view images from simple
// voxel objects made of integer boxes. Mapping follows spec §4.
#include <array>
#include <map>
#include <vector>

#include <string>

#include "pixel3d/image/image.hpp"
#include "pixel3d/image/mask.hpp"
#include "pixel3d/views/align.hpp"
#include "pixel3d/views/view_role.hpp"

namespace p3dtest {

using pixel3d::Dims;
using pixel3d::ImageRGBA;
using pixel3d::Rgba8;
using pixel3d::ViewRole;

struct Box {
  int x0, y0, z0, x1, y1, z1;  // half-open voxel ranges
  Rgba8 color{150, 100, 60, 255};
};

// Pixel (u, v) of `role` that the voxel cell (x, y, z) projects to.
inline std::array<int, 2> project_cell(ViewRole role, Dims d, int x, int y, int z) {
  switch (role) {
    case ViewRole::Front: return {x, d.H - 1 - y};
    case ViewRole::Back: return {d.W - 1 - x, d.H - 1 - y};
    case ViewRole::Side: return {z, d.H - 1 - y};
    case ViewRole::Right: return {d.D - 1 - z, d.H - 1 - y};
    case ViewRole::Top: return {x, z};
    case ViewRole::Bottom: return {x, d.D - 1 - z};
  }
  return {0, 0};
}

// Distance from the camera-side bbox face to the cell centre (for occlusion).
inline double cell_depth(ViewRole role, Dims d, int x, int y, int z) {
  switch (role) {
    case ViewRole::Front: return d.D - (z + 0.5);
    case ViewRole::Back: return z + 0.5;
    case ViewRole::Side: return x + 0.5;
    case ViewRole::Right: return d.W - (x + 0.5);
    case ViewRole::Top: return d.H - (y + 0.5);
    case ViewRole::Bottom: return y + 0.5;
  }
  return 0;
}

// Renders each view with the colour of the nearest box cell (no shading).
inline ImageRGBA render_view(const std::vector<Box>& boxes, Dims d, ViewRole role) {
  const auto size = pixel3d::view_image_size(role, d);
  ImageRGBA img(size.w, size.h);
  std::vector<double> depth(static_cast<size_t>(size.w) * size.h, 1e30);
  for (const Box& b : boxes)
    for (int z = b.z0; z < b.z1; ++z)
      for (int y = b.y0; y < b.y1; ++y)
        for (int x = b.x0; x < b.x1; ++x) {
          const auto [u, v] = project_cell(role, d, x, y, z);
          const double dd = cell_depth(role, d, x, y, z);
          double& slot = depth[static_cast<size_t>(v) * size.w + u];
          if (dd < slot) {
            slot = dd;
            img.at(u, v) = b.color;
          }
        }
  return img;
}

inline std::map<ViewRole, ImageRGBA> render_views(const std::vector<Box>& boxes, Dims d,
                                                  std::vector<ViewRole> roles = {ViewRole::Front, ViewRole::Side,
                                                                                 ViewRole::Top}) {
  std::map<ViewRole, ImageRGBA> out;
  for (ViewRole r : roles) out.emplace(r, render_view(boxes, d, r));
  return out;
}

// Nearest-neighbour upscale by k with transparent margins (left, top, right, bottom).
inline ImageRGBA upscale_padded(const ImageRGBA& src, int k, int l, int t, int r, int b) {
  ImageRGBA out(src.width() * k + l + r, src.height() * k + t + b);
  for (int y = 0; y < src.height() * k; ++y)
    for (int x = 0; x < src.width() * k; ++x) out.at(x + l, y + t) = src.at(x / k, y / k);
  return out;
}

// Aligned views of a committed fixture (tests/fixtures/<name>/<role>.png).
struct Fixture {
  std::vector<pixel3d::SourceView> views;
  pixel3d::AlignResult align;
  const pixel3d::SourceView& view(ViewRole r) const {
    for (const auto& v : views)
      if (v.role == r) return v;
    throw std::runtime_error("fixture view missing");
  }
  pixel3d::Mask2D mask(ViewRole r) const { return pixel3d::opaque_mask(*view(r).aligned); }
  std::map<ViewRole, pixel3d::Mask2D> y_masks() const {
    std::map<ViewRole, pixel3d::Mask2D> m;
    for (const auto& v : views)
      if (v.role != ViewRole::Top && v.role != ViewRole::Bottom) m.emplace(v.role, pixel3d::opaque_mask(*v.aligned));
    return m;
  }
};

inline Fixture load_fixture(const std::string& name,
                            std::vector<ViewRole> roles = {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
  Fixture f;
  for (ViewRole r : roles) {
    auto img = pixel3d::load_png(std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + std::string(pixel3d::to_string(r)) +
                                 ".png");
    if (!img) throw std::runtime_error("cannot load fixture " + name + ": " + img.error().text);
    pixel3d::SourceView s;
    s.role = r;
    s.original = std::make_shared<const ImageRGBA>(std::move(*img));
    f.views.push_back(std::move(s));
  }
  auto res = pixel3d::auto_align(f.views, pixel3d::AlignOptions{});
  if (!res) throw std::runtime_error("align failed: " + res.error().text);
  f.align = *res;
  return f;
}

}  // namespace p3dtest
