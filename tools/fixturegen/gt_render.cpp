#include "fixturegen/gt_render.hpp"

#include <cmath>
#include <limits>

namespace p3dfix {

using namespace pixel3d;

namespace {

struct Hit {
  bool hit = false;
  double t = std::numeric_limits<double>::infinity();
  int part = -1;
  vec3d normal;
  vec3d point;
};

// Ray / convex polyhedron (Kay–Kajiya over half-spaces).
Hit cast(const GroundTruth& gt, vec3d o, vec3d dir) {
  Hit best;
  for (size_t i = 0; i < gt.parts.size(); ++i) {
    double t_in = -std::numeric_limits<double>::infinity(), t_out = std::numeric_limits<double>::infinity();
    vec3d n_in;
    bool miss = false;
    for (const Plane& pl : gt.parts[i].planes) {
      const double denom = dot(pl.n, dir), dist = pl.d - dot(pl.n, o);
      if (std::abs(denom) < 1e-12) {
        if (dist < 0) miss = true;
        continue;
      }
      const double t = dist / denom;
      if (denom < 0) {
        if (t > t_in) t_in = t, n_in = pl.n;
      } else {
        t_out = std::min(t_out, t);
      }
    }
    if (miss || t_in > t_out || t_out <= 0) continue;
    if (t_in < best.t) best = Hit{true, t_in, static_cast<int>(i), normalize(n_in), o + dir * t_in};
  }
  return best;
}

}  // namespace

ImageRGBA render_gt_view(const GroundTruth& gt, ViewRole role) {
  const ImageSize sz = view_image_size(role, gt.dims);
  const vec3d dir = view_dir(role);
  ImageRGBA img(sz.w, sz.h);
  std::vector<int> part(static_cast<size_t>(sz.w) * sz.h, -1);
  std::vector<double> depth(part.size(), 0);
  auto idx = [&](int u, int v) { return static_cast<size_t>(v) * sz.w + u; };

  for (int v = 0; v < sz.h; ++v)
    for (int u = 0; u < sz.w; ++u) {
      const vec3d o = world_from_view(role, gt.dims, u + 0.5, v + 0.5, -1.0);
      const Hit h = cast(gt, o, dir);
      if (!h.hit) continue;
      const auto& ramp = gt.ramps[gt.parts[h.part].material];
      bool shadow = false;
      for (const AABB& s : gt.shadow_volumes) shadow = shadow || s.contains(h.point);
      Rgba8 c = ramp[1];
      if (shadow) c = ramp[0];
      else if (h.normal.y > 0.7) c = ramp[3];
      else if (dot(h.normal, -dir) > 0.95) c = ramp[2];
      img.at(u, v) = c;
      part[idx(u, v)] = h.part;
      depth[idx(u, v)] = h.t;
    }

  // Edge pixels: next to transparency, another part or a depth jump.
  std::vector<uint8_t> edge(part.size(), 0);
  const int d4x[] = {1, -1, 0, 0}, d4y[] = {0, 0, 1, -1};
  for (int v = 0; v < sz.h; ++v)
    for (int u = 0; u < sz.w; ++u) {
      if (part[idx(u, v)] < 0) continue;
      for (int k = 0; k < 4; ++k) {
        const int nu = u + d4x[k], nv = v + d4y[k];
        if (nu < 0 || nv < 0 || nu >= sz.w || nv >= sz.h || part[idx(nu, nv)] < 0 ||
            part[idx(nu, nv)] != part[idx(u, v)] || std::abs(depth[idx(nu, nv)] - depth[idx(u, v)]) > 1.5)
          edge[idx(u, v)] = 1;
      }
    }
  // Outline an edge pixel only if interior of the same part lies behind it, so
  // 1–2 px features keep their colour instead of becoming solid outline.
  for (int v = 0; v < sz.h; ++v)
    for (int u = 0; u < sz.w; ++u) {
      if (!edge[idx(u, v)]) continue;
      bool interior = false;
      for (int dv = -1; dv <= 1; ++dv)
        for (int du = -1; du <= 1; ++du) {
          const int nu = u + du, nv = v + dv;
          if (nu < 0 || nv < 0 || nu >= sz.w || nv >= sz.h) continue;
          if (part[idx(nu, nv)] == part[idx(u, v)] && !edge[idx(nu, nv)]) interior = true;
        }
      if (interior) img.at(u, v) = gt.outline;
    }
  return img;
}

}  // namespace p3dfix
