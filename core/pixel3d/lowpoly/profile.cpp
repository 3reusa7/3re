#include "pixel3d/lowpoly/profile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pixel3d::lowpoly {
namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

// 1D squared distance transform (Felzenszwalb & Huttenlocher); f = kFar marks "no site".
constexpr double kFar = 1e20;
void edt_1d(const double* f, double* d, int n, std::vector<int>& v, std::vector<double>& z) {
  v.assign(static_cast<size_t>(n), 0);
  z.assign(static_cast<size_t>(n) + 1, 0);
  int k = 0;
  z[0] = -kInf;
  z[1] = kInf;
  for (int q = 1; q < n; ++q) {
    double s;
    while (true) {
      const int p = v[static_cast<size_t>(k)];
      s = ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * (q - p));
      if (s <= z[static_cast<size_t>(k)] && k > 0) {
        --k;
        continue;
      }
      break;
    }
    ++k;
    v[static_cast<size_t>(k)] = q;
    z[static_cast<size_t>(k)] = s;
    z[static_cast<size_t>(k) + 1] = kInf;
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[static_cast<size_t>(k) + 1] < q) ++k;
    const int p = v[static_cast<size_t>(k)];
    d[q] = double(q - p) * (q - p) + f[p];
  }
}

struct Crossings {
  double tmin = kInf, tmax = -kInf, tpos = kInf, tneg = -kInf;
};

Crossings cross_line(const Polygons2D& poly, vec2d p, vec2d n) {
  Crossings c;
  for (const Ring& r : poly)
    for (size_t i = 0; i < r.size(); ++i) {
      const vec2d a = r[i], b = r[(i + 1) % r.size()], e = b - a;
      const double den = cross(n, e);
      if (std::abs(den) < 1e-12) continue;
      const vec2d ap = a - p;
      const double t = cross(ap, e) / den, u = cross(ap, n) / den;
      if (u < 0 || u >= 1) continue;
      c.tmin = std::min(c.tmin, t);
      c.tmax = std::max(c.tmax, t);
      if (t >= 0) c.tpos = std::min(c.tpos, t);
      if (t <= 0) c.tneg = std::max(c.tneg, t);
    }
  return c;
}

vec2d lerp(vec2d a, vec2d b, double t) { return a + (b - a) * t; }
double lerp(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace

double DistanceField::sample(vec2d p) const {
  const double x = p.x - 0.5, y = p.y - 0.5;
  const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
  const double fx = x - x0, fy = y - y0;
  return (1 - fy) * ((1 - fx) * at(x0, y0) + fx * at(x0 + 1, y0)) + fy * ((1 - fx) * at(x0, y0 + 1) + fx * at(x0 + 1, y0 + 1));
}

float DistanceField::max() const {
  float m = 0;
  for (float v : d) m = std::max(m, v);
  return m;
}

DistanceField distance_transform(const Mask2D& m) {
  // Sites are the outside pixels, including a one-pixel ring around the image.
  const int w = m.width() + 2, h = m.height() + 2;
  std::vector<double> g(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) g[static_cast<size_t>(y) * w + x] = m.get_or_false(x - 1, y - 1) ? kFar : 0;
  std::vector<int> v;
  std::vector<double> z, f(static_cast<size_t>(std::max(w, h))), d(f.size());
  for (int x = 0; x < w; ++x) {
    for (int y = 0; y < h; ++y) f[static_cast<size_t>(y)] = g[static_cast<size_t>(y) * w + x];
    edt_1d(f.data(), d.data(), h, v, z);
    for (int y = 0; y < h; ++y) g[static_cast<size_t>(y) * w + x] = d[static_cast<size_t>(y)];
  }
  for (int y = 0; y < h; ++y) {
    edt_1d(&g[static_cast<size_t>(y) * w], d.data(), w, v, z);
    for (int x = 0; x < w; ++x) g[static_cast<size_t>(y) * w + x] = d[static_cast<size_t>(x)];
  }
  DistanceField df;
  df.w = m.width();
  df.h = m.height();
  df.d.assign(static_cast<size_t>(df.w) * df.h, 0.f);
  for (int y = 0; y < df.h; ++y)
    for (int x = 0; x < df.w; ++x)
      if (m.get(x, y))
        df.d[static_cast<size_t>(y) * df.w + x] =
            static_cast<float>(std::sqrt(g[static_cast<size_t>(y + 1) * w + x + 1]) - 0.5);
  return df;
}

std::vector<Station> measure_widths(const Spine& sp, const Polygons2D& outline, bool outer_extent) {
  std::vector<Station> out(sp.pts.size());
  double s = 0;
  for (size_t i = 0; i < sp.pts.size(); ++i) {
    if (i) s += length(sp.pts[i] - sp.pts[i - 1]);
    Station& st = out[i];
    st.s = s;
    st.n = sp.normals[i];
    st.centre = sp.pts[i];
    const Crossings c = cross_line(outline, sp.pts[i], st.n);
    double lo = 0, hi = 0;
    if (outer_extent) {
      if (c.tmin < c.tmax) lo = c.tmin, hi = c.tmax;
    } else if (c.tpos < kInf && c.tneg > -kInf) {
      lo = c.tneg, hi = c.tpos;
    }
    st.centre = sp.pts[i] + st.n * ((lo + hi) / 2);
    st.half_w = (hi - lo) / 2;
  }
  return out;
}

std::string measure_depth(std::vector<Station>& st, const Spine&, const DistanceField& dt, const DepthOptions& o) {
  const double limit = o.depth_limit > 0 ? std::min(o.depth_limit, o.box_depth) : o.box_depth;
  int measured = 0;
  for (Station& s : st) {
    s.measured = false;
    const vec2d L = s.centre - s.n * s.half_w, R = s.centre + s.n * s.half_w;
    if (o.side || o.top) {
      const Mask2D& m = o.side ? *o.side : *o.top;
      // Side: the rows the cross-section spans (u = z). Top: its columns (v = z).
      const double a0 = o.side ? std::min(L.y, R.y) : std::min(L.x, R.x);
      const double a1 = o.side ? std::max(L.y, R.y) : std::max(L.x, R.x);
      const int lim = o.side ? m.height() : m.width();
      int z0 = std::numeric_limits<int>::max(), z1 = -1;
      for (int r = std::max(0, static_cast<int>(std::floor(a0 - 0.5))); r < lim && r + 0.5 <= a1 + 0.5; ++r) {
        if (r + 0.5 < a0 - 0.5) continue;
        const int n = o.side ? m.width() : m.height();
        for (int k = 0; k < n; ++k)
          if (o.side ? m.get(k, r) : m.get(r, k)) z0 = std::min(z0, k), z1 = std::max(z1, k + 1);
      }
      if (z1 > z0) {
        s.measured = true;
        s.z_c = (z0 + z1) / 2.0;
        s.half_d = (z1 - z0) / 2.0;
        ++measured;
        continue;
      }
    }
  }
  // Measured depth comes in whole pixels: smooth it over ±1.5 px of spine so pixel steps are not shape.
  if (measured > 0) {
    std::vector<double> lo(st.size()), hi(st.size());
    for (size_t i = 0; i < st.size(); ++i) lo[i] = st[i].z_c - st[i].half_d, hi[i] = st[i].z_c + st[i].half_d;
    for (size_t i = 0; i < st.size(); ++i) {
      if (!st[i].measured) continue;
      double a = 0, b = 0;
      int n = 0;
      for (size_t j = i >= 3 ? i - 3 : 0; j < st.size() && j <= i + 3; ++j)
        if (st[j].measured) a += lo[j], b += hi[j], ++n;
      st[i].z_c = (a + b) / (2 * n);
      st[i].half_d = (b - a) / (2 * n);
    }
  }
  // Inflation for every station without a measurement.
  std::vector<double> cue(st.size(), 1.0);
  if (o.cue && o.cue->w > 0 && o.cue->h > 0) {
    double sum = 0;
    int n = 0;
    for (size_t i = 0; i < st.size(); ++i) {
      const int x = std::clamp(static_cast<int>(std::floor(st[i].centre.x)), 0, o.cue->w - 1);
      const int y = std::clamp(static_cast<int>(std::floor(st[i].centre.y)), 0, o.cue->h - 1);
      cue[i] = o.cue->values[static_cast<size_t>(y) * o.cue->w + x];
      if (cue[i] > 0) sum += cue[i], ++n;
    }
    const double mean = n ? sum / n : 1.0;
    for (double& c : cue) c = mean > 0 ? lerp(1.0, c / mean, o.cue->weight) : 1.0;
  }
  for (size_t i = 0; i < st.size(); ++i) {
    Station& s = st[i];
    if (s.measured) continue;
    const double R = std::max(s.half_w, 1e-6);
    const double r = std::min(static_cast<double>(dt.sample(s.centre)), R);
    const double dome = std::sqrt(std::max(0.0, r * (2 * R - r)));
    const double round = i < o.roundness_per_station.size() ? o.roundness_per_station[i] : o.roundness;
    const double minr = i < o.min_ratio_per_station.size() ? o.min_ratio_per_station[i] : o.min_ratio;
    const double inflated = std::max(round * dome * std::max(0.0, cue[i]), minr * s.half_w);
    s.half_d = std::clamp(inflated, o.min_half_depth, std::max(o.min_half_depth, limit / 2));
    s.z_c = o.box_depth / 2;
  }
  const bool partial = measured > 0 && measured < static_cast<int>(st.size());
  if (measured == 0) return "inflation";
  const std::string src = o.side ? "side" : "top";
  return partial ? src + "+inflation" : src;
}

double station_deviation(const Station& a, const Station& b, const Station& x) {
  const double t = b.s > a.s ? (x.s - a.s) / (b.s - a.s) : 0;
  auto L = [](const Station& s) { return s.centre - s.n * s.half_w; };
  auto R = [](const Station& s) { return s.centre + s.n * s.half_w; };
  double d = std::max(length(L(x) - lerp(L(a), L(b), t)), length(R(x) - lerp(R(a), R(b), t)));
  d = std::max(d, std::abs((x.z_c - x.half_d) - lerp(a.z_c - a.half_d, b.z_c - b.half_d, t)));
  d = std::max(d, std::abs((x.z_c + x.half_d) - lerp(a.z_c + a.half_d, b.z_c + b.half_d, t)));
  return d;
}

Reduction reduce_stations(const std::vector<Station>& st, const std::vector<int>& fixed, const std::vector<bool>& forbidden,
                          const std::vector<double>* floor) {
  Reduction r;
  const int n = static_cast<int>(st.size());
  if (n == 0) return r;
  std::vector<bool> kept(st.size(), false);
  kept.front() = kept.back() = true;
  for (int f : fixed)
    if (f >= 0 && f < n) kept[static_cast<size_t>(f)] = true;
  for (int i = 0; i < n; ++i)
    if (kept[static_cast<size_t>(i)]) r.initial.push_back(i);
  std::vector<double> dev(st.size(), 0);
  auto recompute = [&](int lo, int hi) {  // kept stations lo < hi; everything between
    for (int i = lo + 1; i < hi; ++i) {
      const double f = floor && static_cast<size_t>(i) < floor->size() ? (*floor)[static_cast<size_t>(i)] : 0.0;
      dev[static_cast<size_t>(i)] =
          std::max(0.0, station_deviation(st[static_cast<size_t>(lo)], st[static_cast<size_t>(hi)], st[static_cast<size_t>(i)]) - f);
    }
  };
  for (size_t k = 0; k + 1 < r.initial.size(); ++k) recompute(r.initial[k], r.initial[k + 1]);
  // Forbidden stations (joint zones, where the loops stand in) cannot be refined: they do not count.
  auto max_dev = [&] {
    double m = 0;
    for (int i = 0; i < n; ++i)
      if (!kept[static_cast<size_t>(i)] && (forbidden.empty() || !forbidden[static_cast<size_t>(i)]))
        m = std::max(m, dev[static_cast<size_t>(i)]);
    return m;
  };
  r.errors.push_back(max_dev());
  while (true) {
    int best = -1;
    for (int i = 0; i < n; ++i) {
      if (kept[static_cast<size_t>(i)] || (!forbidden.empty() && forbidden[static_cast<size_t>(i)])) continue;
      if (best < 0 || dev[static_cast<size_t>(i)] > dev[static_cast<size_t>(best)]) best = i;
    }
    if (best < 0 || dev[static_cast<size_t>(best)] <= 0) break;  // everything left is below its floor
    kept[static_cast<size_t>(best)] = true;
    r.order.push_back(best);
    int lo = best - 1, hi = best + 1;
    while (!kept[static_cast<size_t>(lo)]) --lo;
    while (!kept[static_cast<size_t>(hi)]) ++hi;
    dev[static_cast<size_t>(best)] = 0;
    recompute(lo, best);
    recompute(best, hi);
    r.errors.push_back(max_dev());
  }
  return r;
}

}  // namespace pixel3d::lowpoly
