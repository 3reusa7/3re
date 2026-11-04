#include "pixel3d/lowpoly/allocate.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <string>

namespace pixel3d::lowpoly {
namespace {

// Everything about one tube at one joint-loop count that does not depend on N or the extra stations.
struct Prep {
  std::vector<double> loop_s;
  std::vector<int> loop_joint, loop_index, fixed;
  std::vector<bool> forbidden;
  Reduction red;
  std::vector<double> env;  // non-increasing envelope of red.errors
  int base_rings = 0;       // rings with no extra station
  int min_extra = 0;
};

struct Work {
  const TubeSpec* t = nullptr;
  std::vector<Prep> by_loops;  // index = loops − 1
  Cap cs = Cap::Flat, ce = Cap::Flat;
  double max_half_w = 0;
  double importance = 0;
  int sides = 4, loops = 1, extra = 0;
  const Prep& prep() const { return by_loops[static_cast<size_t>(loops - 1)]; }
  int rings(int extra_) const { return prep().base_rings + extra_; }
  int tris(int n, int extra_) const { return tube_triangles(n, rings(extra_), cs, ce); }
  int tris() const { return tris(sides, extra); }
  double floor = 0;  // the tube's smallest error floor (sides follow the same stop rule)
  double sag(int n) const { return std::max(0.0, max_half_w * (1 - std::cos(std::numbers::pi / n)) - floor); }
};

size_t nearest_index(const std::vector<Station>& d, double s) {
  size_t best = 0;
  for (size_t i = 1; i < d.size(); ++i)
    if (std::abs(d[i].s - s) < std::abs(d[best].s - s)) best = i;
  return best;
}

double loop_spacing(const JointSpec& j) { return std::clamp(0.3 * j.width, 0.75, 4.0); }

Prep prepare(const TubeSpec& t, int loops, Cap cs, Cap ce) {
  Prep p;
  const auto& d = t.dense;
  const double smin = d.front().s, smax = d.back().s;
  const size_t n = d.size();
  p.forbidden.assign(n, false);
  for (size_t i = 0; i < n; ++i)
    if (d[i].s < 0 && i > 0) p.forbidden[i] = true;  // inside the parent: only the base cap ring
  for (size_t j = 0; j < t.joints.size(); ++j) {
    const JointSpec& js = t.joints[j];
    const double dl = loop_spacing(js);
    std::vector<double> ss;
    for (int i = 0; i < loops; ++i)
      ss.push_back(js.attachment ? js.s + (i + 0.5) * dl : js.s + (i - (loops - 1) / 2.0) * dl);
    for (int i = 0; i < loops; ++i) {
      const double s = std::clamp(ss[static_cast<size_t>(i)], smin + 0.25, smax - 0.25);
      p.loop_s.push_back(s);
      p.loop_joint.push_back(static_cast<int>(j));
      p.loop_index.push_back(i);
    }
    const double lo = ss.front() - dl / 2, hi = ss.back() + dl / 2;
    for (size_t i = 0; i < n; ++i)
      if (d[i].s > lo && d[i].s < hi) p.forbidden[i] = true;
  }
  // Loops enter the reducer as fixed stations (snapped), so the profile error knows they are there.
  std::set<int> fixed;
  for (double s : p.loop_s) {
    size_t k = nearest_index(d, s);
    k = std::clamp<size_t>(k, 1, n >= 2 ? n - 2 : 0);
    fixed.insert(static_cast<int>(k));
  }
  p.fixed.assign(fixed.begin(), fixed.end());
  for (int f : p.fixed) p.forbidden[static_cast<size_t>(f)] = true;
  p.red = reduce_stations(d, p.fixed, p.forbidden, t.error_floor.empty() ? nullptr : &t.error_floor);
  double m = std::numeric_limits<double>::infinity();
  for (double e : p.red.errors) p.env.push_back(m = std::min(m, e));
  // Rings: kept stations that are not loop stand-ins and not apex ends, plus the loops.
  int kept = static_cast<int>(p.red.initial.size()) - static_cast<int>(p.fixed.size());
  if (cs == Cap::Apex) --kept;
  if (ce == Cap::Apex) --kept;
  p.base_rings = kept + static_cast<int>(p.loop_s.size());
  // Two flat caps need two rings; an apex needs one.
  const int need = (cs == Cap::Flat && ce == Cap::Flat) ? 2 : 1;
  p.min_extra = std::max(0, need - p.base_rings);
  return p;
}

}  // namespace

Expected<Allocation> allocate(const Layout& l, const Settings& s) {
  if (s.tri_budget < kMinBudget || s.tri_budget > kMaxBudget)
    return make_error("lowpoly.budget_out_of_range",
                      "the triangle budget must be " + std::to_string(kMinBudget) + "–" + std::to_string(kMaxBudget),
                      {{"budget", s.tri_budget}});
  const int jreq = std::clamp(s.joint_loops, 1, 3);
  const int nmin = std::max(4, s.min_sides & ~1), nmax = std::clamp(s.max_sides & ~1, nmin, 8);
  Allocation a;
  a.budget = s.tri_budget;
  a.base_budget = s.hull ? s.tri_budget / 2 : s.tri_budget;
  std::vector<Work> w(l.tubes.size());
  for (size_t i = 0; i < l.tubes.size(); ++i) {
    const TubeSpec& t = l.tubes[i];
    Work& k = w[i];
    k.t = &t;
    for (const auto& st : t.dense) k.max_half_w = std::max(k.max_half_w, st.half_w);
    k.floor = t.error_floor.empty() ? 0 : *std::min_element(t.error_floor.begin(), t.error_floor.end());
    const bool free_start = !t.branch;  // a branch starts inside its parent
    k.cs = (free_start && t.dense.front().half_w < 0.25 * k.max_half_w) ? Cap::Apex : Cap::Flat;
    k.ce = t.dense.back().half_w < 0.25 * k.max_half_w ? Cap::Apex : Cap::Flat;
    for (int j = 1; j <= jreq; ++j) k.by_loops.push_back(prepare(t, j, k.cs, k.ce));
    // Visual importance: silhouette error shows along the outline (∝ √area), heads count more,
    // and shapes that deviate much from a straight tube for their size count more.
    const double complexity = std::min(3.0, k.prep().red.errors.front() / std::max(1.0, k.max_half_w));
    k.importance = std::sqrt(t.area_share) * t.type_weight * (1 + complexity);
    k.sides = nmin;
    k.loops = 1;
    k.extra = k.prep().min_extra;
  }
  auto total = [&] {
    int n = 0;
    for (const auto& k : w) n += k.tris();
    return n;
  };
  const int minimum = total();
  if (minimum > a.base_budget)
    return make_error("lowpoly.budget_too_small",
                      "the parts need at least " + std::to_string(minimum) + " triangles" + (s.hull ? " (×2 with the hull)" : "") +
                          "; raise the budget or merge parts",
                      {{"minimum", s.hull ? 2 * minimum : minimum}, {"budget", s.tri_budget}});
  // Symmetric partners (left/right) are upgraded together so the model stays symmetric.
  std::vector<std::vector<size_t>> groups;
  for (size_t i = 0; i < w.size(); ++i) {
    const int pt = l.tubes[i].partner;
    if (pt >= 0 && static_cast<size_t>(pt) < i && l.tubes[static_cast<size_t>(pt)].partner == static_cast<int>(i)) continue;
    groups.push_back({i});
    if (pt >= 0 && static_cast<size_t>(pt) > i && l.tubes[static_cast<size_t>(pt)].partner == static_cast<int>(i))
      groups.back().push_back(static_cast<size_t>(pt));
  }
  auto importance = [&](const std::vector<size_t>& g) {
    double v = 0;
    for (size_t i : g) v += w[i].importance;
    return v;
  };
  std::stable_sort(groups.begin(), groups.end(), [&](const auto& x, const auto& y) { return importance(x) > importance(y); });
  int left = a.base_budget - total();
  // Phase 1: six sides everywhere (most important first).
  bool sides_reduced = false;
  for (const auto& g : groups) {
    const int target = std::min(6, nmax);
    int cost = 0;
    for (size_t i : g) cost += w[i].sides < target ? w[i].tris(target, w[i].extra) - w[i].tris() : 0;
    if (cost == 0) continue;
    if (cost > left) {
      sides_reduced = true;
      continue;
    }
    for (size_t i : g) w[i].sides = std::max(w[i].sides, target);
    left -= cost;
  }
  // Phase 2: the requested loops on every joint.
  bool loops_reduced = false;
  for (const auto& g : groups) {
    if (w[g.front()].t->joints.empty() || jreq == 1) continue;
    std::vector<Work> trial;
    int cost = 0;
    for (size_t i : g) {
      Work k = w[i];
      const int before = k.tris();
      k.loops = jreq;
      k.extra = std::max(k.extra, k.prep().min_extra);
      cost += k.tris() - before;
      trial.push_back(k);
    }
    if (cost > left) {
      loops_reduced = true;
      continue;
    }
    for (size_t n = 0; n < g.size(); ++n) w[g[n]] = trial[n];
    left -= cost;
  }
  // Phase 3: greedy refinement, one group action at a time.
  auto station_step = [&](const Work& k) {  // stations to the next real error drop
    const Prep& p = k.prep();
    if (k.extra >= static_cast<int>(p.red.order.size())) return 0;
    int m = 1;
    while (k.extra + m < static_cast<int>(p.env.size()) - 1 &&
           p.env[static_cast<size_t>(k.extra + m)] >= p.env[static_cast<size_t>(k.extra)] - 1e-12)
      ++m;
    return m;
  };
  while (true) {
    double best = 0;
    int bg = -1, kind = 0, bcost = 0;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
      const auto& g = groups[gi];
      double gain = 0;
      int cost = 0;
      for (size_t i : g) {
        const Work& k = w[i];
        const int m = station_step(k);
        if (m == 0) continue;
        const Prep& p = k.prep();
        gain += k.importance * (p.env[static_cast<size_t>(k.extra)] - p.env[static_cast<size_t>(k.extra + m)]);
        cost += 2 * k.sides * m;
      }
      if (gain > 1e-12 && cost <= left && gain / cost > best) best = gain / cost, bg = static_cast<int>(gi), kind = 1, bcost = cost;
      gain = 0;
      cost = 0;
      for (size_t i : g) {
        const Work& k = w[i];
        if (k.sides + 2 > nmax) continue;
        gain += k.importance * (k.sag(k.sides) - k.sag(k.sides + 2));
        cost += k.tris(k.sides + 2, k.extra) - k.tris();
      }
      if (gain > 1e-12 && cost <= left && gain / cost > best) best = gain / cost, bg = static_cast<int>(gi), kind = 2, bcost = cost;
    }
    if (bg < 0) break;
    for (size_t i : groups[static_cast<size_t>(bg)]) {
      Work& k = w[i];
      if (kind == 1) k.extra += station_step(k);
      else if (k.sides + 2 <= nmax) k.sides += 2;
    }
    left -= bcost;
  }
  // Plans: drop extra stations that do not improve on fewer (the reducer is not strictly monotone).
  for (size_t i = 0; i < w.size(); ++i) {
    Work& k = w[i];
    const Prep& p = k.prep();
    const double target = p.env[static_cast<size_t>(k.extra)];
    int e = k.extra;
    while (e > p.min_extra && p.red.errors[static_cast<size_t>(e)] > target + 1e-12) --e;
    while (e > p.min_extra && p.red.errors[static_cast<size_t>(e) - 1] <= target + 1e-12 &&
           p.red.errors[static_cast<size_t>(e)] > target + 1e-12)
      --e;
    k.extra = e;
    TubePlan plan;
    plan.sides = k.sides;
    plan.loops = k.loops;
    plan.extra = k.extra;
    plan.cap_start = k.cs;
    plan.cap_end = k.ce;
    std::set<int> st(p.red.initial.begin(), p.red.initial.end());
    for (int j = 0; j < k.extra; ++j) st.insert(p.red.order[static_cast<size_t>(j)]);
    for (int f : p.fixed) st.erase(f);
    plan.stations.assign(st.begin(), st.end());
    plan.loop_s = p.loop_s;
    plan.loop_joint = p.loop_joint;
    plan.loop_index = p.loop_index;
    plan.rings = k.rings(k.extra);
    plan.triangles = k.tris();
    plan.profile_error = p.red.errors[static_cast<size_t>(k.extra)];
    plan.importance = k.importance;
    a.total += plan.triangles;
    a.tubes.push_back(std::move(plan));
  }
  if (s.tri_budget < kLowBudgetWarning)
    a.messages.push_back(Message{"lowpoly.low_budget",
                                 "a budget of " + std::to_string(s.tri_budget) +
                                     " triangles is the accepted minimum range: silhouettes and joints are coarse",
                                 Severity::Warning, {{"budget", s.tri_budget}}});
  if (sides_reduced)
    a.messages.push_back(Message{"lowpoly.sides_reduced", "some parts keep 4 sides: the budget does not allow 6",
                                 Severity::Warning, {}});
  if (loops_reduced)
    a.messages.push_back(Message{"lowpoly.joint_loops_reduced",
                                 "some joints have fewer than " + std::to_string(jreq) + " loops: the budget does not allow more",
                                 Severity::Warning, {{"requested", jreq}}});
  return a;
}

TubeShape shape_for(const Layout& l, const TubeSpec& t, const TubePlan& p, int H) {
  TubeShape sh;
  sh.sides = p.sides;
  sh.cap_start = p.cap_start;
  sh.cap_end = p.cap_end;
  const auto& d = t.dense;
  const int n = static_cast<int>(d.size());
  for (int idx : p.stations) {
    if ((idx == 0 && p.cap_start == Cap::Apex) || (idx == n - 1 && p.cap_end == Cap::Apex)) continue;
    LoftRing r = ring_from_station(d[static_cast<size_t>(idx)], H);
    r.bone_a = bone_at(l, t, r.s);
    sh.rings.push_back(r);
  }
  for (size_t i = 0; i < p.loop_s.size(); ++i) {
    const JointSpec& j = t.joints[static_cast<size_t>(p.loop_joint[i])];
    LoftRing r = ring_from_station(station_at(d, p.loop_s[i]), H);
    r.bone_a = j.parent_bone;
    r.bone_b = j.child_bone;
    r.w_b = static_cast<float>(p.loop_index[i] + 1) / static_cast<float>(p.loops + 1);
    r.joint_loop = true;
    sh.rings.push_back(r);
  }
  std::stable_sort(sh.rings.begin(), sh.rings.end(), [](const LoftRing& a, const LoftRing& b) { return a.s < b.s; });
  // Round parts: their ring count stays, their rings move to regular latitudes of the fitted ellipsoid.
  for (const RoundFit& f : t.round_fits) {
    if (!f.ok || !t.trunk) continue;
    // The top run of this part's non-loop rings whose width matches the ellipsoid there.
    std::vector<size_t> run;
    for (size_t i = sh.rings.size(); i-- > 0;) {
      const LoftRing& r = sh.rings[i];
      if (r.owner != f.part || r.joint_loop) break;
      const double sy = (r.c.y - f.y0) / f.ry;
      if (sy < -0.95) break;
      const double ew = f.rx * std::sqrt(std::max(0.0, 1 - sy * sy));
      if (sy < 0 && std::abs(r.a - ew) > 0.2 * f.rx) break;  // below the equator only while it still follows the ellipse
      run.push_back(i);
    }
    if (run.size() < 2) continue;
    std::reverse(run.begin(), run.end());
    const size_t below = run.front();  // the ring under the run stays (a neck loop or a loft ring), if any
    double lo = std::asin(-0.9);
    if (below > 0) lo = std::asin(std::clamp((sh.rings[below - 1].c.y - f.y0) / f.ry, -0.95, 0.95));
    const double hi = std::numbers::pi / 2;
    const int k = static_cast<int>(run.size());
    std::vector<double> lat;
    if (lo < -0.08 && k >= 3) {  // a ring on the equator, the rest evenly below and above it
      const int n_lo = std::clamp(static_cast<int>(std::lround((k - 1) * (-lo) / (hi - lo))), 1, k - 2);
      const int n_hi = k - 1 - n_lo;
      for (int j = 1; j <= n_lo; ++j) lat.push_back(lo * (1 - static_cast<double>(j) / (n_lo + 1)));
      lat.push_back(0);
      for (int j = 1; j <= n_hi; ++j) lat.push_back(hi * j / (n_hi + 1.0));
    } else {
      for (int j = 1; j <= k; ++j) lat.push_back(lo + (hi - lo) * j / (k + 1.0));
    }
    for (size_t j = 0; j < run.size(); ++j) {
      LoftRing& r = sh.rings[run[j]];
      const double phi = lat[j];
      r.c = {f.x0, f.y0 + f.ry * std::sin(phi), f.z0};
      r.n = {1, 0, 0};
      r.a = std::max(0.25, f.rx * std::cos(phi));
      r.b = r.a * (f.rz / f.rx);
      r.s = f.ry * (1 + std::sin(phi)) + (below > 0 ? sh.rings[below - 1].s : 0);
      r.lathe = true;
    }
    if (p.cap_end == Cap::Apex && run.back() == sh.rings.size() - 1) sh.apex_end = {f.x0, f.y0 + f.ry, f.z0};
  }
  untangle(sh.rings, sh.sides);
  sh.apex_start = {d.front().centre.x, H - d.front().centre.y, d.front().z_c};
  sh.apex_end = {d.back().centre.x, H - d.back().centre.y, d.back().z_c};
  return sh;
}

}  // namespace pixel3d::lowpoly
