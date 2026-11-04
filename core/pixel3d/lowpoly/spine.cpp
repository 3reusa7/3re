#include "pixel3d/lowpoly/spine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include "pixel3d/polygon/simplify.hpp"

namespace pixel3d::lowpoly {
namespace {

vec2d unit(vec2d v) {
  const double l = length(v);
  return l > 0 ? v * (1 / l) : vec2d{0, -1};
}

vec2d normal_of(vec2d t) { return {-t.y, t.x}; }

bool inside(const Mask2D& m, vec2d p) {
  return m.get_or_false(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)));
}

// Points every `step` along the polyline.
std::vector<vec2d> resample(const std::vector<vec2d>& in, double step) {
  if (in.size() < 2) return in;
  std::vector<vec2d> out{in.front()};
  double carry = 0;
  for (size_t i = 1; i < in.size(); ++i) {
    const vec2d a = in[i - 1], b = in[i];
    const double len = length(b - a);
    double t = step - carry;
    while (t <= len) {
      out.push_back(a + (b - a) * (t / len));
      t += step;
    }
    carry = len - (t - step);
  }
  if (length(out.back() - in.back()) > 1e-9) out.push_back(in.back());
  return out;
}

void fill_normals(Spine& sp) {
  sp.normals.resize(sp.pts.size());
  for (size_t i = 0; i < sp.pts.size(); ++i) sp.normals[i] = normal_of(sp.tangent(i));
}

// Zhang–Suen thinning; returns the skeleton pixels (8-connected, one pixel wide).
Mask2D thin(const Mask2D& m) {
  const int w = m.width() + 2, h = m.height() + 2;
  std::vector<uint8_t> a(static_cast<size_t>(w) * h, 0);
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) a[static_cast<size_t>(y + 1) * w + x + 1] = m.get(x, y);
  auto px = [&](int x, int y) -> int { return a[static_cast<size_t>(y) * w + x]; };
  bool changed = true;
  std::vector<size_t> del;
  while (changed) {
    changed = false;
    for (int pass = 0; pass < 2; ++pass) {
      del.clear();
      for (int y = 1; y < h - 1; ++y)
        for (int x = 1; x < w - 1; ++x) {
          if (!px(x, y)) continue;
          const int p[8] = {px(x, y - 1), px(x + 1, y - 1), px(x + 1, y), px(x + 1, y + 1),
                            px(x, y + 1), px(x - 1, y + 1), px(x - 1, y), px(x - 1, y - 1)};
          int b = 0, t = 0;
          for (int k = 0; k < 8; ++k) {
            b += p[k];
            t += (p[k] == 0 && p[(k + 1) % 8] == 1);
          }
          if (b < 2 || b > 6 || t != 1) continue;
          if (pass == 0 ? (p[0] * p[2] * p[4] == 0 && p[2] * p[4] * p[6] == 0)
                        : (p[0] * p[2] * p[6] == 0 && p[0] * p[4] * p[6] == 0))
            del.push_back(static_cast<size_t>(y) * w + x);
        }
      for (size_t i : del) a[i] = 0;
      changed |= !del.empty();
    }
  }
  Mask2D out(m.width(), m.height());
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) out.set(x, y, a[static_cast<size_t>(y + 1) * w + x + 1] != 0);
  return out;
}

// Farthest skeleton pixel from `src` (Dijkstra over 8-neighbours) and the parent links.
int farthest(const Mask2D& sk, int src, std::vector<int>& parent) {
  const int w = sk.width(), h = sk.height();
  std::vector<double> dist(static_cast<size_t>(w) * h, std::numeric_limits<double>::infinity());
  parent.assign(dist.size(), -1);
  using Item = std::pair<double, int>;
  std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
  dist[static_cast<size_t>(src)] = 0;
  q.push({0, src});
  int best = src;
  while (!q.empty()) {
    const auto [d, i] = q.top();
    q.pop();
    if (d > dist[static_cast<size_t>(i)]) continue;
    if (d > dist[static_cast<size_t>(best)] || (d == dist[static_cast<size_t>(best)] && i < best)) best = i;
    const int x = i % w, y = i / w;
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        if ((!dx && !dy) || !sk.get_or_false(x + dx, y + dy)) continue;
        const int j = (y + dy) * w + x + dx;
        const double nd = d + ((dx && dy) ? std::sqrt(2.0) : 1.0);
        if (nd < dist[static_cast<size_t>(j)]) {
          dist[static_cast<size_t>(j)] = nd;
          parent[static_cast<size_t>(j)] = i;
          q.push({nd, j});
        }
      }
  }
  return best;
}

}  // namespace

double Spine::length() const {
  double l = 0;
  for (size_t i = 1; i < pts.size(); ++i) l += pixel3d::length(pts[i] - pts[i - 1]);
  return l;
}

vec2d Spine::tangent(size_t i) const {
  if (pts.size() < 2) return {0, -1};
  const size_t k = 4;
  const size_t a = i >= k ? i - k : 0, b = std::min(pts.size() - 1, i + k);
  return unit(pts[b] - pts[a]);
}

void Spine::extend_start(double dist) {
  if (pts.size() < 2 || dist <= 0) return;
  const vec2d t = tangent(0), n0 = normals.front();
  const int k = static_cast<int>(std::ceil(dist / step));
  std::vector<vec2d> p, n;
  for (int i = k; i >= 1; --i) {
    p.push_back(pts.front() - t * (i * step));
    n.push_back(n0);
  }
  pts.insert(pts.begin(), p.begin(), p.end());
  normals.insert(normals.begin(), n.begin(), n.end());
}

void Spine::reverse() {
  std::reverse(pts.begin(), pts.end());
  std::reverse(normals.begin(), normals.end());
  for (vec2d& n : normals) n = n * -1;
}

Polygons2D part_outline(const Mask2D& m) { return simplify_feature_preserving(mask_to_polygons(m), m, SimplifyOptions{}); }

Axis principal_axis(const Mask2D& m) {
  Axis ax;
  double n = 0, sx = 0, sy = 0;
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x)
      if (m.get(x, y)) n += 1, sx += x + 0.5, sy += y + 0.5;
  if (n == 0) return ax;
  ax.centroid = {sx / n, sy / n};
  double cxx = 0, cyy = 0, cxy = 0;
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x)
      if (m.get(x, y)) {
        const double dx = x + 0.5 - ax.centroid.x, dy = y + 0.5 - ax.centroid.y;
        cxx += dx * dx, cyy += dy * dy, cxy += dx * dy;
      }
  cxx /= n, cyy /= n, cxy /= n;
  const double tr = cxx + cyy, det = cxx * cyy - cxy * cxy, disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
  const double l1 = tr / 2 + disc, l2 = std::max(1e-12, tr / 2 - disc);
  vec2d v = std::abs(cxy) > 1e-12 ? vec2d{l1 - cyy, cxy} : (cxx >= cyy ? vec2d{1, 0} : vec2d{0, 1});
  v = unit(v);
  if (v.y > 0 || (v.y == 0 && v.x < 0)) v = v * -1;
  ax.dir = v;
  ax.elongation = std::sqrt(l1 / l2);
  return ax;
}

Spine straight_spine(const Mask2D& m, vec2d dir) {
  Spine sp;
  sp.straight = true;
  dir = unit(dir);
  const Axis ax = principal_axis(m);
  double t0 = std::numeric_limits<double>::infinity(), t1 = -t0;
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x)
      if (m.get(x, y))
        for (const vec2d c : {vec2d{double(x), double(y)}, vec2d{x + 1.0, double(y)}, vec2d{double(x), y + 1.0},
                              vec2d{x + 1.0, y + 1.0}}) {
          const double t = dot(c - ax.centroid, dir);
          t0 = std::min(t0, t), t1 = std::max(t1, t);
        }
  if (!(t1 > t0)) return sp;
  const int n = static_cast<int>(std::ceil((t1 - t0) / sp.step));
  for (int i = 0; i <= n; ++i) sp.pts.push_back(ax.centroid + dir * std::min(t1, t0 + i * sp.step));
  sp.normals.assign(sp.pts.size(), normal_of(dir));
  return sp;
}

Spine medial_spine(const Mask2D& m, vec2d hint) {
  const Mask2D sk = thin(m);
  int any = -1;
  for (int i = 0; i < sk.width() * sk.height() && any < 0; ++i)
    if (sk.get(i % sk.width(), i / sk.width())) any = i;
  if (any < 0) return straight_spine(m, principal_axis(m).dir);
  std::vector<int> parent;
  const int a = farthest(sk, any, parent);
  const int b = farthest(sk, a, parent);
  std::vector<vec2d> path;
  for (int i = b; i >= 0; i = parent[static_cast<size_t>(i)])
    path.push_back({i % sk.width() + 0.5, i / sk.width() + 0.5});
  if (path.size() < 3) return straight_spine(m, principal_axis(m).dir);
  // Smooth (window ±2), keeping the ends.
  std::vector<vec2d> sm = path;
  for (size_t i = 1; i + 1 < path.size(); ++i) {
    const size_t lo = i >= 2 ? i - 2 : 0, hi = std::min(path.size() - 1, i + 2);
    vec2d s{};
    for (size_t j = lo; j <= hi; ++j) s = s + path[j];
    sm[i] = s * (1.0 / static_cast<double>(hi - lo + 1));
  }
  Spine sp;
  sp.pts = resample(sm, sp.step);
  fill_normals(sp);
  // Extend both ends along their tangents to the outline.
  for (int end = 0; end < 2; ++end) {
    const vec2d t = sp.tangent(0) * -1.0;
    double d = 0;
    while (d < 4 * (m.width() + m.height()) && inside(m, sp.pts.front() + t * (d + 0.25))) d += 0.25;
    sp.extend_start(d + 0.125);
    sp.reverse();
  }
  fill_normals(sp);
  if (!std::isnan(hint.x) && length(sp.pts.back() - hint) < length(sp.pts.front() - hint)) {
    sp.reverse();
    fill_normals(sp);
  }
  return sp;
}

}  // namespace pixel3d::lowpoly
