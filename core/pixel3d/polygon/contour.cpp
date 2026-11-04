#include "pixel3d/polygon/contour.hpp"

#include <clipper2/clipper.h>

namespace pixel3d {
namespace {

constexpr int kPrecision = 8;  // decimal places kept by Clipper2's double API

Clipper2Lib::PathsD to_clipper(const Polygons2D& p) {
  Clipper2Lib::PathsD out;
  out.reserve(p.size());
  for (const Ring& r : p) {
    Clipper2Lib::PathD path;
    path.reserve(r.size());
    for (const vec2d& v : r) path.emplace_back(v.x, v.y);
    out.push_back(std::move(path));
  }
  return out;
}

Polygons2D from_clipper(const Clipper2Lib::PathsD& p) {
  Polygons2D out;
  out.reserve(p.size());
  for (const auto& path : p) {
    Ring r;
    r.reserve(path.size());
    for (const auto& pt : path) r.push_back({pt.x, pt.y});
    if (r.size() >= 3) out.push_back(std::move(r));
  }
  return out;
}

}  // namespace

Polygons2D mask_to_polygons(const Mask2D& m) {
  Clipper2Lib::Paths64 rects;
  for (int v = 0; v < m.height(); ++v) {
    int u = 0;
    while (u < m.width()) {
      if (!m.get(u, v)) {
        ++u;
        continue;
      }
      const int a = u;
      while (u < m.width() && m.get(u, v)) ++u;
      rects.push_back(Clipper2Lib::MakePath({a, v, u, v, u, v + 1, a, v + 1}));
    }
  }
  const Clipper2Lib::Paths64 merged = Clipper2Lib::Union(rects, Clipper2Lib::FillRule::NonZero);
  Polygons2D out;
  for (const auto& path : merged) {
    // Clipper2 may keep seam vertices between merged rectangles: drop collinear ones.
    Ring r;
    const size_t n = path.size();
    for (size_t i = 0; i < n; ++i) {
      const auto& a = path[(i + n - 1) % n];
      const auto& b = path[i];
      const auto& c = path[(i + 1) % n];
      const int64_t cr = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
      if (cr != 0) r.push_back({static_cast<double>(b.x), static_cast<double>(b.y)});
    }
    if (r.size() >= 3) out.push_back(std::move(r));
  }
  return out;
}

Polygons2D clip(const Polygons2D& subject, const Polygons2D& clipper) {
  return from_clipper(
      Clipper2Lib::Intersect(to_clipper(subject), to_clipper(clipper), Clipper2Lib::FillRule::NonZero, kPrecision));
}

Polygons2D unite(const Polygons2D& a, const Polygons2D& b) {
  return from_clipper(Clipper2Lib::Union(to_clipper(a), to_clipper(b), Clipper2Lib::FillRule::NonZero, kPrecision));
}

Polygons2D subtract(const Polygons2D& subject, const Polygons2D& clipper) {
  return from_clipper(
      Clipper2Lib::Difference(to_clipper(subject), to_clipper(clipper), Clipper2Lib::FillRule::NonZero, kPrecision));
}

Polygons2D rect_polys(double u0, double v0, double u1, double v1) {
  return {Ring{{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}}};
}

double signed_area(const Ring& r) {
  double a = 0;
  for (size_t i = 0; i < r.size(); ++i) a += cross(r[i], r[(i + 1) % r.size()]);
  return a / 2;
}

double area(const Polygons2D& p) {
  double a = 0;
  for (const Ring& r : p) a += signed_area(r);
  return a;
}

}  // namespace pixel3d
