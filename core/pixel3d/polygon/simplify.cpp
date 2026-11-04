#include "pixel3d/polygon/simplify.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "pixel3d/polygon/raster.hpp"

namespace pixel3d {
namespace {

constexpr double kEps = 1e-9;
constexpr double kMaxDeviation = 0.5;

int dir_of(vec2d d) {
  if (d.x > 0) return 0;
  if (d.y > 0) return 1;
  if (d.x < 0) return 2;
  return 3;
}

vec2d unit_vec(int dir) {
  static const vec2d u[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
  return u[dir];
}

// Drops duplicate and collinear vertices of a rectilinear ring.
Ring clean_ring(const Ring& in) {
  Ring r;
  for (const vec2d& p : in)
    if (r.empty() || !(r.back() == p)) r.push_back(p);
  if (r.size() > 1 && r.front() == r.back()) r.pop_back();
  bool changed = true;
  while (changed && r.size() >= 3) {
    changed = false;
    for (size_t i = 0; i < r.size(); ++i) {
      const vec2d a = r[(i + r.size() - 1) % r.size()], b = r[i], c = r[(i + 1) % r.size()];
      if (std::abs(cross(b - a, c - b)) < kEps) {
        r.erase(r.begin() + static_cast<long>(i));
        changed = true;
        break;
      }
    }
  }
  return r;
}

std::optional<double> intersect_param(vec2d q, vec2d t, vec2d a, vec2d b) {
  // Parameter r along segment a→b where the line q + s·t crosses it.
  const vec2d d = b - a;
  const double den = cross(t, d);
  if (std::abs(den) < kEps) return std::nullopt;
  return cross(t, q - a) / den;
}

// Candidate staircase: unit-length minority edges alternating with majority
// runs in a {k, k+1} window. Indices are edges of the rotated ring.
struct Cand {
  std::vector<int> units;
  std::vector<double> majors;  // majors[i] lies between units[i] and units[i+1]
  int dM = 0, dm = 0;
};

struct Staircase {
  int a = 0, b = 0;  // anchor edges; removed vertices are P(a+1)..P(b)
  int fu = 0, lu = 0;  // first / last unit edge
  vec2d x1, x2;      // replacement segment
  int units = 0;
  double deviation = 0;
  double box[4] = {0, 0, 0, 0};  // influence box u0 v0 u1 v1
  bool active = false;
};

struct RingWork {
  Ring pts;  // rotated so that edge 0 is a feature edge
  std::vector<Staircase> stairs;
};

// Majority runs must stay within {k, k+1}; the first run may be partial (shorter).
bool fits_window(const std::vector<double>& majors, double next) {
  double lo = next, hi = next;
  for (size_t i = 1; i < majors.size(); ++i) lo = std::min(lo, majors[i]), hi = std::max(hi, majors[i]);
  if (hi - lo > 1 + kEps) return false;
  return majors.empty() || majors[0] <= hi + kEps;
}

RingWork analyse_ring(const Ring& ring, const SimplifyOptions& opt) {
  RingWork w;
  Ring r = clean_ring(ring);
  const int n = static_cast<int>(r.size());
  w.pts = r;
  if (n < 4) return w;
  std::vector<int> dir(n);
  std::vector<double> len(n);
  for (int i = 0; i < n; ++i) {
    const vec2d d = r[(i + 1) % n] - r[i];
    dir[i] = dir_of(d);
    len[i] = std::abs(d.x) + std::abs(d.y);
  }
  auto turn = [&](int i) { return cross(unit_vec(dir[i]), unit_vec(dir[(i + 1) % n])) > 0 ? 1 : -1; };
  // Rotate so that edge 0 is bounded by two equal turns: a feature edge that
  // can never be inside a staircase (closed rings always have one).
  int rot = -1;
  for (int i = 0; i < n && rot < 0; ++i)
    if (turn((i + n - 1) % n) == turn(i)) rot = i;
  if (rot < 0) return w;
  std::rotate(r.begin(), r.begin() + rot, r.end());
  std::rotate(dir.begin(), dir.begin() + rot, dir.end());
  std::rotate(len.begin(), len.begin() + rot, len.end());
  w.pts = r;
  auto E = [&](int i) { return ((i % n) + n) % n; };
  auto P = [&](int i) { return r[E(i)]; };
  auto D = [&](int i) { return dir[E(i)]; };
  auto L = [&](int i) { return len[E(i)]; };
  auto is_unit = [&](int i) { return std::abs(L(i) - 1) < kEps; };

  // Maximal alternating runs [s..t]: directions repeat every second edge.
  std::vector<std::pair<int, int>> runs;
  for (int s = 0; s < n;) {
    int t = s + 1;
    while (t + 1 <= n && D(t + 1) == D(t - 1)) ++t;
    if (t - s + 1 >= 3) runs.emplace_back(s, t);
    s = t;
  }

  // Turns a candidate into a concrete replacement segment; nullopt if any
  // removed corner would end up more than 0.5 px away.
  auto make_staircase = [&](const Cand& c, int run_s, int run_t) -> std::optional<Staircase> {
    std::vector<vec2d> mids;
    for (int u : c.units) mids.push_back((P(u) + P(u + 1)) * 0.5);
    // Snap when every interior run equals k and the end runs are partial (≤ k).
    const size_t m = c.majors.size();
    const double k = c.majors[m / 2];
    bool snap = opt.angle_snap;
    for (size_t i = 0; i < m && snap; ++i) {
      const bool end = (i == 0 || i + 1 == m) && m >= 3;
      snap = end ? c.majors[i] <= k + kEps : std::abs(c.majors[i] - k) < kEps;
    }
    vec2d q, t;
    if (snap) {
      t = unit_vec(c.dM) * k + unit_vec(c.dm);
      q = mids.size() >= 3 ? mids[1] : mids[0];
    } else {
      vec2d sum{0, 0};
      for (const vec2d& p : mids) sum = sum + p;
      q = sum * (1.0 / mids.size());
      t = mids.back() - mids.front();
    }
    double kmax = 0;
    for (double x : c.majors) kmax = std::max(kmax, x);
    // Anchors: a long straight flank edge inside the run, else the outermost
    // unit edge itself, else that unit's midpoint (curves chain through these).
    Staircase s;
    s.units = static_cast<int>(c.units.size());
    const int fu = c.units.front(), lu = c.units.back();
    s.fu = fu;
    s.lu = lu;
    s.a = fu;
    s.x1 = mids.front();
    bool anchored = false;
    // Run-boundary edges border a feature corner, so they may anchor even when short.
    if (fu - 1 >= run_s && (L(fu - 1) > kmax + 1 + kEps || fu - 1 == run_s)) {
      const auto r0 = intersect_param(q, t, P(fu - 1), P(fu));
      if (r0 && *r0 > kEps && *r0 <= 1 + kEps) {
        s.a = fu - 1;
        s.x1 = P(fu - 1) + (P(fu) - P(fu - 1)) * *r0;
        anchored = true;
      }
    }
    if (!anchored) {
      const auto r0 = intersect_param(q, t, P(fu), P(fu + 1));
      if (r0 && *r0 > kEps && *r0 <= 1 + kEps) s.x1 = P(fu) + (P(fu + 1) - P(fu)) * *r0;
    }
    s.b = lu;
    s.x2 = mids.back();
    anchored = false;
    if (lu + 1 <= run_t && (L(lu + 1) > kmax + 1 + kEps || lu + 1 == run_t)) {
      const auto r1 = intersect_param(q, t, P(lu + 1), P(lu + 2));
      if (r1 && *r1 >= -kEps && *r1 < 1 - kEps) {
        s.b = lu + 1;
        s.x2 = P(lu + 1) + (P(lu + 2) - P(lu + 1)) * *r1;
        anchored = true;
      }
    }
    if (!anchored) {
      const auto r1 = intersect_param(q, t, P(lu), P(lu + 1));
      if (r1 && *r1 >= -kEps && *r1 < 1 - kEps) s.x2 = P(lu) + (P(lu + 1) - P(lu)) * *r1;
    }
    const vec2d seg = s.x2 - s.x1;
    const double seg_len = length(seg);
    if (seg_len < kEps || s.b - s.a >= n - 1 || s.b < s.a) return std::nullopt;
    s.box[0] = std::min(s.x1.x, s.x2.x), s.box[1] = std::min(s.x1.y, s.x2.y);
    s.box[2] = std::max(s.x1.x, s.x2.x), s.box[3] = std::max(s.x1.y, s.x2.y);
    for (int i = s.a + 1; i <= s.b; ++i) {
      const vec2d p = P(i);
      s.deviation = std::max(s.deviation, std::abs(cross(seg, p - s.x1)) / seg_len);
      s.box[0] = std::min(s.box[0], p.x), s.box[1] = std::min(s.box[1], p.y);
      s.box[2] = std::max(s.box[2], p.x), s.box[3] = std::max(s.box[3], p.y);
    }
    if (s.deviation > kMaxDeviation + kEps) return std::nullopt;
    s.active = true;
    return s;
  };

  // Two passes per run (either direction may be the unit-step one; curves
  // switch between them). A window break starts the next candidate on the
  // last unit, so consecutive pieces chain through shared unit midpoints.
  std::vector<Staircase> found;
  for (auto [s, t] : runs)
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<Cand> chain;
      Cand cur;
      cur.dm = D(s + pass);
      cur.dM = D(s + 1 - pass);
      auto close = [&](bool keep_last) {
        const int last = cur.units.empty() ? -1 : cur.units.back();
        if (static_cast<int>(cur.units.size()) >= opt.min_unit_steps) chain.push_back(cur);
        cur.units.clear();
        cur.majors.clear();
        if (keep_last && last >= 0) cur.units.push_back(last);
      };
      double pending = 0;
      for (int i = s; i <= t; ++i) {
        if (D(i) != cur.dm) {
          pending = L(i);
          continue;
        }
        if (!is_unit(i)) {
          close(false);
          continue;
        }
        if (!cur.units.empty() && !fits_window(cur.majors, pending)) close(true);
        if (!cur.units.empty()) cur.majors.push_back(pending);
        cur.units.push_back(i);
      }
      close(false);
      // Split pieces that fail the deviation check into two-unit pieces, then
      // merge neighbours back while the merged piece stays valid.
      std::vector<Cand> pieces;
      for (const Cand& c : chain) {
        if (make_staircase(c, s, t)) {
          pieces.push_back(c);
          continue;
        }
        for (size_t u = 0; u + 1 < c.units.size(); ++u) {
          Cand two = c;
          two.units = {c.units[u], c.units[u + 1]};
          two.majors = {c.majors[u]};
          pieces.push_back(two);
        }
      }
      std::vector<Cand> merged;
      for (const Cand& c : pieces) {
        if (!merged.empty() && merged.back().units.back() == c.units.front()) {
          Cand mc = merged.back();
          mc.units.insert(mc.units.end(), c.units.begin() + 1, c.units.end());
          mc.majors.insert(mc.majors.end(), c.majors.begin(), c.majors.end());
          if (make_staircase(mc, s, t)) {
            merged.back() = mc;
            continue;
          }
        }
        merged.push_back(c);
      }
      for (const Cand& c : merged)
        if (auto st = make_staircase(c, s, t)) found.push_back(*st);
    }

  // Greedy: larger staircases first; removal ranges must not overlap. Two
  // staircases may share an anchor edge when their endpoints are in order
  // (equal endpoints are how chained curve pieces meet).
  std::sort(found.begin(), found.end(), [](const Staircase& x, const Staircase& y) { return x.units > y.units; });
  for (const Staircase& s : found) {
    bool ok = true;
    for (const Staircase& o : w.stairs) {
      const Staircase& lo = o.a <= s.a ? o : s;
      const Staircase& hi = o.a <= s.a ? s : o;
      if (lo.b > hi.a) ok = false;
      if (lo.b == hi.a) {
        const vec2d d = P(hi.a + 1) - P(hi.a);
        if (dot(lo.x2 - P(hi.a), d) > dot(hi.x1 - P(hi.a), d) + kEps) ok = false;
      }
      if (!ok) break;
    }
    if (ok) w.stairs.push_back(s);
  }

  // Where one piece ends on its last unit and the next begins on the very next
  // edge (a switch between the two passes, e.g. at 45°), drop the corner
  // between them when it stays within 0.5 px of the joining segment.
  std::sort(w.stairs.begin(), w.stairs.end(), [](const Staircase& x, const Staircase& y) { return x.a < y.a; });
  for (size_t k = 0; k + 1 < w.stairs.size(); ++k) {
    Staircase& A = w.stairs[k];
    const Staircase& B = w.stairs[k + 1];
    if (A.b != A.lu || B.a != B.fu || B.fu != A.lu + 1) continue;
    const vec2d corner = P(A.lu + 1), seg = B.x1 - A.x2;
    const double sl = length(seg);
    if (sl < kEps || std::abs(cross(seg, corner - A.x2)) / sl > kMaxDeviation) continue;
    A.b = A.lu + 1;  // B.x1 lies on edge A.lu + 1, so the pieces now meet there
    A.deviation = std::max(A.deviation, std::abs(cross(seg, corner - A.x2)) / sl);
    A.box[0] = std::min(A.box[0], corner.x), A.box[1] = std::min(A.box[1], corner.y);
    A.box[2] = std::max(A.box[2], corner.x), A.box[3] = std::max(A.box[3], corner.y);
  }
  return w;
}

Ring build_ring(const RingWork& w) {
  const int n = static_cast<int>(w.pts.size());
  if (w.stairs.empty() || n < 4) return w.pts;
  std::vector<int> removed(n, 0);
  std::vector<std::vector<vec2d>> insert_at(n);
  for (const Staircase& s : w.stairs) {
    if (!s.active) continue;
    for (int k = s.a + 1; k <= s.b; ++k) removed[((k % n) + n) % n] = 1;
    insert_at[(((s.a + 1) % n) + n) % n] = {s.x1, s.x2};
  }
  Ring out;
  for (int k = 0; k < n; ++k) {
    for (const vec2d& p : insert_at[k])
      if (out.empty() || !(out.back() == p)) out.push_back(p);
    if (!removed[k] && (out.empty() || !(out.back() == w.pts[k]))) out.push_back(w.pts[k]);
  }
  if (out.size() > 1 && out.front() == out.back()) out.pop_back();
  return out;
}

}  // namespace

Polygons2D simplify_feature_preserving(const Polygons2D& exact, const Mask2D& source, const SimplifyOptions& opt,
                                       SimplifyStats* stats) {
  std::vector<RingWork> rings;
  for (const Ring& r : exact) rings.push_back(analyse_ring(r, opt));
  int candidates = 0;
  for (const auto& w : rings) candidates += static_cast<int>(w.stairs.size());

  Polygons2D out;
  bool verified = false;
  for (int iter = 0; iter < 64; ++iter) {
    out.clear();
    for (const auto& w : rings) out.push_back(build_ring(w));
    const Mask2D got = rasterize_pixel_centres(out, source.width(), source.height());
    if (got == source) {
      verified = true;
      break;
    }
    // Revert every active staircase whose influence box touches a wrong pixel.
    bool reverted_any = false;
    for (int v = 0; v < source.height(); ++v)
      for (int u = 0; u < source.width(); ++u) {
        if (got.get(u, v) == source.get(u, v)) continue;
        for (auto& w : rings)
          for (auto& s : w.stairs)
            if (s.active && u + 1.5 >= s.box[0] && u - 0.5 <= s.box[2] && v + 1.5 >= s.box[1] &&
                v - 0.5 <= s.box[3]) {
              s.active = false;
              reverted_any = true;
            }
      }
    if (!reverted_any)
      for (auto& w : rings)
        for (auto& s : w.stairs) s.active = false;
  }

  if (!verified) {  // give up on simplification: the staircase rings are exact by construction
    for (auto& w : rings)
      for (auto& s : w.stairs) s.active = false;
    out.clear();
    for (const auto& w : rings) out.push_back(build_ring(w));
  }

  if (stats) {
    *stats = SimplifyStats{};
    for (const auto& w : rings)
      for (const auto& s : w.stairs)
        if (s.active) {
          ++stats->staircases_replaced;
          stats->max_deviation = std::max(stats->max_deviation, s.deviation);
        }
    stats->reverted = candidates - stats->staircases_replaced;
  }
  return out;
}

}  // namespace pixel3d
