#include "pixel3d/style/rounding.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include <clipper2/clipper.h>

#include "pixel3d/polygon/raster.hpp"

namespace pixel3d {

CornerReport& CornerReport::operator+=(const CornerReport& o) {
  corners += o.corners;
  rounded += o.rounded;
  clamped += o.clamped;
  thin += o.thin;
  beveled += o.beveled;
  joined += o.joined;
  restored_pixels += o.restored_pixels;
  max_size = std::max(max_size, o.max_size);
  return *this;
}

namespace {

// NonZero winding number of `c` in `p`.
int winding(const Polygons2D& p, vec2d c) {
  int w = 0;
  for (const Ring& r : p)
    for (size_t i = 0; i < r.size(); ++i) {
      const vec2d a = r[i], b = r[(i + 1) % r.size()];
      if (a.y <= c.y) {
        if (b.y > c.y && cross(b - a, c - a) > 0) ++w;
      } else if (b.y <= c.y && cross(b - a, c - a) < 0) {
        --w;
      }
    }
  return w;
}

// One convex corner V with unit edge directions a (towards the previous vertex)
// and b (towards the next one). In the affine frame V + s·a + u·b the corner
// curve is the superellipse quadrant |t−s|ⁿ + |t−u|ⁿ = tⁿ, tangent to both edges
// at distance t from V; everything between it and V is cut away.
struct Corner {
  vec2d v, a, b;
  double det;  // cross(a, b) ≠ 0
  bool cuts(vec2d c, double t, double n) const {
    const vec2d d = c - v;
    const double s = cross(d, b) / det, u = cross(a, d) / det;
    if (s < 0 || u < 0 || s > t || u > t) return false;
    return std::pow((t - s) / t, n) + std::pow((t - u) / t, n) > 1;
  }
};

// Whether a corner of tangent length t (grown by the safety margin) would
// uncover a pixel centre that `p` covers.
bool loses_centre(const Corner& k, double t, double n, double margin, const Polygons2D& p) {
  const double tm = t + margin;
  const vec2d q[4] = {k.v, k.v + k.a * tm, k.v + k.b * tm, k.v + (k.a + k.b) * tm};
  double x0 = q[0].x, x1 = q[0].x, y0 = q[0].y, y1 = q[0].y;
  for (const vec2d& v : q) x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
  for (double cy = std::floor(y0 - 0.5) + 0.5; cy <= y1; cy += 1)
    for (double cx = std::floor(x0 - 0.5) + 0.5; cx <= x1; cx += 1)
      if (k.cuts({cx, cy}, tm, n) && winding(p, {cx, cy}) != 0) return true;
  return false;
}

// `p` shrunk by `d` (sharp miters, so thick corners keep their place).
Polygons2D inset(const Polygons2D& p, double d) {
  Clipper2Lib::PathsD in;
  for (const Ring& r : p) {
    Clipper2Lib::PathD path;
    for (const vec2d& v : r) path.push_back({v.x, v.y});
    in.push_back(std::move(path));
  }
  Polygons2D out;
  for (const auto& path : Clipper2Lib::InflatePaths(in, -d, Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon, 50, 8)) {
    Ring r;
    for (const auto& v : path) r.push_back({v.x, v.y});
    out.push_back(std::move(r));
  }
  return out;
}

double distance_to_outline(const Polygons2D& p, vec2d c) {
  double best = 1e300;
  for (const Ring& r : p)
    for (size_t i = 0; i < r.size(); ++i) {
      const vec2d a = r[i], b = r[(i + 1) % r.size()], ab = b - a;
      const double l2 = dot(ab, ab), t = l2 > 0 ? std::clamp(dot(c - a, ab) / l2, 0.0, 1.0) : 0.0;
      best = std::min(best, length(c - (a + ab * t)));
    }
  return best;
}

struct GridBox {
  int ox = 0, oy = 0, w = 0, h = 0;
};

GridBox grid_of(const Polygons2D& p) {
  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
  for (const Ring& r : p)
    for (const vec2d& v : r) x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y);
  if (x0 > x1) return {};
  GridBox g;
  g.ox = static_cast<int>(std::floor(x0)) - 1;
  g.oy = static_cast<int>(std::floor(y0)) - 1;
  g.w = static_cast<int>(std::ceil(x1)) + 1 - g.ox;
  g.h = static_cast<int>(std::ceil(y1)) + 1 - g.oy;
  return g;
}

Mask2D raster_on(const Polygons2D& p, const GridBox& g) {
  Polygons2D q = p;
  for (Ring& r : q)
    for (vec2d& v : r) v = {v.x - g.ox, v.y - g.oy};
  return rasterize_pixel_centres(q, g.w, g.h);
}

}  // namespace

Polygons2D round_corners(const Polygons2D& p, const CornerOptions& opt, CornerReport* report, const Polygons2D* joined) {
  CornerReport rep;
  const double n = std::clamp(opt.exponent, 1.0, 8.0);
  const int segments = n == 1.0 ? 1 : std::max(1, opt.segments);
  // A feature at most thin_px wide disappears when shrunk by half of that: its
  // corners lie much farther from the shrunk outline than a thick corner would.
  const double half = std::max(0.0, opt.thin_px) / 2 + 1e-3;
  const Polygons2D core = inset(p, half);
  Polygons2D rings;
  for (const Ring& r : p) {
    const size_t m = r.size();
    Ring out;
    for (size_t i = 0; i < m; ++i) {
      const vec2d prev = r[(i + m - 1) % m], v = r[i], next = r[(i + 1) % m];
      const double lp = length(prev - v), ln = length(next - v);
      if (lp < 1e-12 || ln < 1e-12) continue;
      const Corner k{v, (prev - v) * (1 / lp), (next - v) * (1 / ln), 0};
      // Convex = a left turn on a positively oriented ring (hole rings turn right at their corners).
      const double turn = cross(v - prev, next - v);
      if (turn <= 1e-9 * lp * ln) {
        out.push_back(v);
        continue;
      }
      ++rep.corners;
      // Just past the corner along each edge's line (nudged inwards along the
      // other edge): inside a touching part means the outline continues there.
      if (joined) {
        const vec2d a = (prev - v) * (1 / lp), b = (next - v) * (1 / ln);
        if (winding(*joined, v - a * 0.05 + b * 0.02) != 0 || winding(*joined, v - b * 0.05 + a * 0.02) != 0) {
          ++rep.joined;
          out.push_back(v);
          continue;
        }
      }
      Corner c = k;
      c.det = cross(c.a, c.b);
      const double angle = std::acos(std::clamp(dot(c.a, c.b), -1.0, 1.0));
      const bool thin = core.empty() || distance_to_outline(core, v) > half / std::sin(angle / 2) + 0.3;
      // Largest tangent length up to `wanted` that keeps every covered pixel centre.
      auto fit = [&](double radius) {
        const double wanted = std::max(0.0, radius) / std::tan(angle / 2);
        const double cap = std::min({wanted, 0.5 * lp, 0.5 * ln});
        double t = cap;
        if (t > 0 && loses_centre(c, t, n, opt.margin, p)) {
          double lo = 0, hi = cap;
          for (int it = 0; it < 30; ++it) {
            const double mid = (lo + hi) / 2;
            (loses_centre(c, mid, n, opt.margin, p) ? hi : lo) = mid;
          }
          t = lo;
        }
        return std::pair{t, wanted};
      };
      double t = 0;
      if (thin) {
        ++rep.thin;
        if (opt.bevel > 0) t = fit(opt.bevel).first;
        if (t < 0.02) {
          out.push_back(v);
          continue;
        }
        ++rep.beveled;
      } else {
        const auto [fitted, wanted] = fit(std::max(opt.radius, opt.bevel));
        t = fitted;
        if (t < wanted - 1e-9) ++rep.clamped;
        if (t < 0.05) {
          out.push_back(v);
          continue;
        }
      }
      if (!thin) ++rep.rounded;
      rep.max_size = std::max(rep.max_size, t);
      // From the previous edge's tangent point to the next edge's.
      for (int j = 0; j <= segments; ++j) {
        const double phi = (1 - static_cast<double>(j) / segments) * std::acos(0.0);
        const double s = t - t * std::pow(std::cos(phi), 2 / n), u = t - t * std::pow(std::sin(phi), 2 / n);
        out.push_back(v + c.a * s + c.b * u);
      }
    }
    if (out.size() >= 3) rings.push_back(std::move(out));
  }
  Polygons2D result = clip(unite(rings, {}), p);

  // Last-resort guarantee: put back any covered pixel centre the outline lost.
  const GridBox g = grid_of(p);
  if (g.w > 0) {
    const Mask2D want = raster_on(p, g), got = raster_on(result, g);
    for (int y = 0; y < g.h; ++y)
      for (int x = 0; x < g.w; ++x)
        if (want.get(x, y) && !got.get(x, y)) {
          result = unite(result, clip(rect_polys(x + g.ox, y + g.oy, x + g.ox + 1, y + g.oy + 1), p));
          ++rep.restored_pixels;
        }
  }
  if (report) *report += rep;
  return result;
}

namespace {

// Rotations taking the Front / Side / Top projection axis to +Z, and back.
const manifold::mat3x4 kTo[3] = {
    manifold::mat3x4({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}),
    manifold::mat3x4({0, 0, -1}, {0, 1, 0}, {1, 0, 0}, {0, 0, 0}),   // (x, y, z) → (z, y, −x)
    manifold::mat3x4({1, 0, 0}, {0, 0, 1}, {0, -1, 0}, {0, 0, 0})};  // (x, y, z) → (x, −z, y)
const manifold::mat3x4 kFrom[3] = {
    manifold::mat3x4({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}),
    manifold::mat3x4({0, 0, 1}, {0, 1, 0}, {-1, 0, 0}, {0, 0, 0}),
    manifold::mat3x4({1, 0, 0}, {0, 0, -1}, {0, 1, 0}, {0, 0, 0})};
const char* const kAxisName[3] = {"front", "side", "top"};

Polygons2D projection(const manifold::Manifold& rotated) {
  Polygons2D p;
  for (const manifold::SimplePolygon& sp : rotated.Project()) {
    Ring r;
    for (const manifold::vec2& v : sp) r.push_back({v.x, v.y});
    p.push_back(std::move(r));
  }
  return unite(p, {});
}

manifold::Polygons to_manifold(const Polygons2D& p) {
  manifold::Polygons out;
  for (const Ring& r : p) {
    manifold::SimplePolygon sp;
    for (const vec2d& v : r) sp.push_back({v.x, v.y});
    out.push_back(std::move(sp));
  }
  return out;
}

}  // namespace

namespace {

// Pixel centres covered by `want` but not by `got` (both projections in one
// rotated frame), with a readable description of the first one.
std::vector<vec2d> lost_centres(const Polygons2D& want, const Polygons2D& got, std::string* where) {
  std::vector<vec2d> lost;
  const GridBox g = grid_of(want);
  if (g.w == 0) return lost;
  const Mask2D a = raster_on(want, g), b = raster_on(got, g);
  for (int y = 0; y < g.h; ++y)
    for (int x = 0; x < g.w; ++x)
      if (a.get(x, y) && !b.get(x, y)) lost.push_back({x + g.ox + 0.5, y + g.oy + 0.5});
  if (where && !lost.empty())
    *where = std::to_string(lost.size()) + " pixel(s), first at (" + std::to_string(lost[0].x - 0.5) + ", " +
             std::to_string(lost[0].y - 0.5) + ")";
  return lost;
}

// Rounds one part without looking at silhouettes; checks only that the result
// is a valid closed solid inside the part (otherwise returns the part).
// A part this one touches; `across` is the projection (0 Front/Z, 1 Side/X,
// 2 Top/Y) along whose axis they are stacked, where the contact is occlusion,
// not a continuing outline (-1: no single stacking axis).
struct Touching {
  manifold::Manifold solid;
  int across = -1;
};

manifold::Manifold round_unchecked(const manifold::Manifold& part, const CornerOptions& opt, PartRoundReport& rep,
                                   const std::vector<Touching>& touching = {}) {
  if (part.IsEmpty()) return part;
  std::vector<manifold::Manifold> keep{part};
  for (int k = 0; k < 3; ++k) {
    const manifold::Manifold q = part.Transform(kTo[k]);
    const Polygons2D own = projection(q);
    Polygons2D joined = own;
    bool any = false;
    for (const Touching& t : touching)
      if (t.across != k) joined = unite(joined, projection(t.solid.Transform(kTo[k]))), any = true;
    CornerReport cr;
    const Polygons2D rounded = round_corners(own, opt, &cr, any ? &joined : nullptr);
    rep.corners += cr;
    if (cr.rounded == 0 && cr.beveled == 0 && cr.restored_pixels == 0) continue;
    const manifold::Box bb = q.BoundingBox();
    const double z0 = bb.min.z - 1, z1 = bb.max.z + 1;
    keep.push_back(manifold::Manifold::Extrude(to_manifold(rounded), z1 - z0).Translate({0, 0, z0}).Transform(kFrom[k]));
  }
  if (keep.size() == 1) return part;
  const manifold::Manifold out = manifold::Manifold::BatchBoolean(keep, manifold::OpType::Intersect);
  auto fail = [&](const char* why) {
    rep.fallback = true;
    rep.reason = why;
    return part;
  };
  if (out.Status() != manifold::Manifold::Error::NoError || out.IsEmpty()) return fail("rounding produced no valid solid");
  if ((out - part).Volume() > 1e-6) return fail("rounding left the part's volume");
  return out;
}

}  // namespace

manifold::Manifold round_part(const manifold::Manifold& part, const CornerOptions& opt, PartRoundReport* report) {
  PartRoundReport rep;
  manifold::Manifold out = round_unchecked(part, opt, rep);
  if (!rep.fallback && !part.IsEmpty())
    for (int k = 0; k < 3; ++k) {
      std::string where;
      if (!lost_centres(projection(part.Transform(kTo[k])), projection(out.Transform(kTo[k])), &where).empty()) {
        rep.fallback = true;
        rep.reason = std::string("rounding would change the ") + kAxisName[k] + " silhouette: " + where;
        out = part;
        break;
      }
    }
  if (report) *report = rep;
  return out;
}

PartGeometry round_parts(const PartGeometry& g, const CornerOptions& opt, std::vector<PartRoundReport>* reports) {
  PartGeometry out;
  std::map<int, PartRoundReport> reps;
  for (const auto& [id, m] : g.parts) {
    PartRoundReport& rep = reps[id];
    rep.part = id;
    // Parts sharing surface with this one (bounding boxes touch and no gap).
    std::vector<Touching> touching;
    const manifold::Box b = m.BoundingBox();
    for (const auto& [oid, o] : g.parts) {
      if (oid == id || o.IsEmpty()) continue;
      const manifold::Box ob = o.BoundingBox();
      constexpr double eps = 1e-6;
      if (ob.min.x > b.max.x + eps || ob.max.x < b.min.x - eps || ob.min.y > b.max.y + eps || ob.max.y < b.min.y - eps ||
          ob.min.z > b.max.z + eps || ob.max.z < b.min.z - eps)
        continue;
      if (m.MinGap(o, 0.01) >= 1e-6) continue;
      // The contact's thinnest side is its normal: Z → Front (0), X → Side (1), Y → Top (2).
      const double ex = std::min(b.max.x, ob.max.x) - std::max(b.min.x, ob.min.x);
      const double ey = std::min(b.max.y, ob.max.y) - std::max(b.min.y, ob.min.y);
      const double ez = std::min(b.max.z, ob.max.z) - std::max(b.min.z, ob.min.z);
      const double thin = std::min({ex, ey, ez});
      const int across = thin > 1e-3 ? -1 : thin == ez ? 0 : thin == ex ? 1 : 2;
      touching.push_back({o, across});
    }
    out.parts[id] = round_unchecked(m, opt, rep, touching);
  }
  // The silhouettes that matter are the whole model's: a part's own projection
  // may lose a centre another part still covers. Any pixel centre the model
  // loses sends the rounded parts that covered it back to their crisp shape.
  const manifold::Manifold crisp = g.all();
  for (int k = 0; k < 3 && !crisp.IsEmpty(); ++k) {
    const Polygons2D want = projection(crisp.Transform(kTo[k]));
    for (size_t guard = 0; guard <= g.parts.size(); ++guard) {
      std::string where;
      const std::vector<vec2d> lost = lost_centres(want, projection(out.all().Transform(kTo[k])), &where);
      if (lost.empty()) break;
      bool reverted = false;
      for (const auto& [id, m] : g.parts) {
        PartRoundReport& rep = reps[id];
        if (rep.fallback || (rep.corners.rounded == 0 && rep.corners.beveled == 0)) continue;
        const Polygons2D own = projection(m.Transform(kTo[k]));
        if (std::none_of(lost.begin(), lost.end(), [&](vec2d c) { return winding(own, c) != 0; })) continue;
        out.parts[id] = m;
        rep.fallback = true;
        rep.reason = std::string("kept sharp: rounding would change the ") + kAxisName[k] + " silhouette (" + where + ")";
        reverted = true;
      }
      if (!reverted) break;  // cannot happen for parts inside the crisp model; the gate still checks
    }
  }
  out.normalize();
  if (reports)
    for (auto& [id, rep] : reps) reports->push_back(std::move(rep));
  return out;
}

}  // namespace pixel3d
