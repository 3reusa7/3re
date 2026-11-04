#include "pixel3d/lowpoly/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <set>


namespace pixel3d::lowpoly {
namespace {

struct PartInfo {
  const Part* p = nullptr;
  Mask2D front;
  Mask2D shape;  // the tube's shape: `front` with the gaps between its pieces closed
  int area = 0;
  vec2d centroid;
  Mask2D::Box box;
};

Mask2D unite_masks(const Mask2D& a, const Mask2D& b) {
  if (a.width() == 0) return b;
  Mask2D out = a;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x)
      if (b.get(x, y)) out.set(x, y, true);
  return out;
}

Mask2D intersect_masks(const Mask2D& a, const Mask2D& b) {
  Mask2D out(a.width(), a.height());
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) out.set(x, y, a.get(x, y) && b.get_or_false(x, y));
  return out;
}

vec2d centroid_of(const Mask2D& m) {
  double n = 0, sx = 0, sy = 0;
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x)
      if (m.get(x, y)) n += 1, sx += x + 0.5, sy += y + 0.5;
  return n > 0 ? vec2d{sx / n, sy / n} : vec2d{};
}

// Pixels of `a` with a pixel of `b` in their 8-neighbourhood; their centroid.
int contact(const Mask2D& a, const Mask2D& b, vec2d* where) {
  int n = 0;
  double sx = 0, sy = 0;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) {
      if (!a.get(x, y)) continue;
      bool touch = false;
      for (int dy = -1; dy <= 1 && !touch; ++dy)
        for (int dx = -1; dx <= 1 && !touch; ++dx) touch = b.get_or_false(x + dx, y + dy);
      if (touch) ++n, sx += x + 0.5, sy += y + 0.5;
    }
  if (where && n) *where = {sx / n, sy / n};
  return n;
}

double mask_distance(const Mask2D& a, const Mask2D& b) {
  double best = std::numeric_limits<double>::infinity();
  std::vector<vec2d> pb;
  for (int y = 0; y < b.height(); ++y)
    for (int x = 0; x < b.width(); ++x)
      if (b.get(x, y)) pb.push_back({x + 0.5, y + 0.5});
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x)
      if (a.get(x, y))
        for (const vec2d& q : pb) best = std::min(best, length(q - vec2d{x + 0.5, y + 0.5}));
  return best;
}

int count_pieces(const Mask2D& m) {
  Mask2D seen(m.width(), m.height());
  int n = 0;
  std::vector<std::pair<int, int>> st;
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) {
      if (!m.get(x, y) || seen.get(x, y)) continue;
      ++n;
      st.push_back({x, y});
      seen.set(x, y, true);
      while (!st.empty()) {
        const auto [cx, cy] = st.back();
        st.pop_back();
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx)
            if (m.get_or_false(cx + dx, cy + dy) && !seen.get(cx + dx, cy + dy)) seen.set(cx + dx, cy + dy, true), st.push_back({cx + dx, cy + dy});
      }
    }
  return n;
}

// A part drawn in several pieces (a tunic crossed by a belt) is one tube: the subject pixels
// between its pieces along every row and column join its shape (the crossing part overlaps).
Mask2D close_gaps(const Mask2D& m, const Mask2D& subject) {
  if (count_pieces(m) <= 1) return m;
  Mask2D out = m;
  for (int y = 0; y < m.height(); ++y) {
    int a = -1, b = -1;
    for (int x = 0; x < m.width(); ++x)
      if (m.get(x, y)) b = x, a = a < 0 ? x : a;
    for (int x = a; a >= 0 && x <= b; ++x)
      if (subject.get_or_false(x, y)) out.set(x, y, true);
  }
  for (int x = 0; x < m.width(); ++x) {
    int a = -1, b = -1;
    for (int y = 0; y < m.height(); ++y)
      if (m.get(x, y)) b = y, a = a < 0 ? y : a;
    for (int y = a; a >= 0 && y <= b; ++y)
      if (subject.get_or_false(x, y)) out.set(x, y, true);
  }
  return out;
}

bool point_in(const Mask2D& m, vec2d p) {
  return m.get_or_false(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)));
}

const Station& nearest_station(const std::vector<Station>& st, double s) {
  size_t best = 0;
  for (size_t i = 1; i < st.size(); ++i)
    if (std::abs(st[i].s - s) < std::abs(st[best].s - s)) best = i;
  return st[best];
}

vec3d world_centre(const Station& s, int H) { return {s.centre.x, H - s.centre.y, s.z_c}; }

}  // namespace

double type_weight(PartType t) {
  switch (t) {
    case PartType::Head: return 1.6;
    case PartType::Torso: return 1.0;
    case PartType::Cloth: return 1.0;
    case PartType::Limb: return 0.8;
    case PartType::Accessory: return 0.6;
  }
  return 1.0;
}

double type_roundness(PartType t) {
  switch (t) {
    case PartType::Head: return 0.95;
    case PartType::Torso: return 0.8;
    case PartType::Cloth: return 0.7;
    case PartType::Limb: return 1.0;
    case PartType::Accessory: return 0.6;
  }
  return 1.0;
}

double type_min_depth_ratio(PartType t, bool elongated) {
  switch (t) {
    case PartType::Head: return 0.95;
    case PartType::Torso: return 0.7;
    case PartType::Cloth: return 0.7;
    case PartType::Limb: return 1.0;
    case PartType::Accessory: return elongated ? 1.0 : 0.3;
  }
  return 0.7;
}

double type_error_floor(PartType t) {
  switch (t) {
    case PartType::Head: return 0.35;
    case PartType::Torso: return 0.8;
    case PartType::Cloth: return 1.0;
    case PartType::Limb: return 0.4;
    case PartType::Accessory: return 0.35;
  }
  return 0.5;
}

int bone_at(const Layout& l, const TubeSpec& t, double s) {
  if (t.rigid_bone >= 0) return t.rigid_bone;
  if (s < 0 && t.branch && t.parent_tube >= 0) {
    const TubeSpec& p = l.tubes[static_cast<size_t>(t.parent_tube)];
    // The parent's bone where this branch is attached.
    double best = std::numeric_limits<double>::infinity(), ps = 0;
    for (const Station& st : p.dense)
      if (length(st.centre - t.attach) < best) best = length(st.centre - t.attach), ps = st.s;
    return bone_at(l, p, std::max(0.0, ps));
  }
  for (const Segment& g : t.segments)
    if (s >= g.s0 - 1e-9 && s <= g.s1 + 1e-9) return g.bone;
  if (!t.segments.empty()) return s < t.segments.front().s0 ? t.segments.front().bone : t.segments.back().bone;
  return -1;
}

Expected<Layout> build_layout(const Input& in) {
  Layout L;
  const Dims d = in.dims;
  // ---- subject masks (alpha, or supplied from outside) ----------------------
  for (const SourceView& v : in.views) {
    if (!v.aligned) continue;
    auto it = in.subject_masks.find(v.role);
    if (it != in.subject_masks.end() && it->second.mask.width() == v.aligned->width() &&
        it->second.mask.height() == v.aligned->height()) {
      L.subject[v.role] = it->second.mask;
      L.messages.push_back(Message{"lowpoly.subject_mask", "subject mask from " + it->second.provenance, Severity::Info,
                                   {{"view", to_string(v.role)}, {"provenance", it->second.provenance}}});
    } else {
      L.subject[v.role] = opaque_mask(*v.aligned, in.alpha_threshold);
    }
  }
  if (!L.subject.contains(ViewRole::Front)) return make_error("lowpoly.no_front", "low-poly studio needs a Front drawing");
  if (in.parts.empty())
    return make_error("lowpoly.no_parts", "no parts: suggest parts (lowpoly_suggest_parts) or paint them first");
  const Mask2D& subject = L.subject.at(ViewRole::Front);
  const int subject_px = std::max(1, subject.count());
  const bool has_side = L.subject.contains(ViewRole::Side), has_top = L.subject.contains(ViewRole::Top);

  std::map<int, PartInfo> info;
  for (const Part& p : in.parts) {
    auto it = p.masks.find(ViewRole::Front);
    if (it == p.masks.end()) continue;
    PartInfo pi;
    pi.p = &p;
    pi.front = intersect_masks(it->second, subject);
    pi.area = pi.front.count();
    if (pi.area == 0) {
      L.messages.push_back(Message{"lowpoly.empty_part", "part " + p.name + " has no Front pixels; skipped",
                                   Severity::Warning, {{"part", p.id}}});
      continue;
    }
    pi.centroid = centroid_of(pi.front);
    pi.box = pi.front.bbox();
    pi.shape = close_gaps(pi.front, subject);
    info.emplace(p.id, std::move(pi));
  }
  if (info.empty()) return make_error("lowpoly.no_parts", "no part has Front pixels");
  auto masks_of = [&](const std::vector<int>& ids, ViewRole r) {
    Mask2D m;
    for (int id : ids) {
      const Part& p = *info.at(id).p;
      auto it = p.masks.find(r);
      if (it == p.masks.end()) continue;
      const Mask2D clipped = intersect_masks(it->second, L.subject.at(r));
      m = m.width() ? unite_masks(m, clipped) : clipped;
    }
    return m;
  };

  // ---- trunk ---------------------------------------------------------------
  auto largest = [&](std::initializer_list<PartType> types, const std::set<int>& exclude) {
    int best = -1;
    for (const auto& [id, pi] : info)
      if (!exclude.contains(id) && std::find(types.begin(), types.end(), pi.p->type) != types.end() &&
          (best < 0 || pi.area > info.at(best).area))
        best = id;
    return best;
  };
  std::set<int> used;
  int body = largest({PartType::Torso, PartType::Cloth}, {});
  int head = largest({PartType::Head}, {});
  if (body < 0 && head < 0) body = largest({PartType::Limb, PartType::Accessory}, {});
  std::vector<int> trunk_parts;
  if (body >= 0) trunk_parts.push_back(body);
  if (head >= 0) {
    if (body < 0 || mask_distance(info.at(head).front, info.at(body).front) <= 3.0) trunk_parts.push_back(head);
    else head = -1;
  }
  for (int id : trunk_parts) used.insert(id);

  // ---- limb chains -----------------------------------------------------------
  std::vector<int> limbs;
  for (const auto& [id, pi] : info)
    if (!used.contains(id) && pi.p->type == PartType::Limb) limbs.push_back(id);
  std::map<int, std::vector<int>> limb_adj;
  for (int a : limbs)
    for (int b : limbs)
      if (a != b && contact(info.at(a).front, info.at(b).front, nullptr) > 0) limb_adj[a].push_back(b);
  // Parent candidates for an attachment: parts already placed (trunk first).
  auto best_parent_part = [&](int id, const std::set<int>& candidates, vec2d* where) {
    int best = -1, best_c = 0;
    for (int c : candidates) {
      vec2d w;
      const int n = contact(info.at(id).front, info.at(c).front, &w);
      if (n > best_c) best = c, best_c = n, *where = w;
    }
    if (best < 0) {  // nothing touches: the nearest part, attached at the nearest pixel
      double bd = std::numeric_limits<double>::infinity();
      for (int c : candidates) {
        const double dd = mask_distance(info.at(id).front, info.at(c).front);
        if (dd < bd) bd = dd, best = c;
      }
      if (best >= 0) {
        const Mask2D& a = info.at(id).front;
        const Mask2D& b = info.at(best).front;
        // Its pixels nearest to that part (within half a pixel of the nearest): their centroid.
        std::vector<std::pair<vec2d, double>> d_of;
        for (int y = 0; y < a.height(); ++y)
          for (int x = 0; x < a.width(); ++x) {
            if (!a.get(x, y)) continue;
            double dd = std::numeric_limits<double>::infinity();
            for (int qy = 0; qy < b.height(); ++qy)
              for (int qx = 0; qx < b.width(); ++qx)
                if (b.get(qx, qy)) dd = std::min(dd, std::hypot(qx - x, qy - y));
            d_of.push_back({{x + 0.5, y + 0.5}, dd});
          }
        vec2d sum{};
        int n = 0;
        for (const auto& [q, dd] : d_of)
          if (dd <= bd + 0.5) sum = sum + q, ++n;
        if (n) *where = sum * (1.0 / n);
      }
    }
    return best;
  };

  struct ChainPlan {
    std::vector<int> parts;
    int parent_part = -1;
    vec2d attach;
    PartType type = PartType::Limb;
  };
  // A limb's axis, oriented from the end where it hangs on `from` (its root) to its tip.
  // A limb lying against its parent along most of its length (an arm hanging by the body)
  // hangs from its upper end.
  struct LimbAxis {
    vec2d root, u;
    double len = 0;
  };
  auto limb_axis = [&](int id, const Mask2D& from) {
    const Mask2D& m = info.at(id).front;
    const Axis ax = principal_axis(m);
    double t0 = std::numeric_limits<double>::infinity(), t1 = -t0, c0 = t0, c1 = -t0, cs = 0;
    int cn = 0;
    for (int y = 0; y < m.height(); ++y)
      for (int x = 0; x < m.width(); ++x) {
        if (!m.get(x, y)) continue;
        const double t = dot(vec2d{x + 0.5, y + 0.5} - ax.centroid, ax.dir);
        t0 = std::min(t0, t), t1 = std::max(t1, t);
        bool touch = false;
        for (int dy = -1; dy <= 1 && !touch; ++dy)
          for (int dx = -1; dx <= 1 && !touch; ++dx) touch = from.get_or_false(x + dx, y + dy);
        if (touch) c0 = std::min(c0, t), c1 = std::max(c1, t), cs += t, ++cn;
      }
    const vec2d e0 = ax.centroid + ax.dir * t0, e1 = ax.centroid + ax.dir * t1;
    bool root_at_e0;
    if (cn > 0 && c1 - c0 > 0.5 * (t1 - t0)) root_at_e0 = e0.y < e1.y;  // lies against it: the upper end
    else if (cn > 0) root_at_e0 = std::abs(cs / cn - t0) <= std::abs(cs / cn - t1);
    else root_at_e0 = length(e0 - centroid_of(from)) <= length(e1 - centroid_of(from));
    LimbAxis la;
    la.root = root_at_e0 ? e0 : e1;
    la.u = root_at_e0 ? ax.dir : ax.dir * -1;
    la.len = t1 - t0;
    return la;
  };
  // `next` continues limb `cur` (hanging on `from`) end to end: it touches cur's tip across
  // most of the tip's width and lies beyond it. Limbs drawn side by side (legs with an ink
  // line between them) or touching only at a corner (an arm's hand against a leg) do not.
  auto continues = [&](int cur, const Mask2D& from, int next, double* score) {
    const LimbAxis la = limb_axis(cur, from);
    if (la.len < 1) return false;
    const Mask2D& m = info.at(cur).front;
    const Mask2D& n = info.at(next).front;
    vec2d cw;
    if (contact(m, n, &cw) == 0) return false;
    const double tc = dot(cw - la.root, la.u);
    if (tc < 0.6 * la.len || dot(info.at(next).centroid - la.root, la.u) <= tc) return false;
    // Across the axis: the share of the tip's width (its last rows) that `next` occupies just
    // beyond the tip. A part offset sideways (a leg below and beside an arm) covers little.
    const double band = std::max(1.5, 0.15 * la.len);
    const vec2d across{-la.u.y, la.u.x};
    std::set<long> tip, beyond;
    for (int y = 0; y < m.height(); ++y)
      for (int x = 0; x < m.width(); ++x) {
        const vec2d q{x + 0.5, y + 0.5};
        const double t = dot(q - la.root, la.u);
        const long a = std::lround(dot(q - la.root, across));
        if (m.get(x, y) && t >= la.len - band) tip.insert(a);
        if (n.get(x, y) && t >= la.len - 1 && t <= la.len + band) beyond.insert(a);
      }
    int shared = 0;
    for (long a : tip) shared += beyond.contains(a);
    *score = tip.empty() ? 0 : static_cast<double>(shared) / tip.size();
    return *score >= 0.5;
  };

  std::vector<ChainPlan> chains;
  std::set<int> limb_done;
  // Start chains at limbs touching the trunk, then at limbs touching any other non-limb part
  // (legs under a tunic hang from it), then at the rest (the nearest placed part).
  std::set<int> anchors(trunk_parts.begin(), trunk_parts.end());
  std::set<int> bodies = anchors;
  for (const auto& [id, pi] : info)
    if (pi.p->type != PartType::Limb && pi.p->type != PartType::Accessory) bodies.insert(id);
  for (int pass = 0; pass < 3; ++pass)
    for (int a : limbs) {
      if (limb_done.contains(a)) continue;
      vec2d w;
      int parent = -1;
      if (pass < 2) {
        const std::set<int>& cand = pass == 0 ? anchors : bodies;
        for (int c : cand)
          if (contact(info.at(a).front, info.at(c).front, nullptr) > 0) parent = best_parent_part(a, cand, &w);
        if (parent < 0) continue;
      } else {
        std::set<int> placed = bodies;
        for (int x : limb_done) placed.insert(x);
        parent = best_parent_part(a, placed, &w);
      }
      ChainPlan c{{a}, parent, w, PartType::Limb};
      limb_done.insert(a);
      Mask2D from = parent >= 0 ? info.at(parent).front : Mask2D(subject.width(), subject.height());
      for (int cur = a;;) {  // follow the limb path away from the parent, end to end only
        int next = -1;
        double best = 0;
        for (int b : limb_adj[cur]) {
          double sc = 0;
          if (!limb_done.contains(b) && continues(cur, from, b, &sc) && sc > best) next = b, best = sc;
        }
        if (next < 0) break;
        c.parts.push_back(next);
        limb_done.insert(next);
        from = info.at(cur).front;
        cur = next;
      }
      chains.push_back(c);
    }
  for (const auto& c : chains)
    for (int id : c.parts) used.insert(id);

  // ---- remaining parts: branches -------------------------------------------
  std::vector<ChainPlan> branches;
  // Body parts (torso, cloth, head) first, outward from the trunk: each hangs on a body part
  // already placed that it touches, so limbs hang from bodies and never the other way round
  // (and two bodies never hang on each other).
  std::set<int> body_placed(trunk_parts.begin(), trunk_parts.end());
  for (bool progress = true; progress;) {
    progress = false;
    for (const auto& [id, pi] : info) {
      if (used.contains(id) || !bodies.contains(id)) continue;
      std::set<int> touching;
      for (int c : body_placed)
        if (contact(pi.front, info.at(c).front, nullptr) > 0) touching.insert(c);
      if (touching.empty()) continue;
      vec2d w;
      const int parent = best_parent_part(id, touching, &w);
      branches.push_back({{id}, parent, w, pi.p->type});
      used.insert(id);
      body_placed.insert(id);
      progress = true;
    }
  }
  for (const auto& [id, pi] : info) {
    if (used.contains(id)) continue;
    std::set<int> placed;
    for (const auto& [oid, opi] : info)
      if (oid != id && (used.contains(oid) || opi.p->type != PartType::Accessory)) placed.insert(oid);
    // A body part touching no placed body part (a hood above a scarf) hangs on the nearest
    // body part, not on a limb it happens to touch at a corner.
    if (bodies.contains(id)) {
      std::set<int> near_bodies;
      for (int c : body_placed)
        if (c != id) near_bodies.insert(c);
      if (!near_bodies.empty()) placed = near_bodies;
    }
    vec2d w;
    const int parent = best_parent_part(id, placed, &w);
    branches.push_back({{id}, parent, w, pi.p->type});
  }
  for (const auto& b : branches) used.insert(b.parts.front());

  // ---- template ------------------------------------------------------------
  const vec2d trunk_c = trunk_parts.empty() ? centroid_of(subject) : info.at(trunk_parts.front()).centroid;
  const Mask2D::Box body_box = trunk_parts.empty() ? subject.bbox() : info.at(trunk_parts.front()).box;
  int upper_l = 0, upper_r = 0, lower_l = 0, lower_r = 0;
  std::vector<int> chain_role(chains.size(), 0);  // 1 upper, 2 lower
  for (size_t i = 0; i < chains.size(); ++i) {
    const auto& c = chains[i];
    if (std::find(trunk_parts.begin(), trunk_parts.end(), c.parent_part) == trunk_parts.end()) continue;
    Mask2D m;
    for (int id : c.parts) m = unite_masks(m, info.at(id).front);
    const vec2d cc = centroid_of(m);
    const bool lower = cc.y > body_box.y1 - 0.2 * body_box.height();
    const bool left = c.attach.x > trunk_c.x;  // image right = the character's left
    chain_role[i] = lower ? 2 : 1;
    (lower ? (left ? lower_l : lower_r) : (left ? upper_l : upper_r))++;
  }
  bool widening = false;
  if (body >= 0) {
    const Mask2D& bm = info.at(body).front;
    const auto bx = bm.bbox();
    auto row_w = [&](int y) {
      int x0 = bm.width(), x1 = -1;
      for (int x = 0; x < bm.width(); ++x)
        if (bm.get(x, y)) x0 = std::min(x0, x), x1 = std::max(x1, x);
      return x1 >= x0 ? x1 - x0 + 1 : 0;
    };
    widening = row_w(bx.y1 - 1 - bx.height() / 8) > 1.3 * row_w(bx.y0 + bx.height() / 8);
  }
  const bool cloth_trunk = body >= 0 && (info.at(body).p->type == PartType::Cloth || widening);
  const bool fits_humanoid = head >= 0 && body >= 0 && upper_l == 1 && upper_r == 1 && lower_l == 1 && lower_r == 1;
  const bool fits_cloaked = head >= 0 && body >= 0 && upper_l == 0 && upper_r == 0 && lower_l == 1 && lower_r == 1 && cloth_trunk;
  Template templ = Template::GenericChain;
  if (in.settings.templ == Template::Auto) {
    templ = fits_humanoid ? Template::Humanoid : fits_cloaked ? Template::CloakedCharacter : Template::GenericChain;
  } else if ((in.settings.templ == Template::Humanoid && fits_humanoid) ||
             (in.settings.templ == Template::CloakedCharacter && fits_cloaked) ||
             in.settings.templ == Template::GenericChain) {
    templ = in.settings.templ;
  } else {
    L.messages.push_back(Message{"lowpoly.template_mismatch",
                                 std::string("the parts do not fit the ") + std::string(to_string(in.settings.templ)) +
                                     " template; using generic_chain",
                                 Severity::Warning, {{"requested", to_string(in.settings.templ)}}});
  }
  L.templ = templ;
  const bool named = templ != Template::GenericChain;

  // ---- tubes: spines and profiles ------------------------------------------
  auto make_tube = [&](const std::vector<int>& parts, bool trunk, bool straight_hint, int parent_tube, vec2d attach) {
    TubeSpec t;
    t.parts = parts;
    t.trunk = trunk;
    t.type = info.at(parts.front()).p->type;
    if (trunk && head >= 0 && parts.size() == 1 && parts.front() == head) t.type = PartType::Head;
    t.parent_tube = parent_tube;
    t.branch = parent_tube >= 0;
    t.chain = parts.size() > 1 || (t.type == PartType::Limb && t.branch);
    t.attach = attach;
    Mask2D m;
    int area = 0;
    double tw = 0;
    for (int id : parts) {
      m = unite_masks(m, info.at(id).shape);
      area += info.at(id).area;
      tw += info.at(id).area * type_weight(info.at(id).p->type);
    }
    t.area_share = static_cast<double>(area) / subject_px;
    t.type_weight = area ? tw / area : 1;
    const Axis ax = principal_axis(m);
    const bool blob = ax.elongation < 1.6;
    if (trunk) {
      vec2d dir{0, -1};
      if (!blob && std::abs(ax.dir.y) < std::cos(30 * std::numbers::pi / 180)) dir = ax.dir;
      t.spine = straight_spine(m, dir);
    } else if (blob || straight_hint) {
      vec2d dir = ax.dir;
      // A round branch runs away from where it hangs (its own axis is arbitrary).
      if (t.branch && blob && length(ax.centroid - attach) > 1) dir = (ax.centroid - attach) * (1 / length(ax.centroid - attach));
      if (t.branch && dot(attach - ax.centroid, dir) > 0) dir = dir * -1;  // run away from the parent
      t.spine = straight_spine(m, dir);
    } else {
      t.spine = medial_spine(m, t.branch ? attach : vec2d{std::nan(""), std::nan("")});
    }
    // Trim the very ends of straight spines (the line would run along a pixel edge).
    const Polygons2D outline = part_outline(m);
    t.dense = measure_widths(t.spine, outline, t.spine.straight);
    // A spine may overshoot the outline by up to a step: keep only one zero-width station at each end.
    auto trim = [&](bool back) {
      while (t.dense.size() > 3) {
        const size_t i = back ? t.dense.size() - 2 : 1;
        if (t.dense[i].half_w > 0.05) break;
        t.dense.erase(t.dense.begin() + static_cast<long>(i));
        t.spine.pts.erase(t.spine.pts.begin() + static_cast<long>(i));
        t.spine.normals.erase(t.spine.normals.begin() + static_cast<long>(i));
      }
    };
    trim(true);
    trim(false);
    // Branches: drop the stations lying on the contact edge, then extend into the parent. A
    // part drawn entirely on other parts (a face in a hood, a pouch on a belt) already lies on
    // its parent: no extension (along its own spine it would leave the parent sideways).
    bool enclosed = true;
    for (int y = 0; y < m.height() && enclosed; ++y)
      for (int x = 0; x < m.width() && enclosed; ++x)
        if (m.get(x, y))
          for (const auto& [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
            if (!subject.get_or_false(x + dx, y + dy)) enclosed = false;
    if (t.branch && t.dense.size() > 4 && !enclosed) {
      size_t k = 0;
      double maxw = 0;
      for (const auto& s : t.dense) maxw = std::max(maxw, s.half_w);
      while (k + 2 < t.dense.size() && t.dense[k].half_w < 0.35 * maxw && t.dense[k].s < 1.5) ++k;
      if (k > 0) {
        t.dense.erase(t.dense.begin(), t.dense.begin() + static_cast<long>(k));
        t.spine.pts.erase(t.spine.pts.begin(), t.spine.pts.begin() + static_cast<long>(k));
        t.spine.normals.erase(t.spine.normals.begin(), t.spine.normals.begin() + static_cast<long>(k));
      }
      const double s0 = t.dense.front().s;
      for (auto& s : t.dense) s.s -= s0;
      t.extension = std::clamp(t.dense.front().half_w, 1.0, 3.0);
      const vec2d tan = t.spine.tangent(0);
      std::vector<Station> ext;
      for (double e = t.extension; e > 1e-9; e -= 0.5) {
        Station s = t.dense.front();
        s.s = -e;
        s.centre = t.dense.front().centre - tan * e;
        ext.push_back(s);
      }
      t.dense.insert(t.dense.begin(), ext.begin(), ext.end());
    }
    // Owners: the part containing the station centre (neighbours fill the gaps).
    for (auto& s : t.dense) {
      s.owner = -1;
      for (int id : parts)
        if (point_in(info.at(id).front, s.centre)) s.owner = id;
    }
    for (size_t i = 1; i < t.dense.size(); ++i)
      if (t.dense[i].owner < 0) t.dense[i].owner = t.dense[i - 1].owner;
    for (size_t i = t.dense.size(); i-- > 1;)
      if (t.dense[i - 1].owner < 0) t.dense[i - 1].owner = t.dense[i].owner;
    for (auto& s : t.dense)
      if (s.owner < 0) s.owner = parts.front();
    // A chain keeps its parts in spine order.
    if (parts.size() > 1) {
      std::vector<int> order;
      for (const auto& s : t.dense)
        if (s.s >= 0 && std::find(order.begin(), order.end(), s.owner) == order.end()) order.push_back(s.owner);
      for (int id : parts)
        if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
      t.parts = order;
    }
    std::string nm;
    for (int id : t.parts) nm += (nm.empty() ? "" : "+") + info.at(id).p->name;
    t.name = nm;
    return t;
  };

  if (!trunk_parts.empty()) L.tubes.push_back(make_tube(trunk_parts, true, false, -1, {}));
  auto tube_of_part = [&](int part) {
    for (size_t i = 0; i < L.tubes.size(); ++i)
      if (std::find(L.tubes[i].parts.begin(), L.tubes[i].parts.end(), part) != L.tubes[i].parts.end())
        return static_cast<int>(i);
    return 0;
  };
  std::vector<int> chain_tube(chains.size(), -1);
  std::vector<bool> branch_done(branches.size(), false);
  // A tube is made once the tube of its parent part exists (a chain may hang from another
  // chain or from a branch such as a tunic); chains first, then branches, in rounds.
  auto parent_ready = [&](int part) {
    for (const auto& t : L.tubes)
      if (std::find(t.parts.begin(), t.parts.end(), part) != t.parts.end()) return true;
    return part < 0;
  };
  const int rounds = static_cast<int>(chains.size() + branches.size()) + 1;
  for (int round = 0; round <= rounds; ++round) {
    const bool force = round == rounds;  // whatever is left (a parent cycle): attach as before
    for (size_t i = 0; i < chains.size(); ++i) {
      if (chain_tube[i] >= 0 || (!force && !parent_ready(chains[i].parent_part))) continue;
      chain_tube[i] = static_cast<int>(L.tubes.size());
      L.tubes.push_back(make_tube(chains[i].parts, false, false, tube_of_part(chains[i].parent_part), chains[i].attach));
    }
    for (size_t i = 0; i < branches.size(); ++i) {
      if (branch_done[i] || (!force && !parent_ready(branches[i].parent_part))) continue;
      branch_done[i] = true;
      L.tubes.push_back(make_tube(branches[i].parts, false, false, tube_of_part(branches[i].parent_part), branches[i].attach));
    }
  }

  // ---- depth ---------------------------------------------------------------
  for (auto& t : L.tubes) {
    DepthOptions o;
    o.box_depth = d.D;
    double tr = 0;
    int area = 0;
    for (int id : t.parts) {
      tr += info.at(id).area * type_roundness(info.at(id).p->type);
      area += info.at(id).area;
    }
    o.roundness = area ? tr / area : 1;
    // Shape per station from its own part: a depth_ratio override is the part's depth/width
    // (roundness and minimum); otherwise the type's roundness and depth/width minimum.
    std::map<int, std::pair<double, double>> shape;  // part → (roundness, min ratio)
    for (int id : t.parts) {
      const Part& p = *info.at(id).p;
      const bool elongated = principal_axis(info.at(id).front).elongation >= 1.6;
      shape[id] = p.depth_ratio ? std::pair{*p.depth_ratio, *p.depth_ratio}
                                : std::pair{type_roundness(p.type), type_min_depth_ratio(p.type, elongated)};
    }
    for (const Station& st : t.dense) {
      const auto it = shape.find(st.owner);
      const auto sp = it != shape.end() ? it->second : shape.begin()->second;
      o.roundness_per_station.push_back(sp.first);
      o.min_ratio_per_station.push_back(sp.second);
    }
    o.depth_limit = in.settings.depth;
    for (int id : t.parts)
      if (info.at(id).p->depth) o.depth_limit = *info.at(id).p->depth;
    auto cue_it = in.depth_cues.find(t.parts.front());
    if (cue_it != in.depth_cues.end() && cue_it->second.view == ViewRole::Front) o.cue = &cue_it->second;
    const Mask2D side = has_side ? masks_of(t.parts, ViewRole::Side) : Mask2D{};
    const Mask2D top = has_top ? masks_of(t.parts, ViewRole::Top) : Mask2D{};
    if (side.width() && side.count()) o.side = &side;
    else if (top.width() && top.count()) o.top = &top;
    Mask2D m;
    for (int id : t.parts) m = unite_masks(m, info.at(id).shape);
    t.depth_source = measure_depth(t.dense, t.spine, distance_transform(m), o);
  }
  // Partners: symmetric tubes (mirror of each other about the trunk centre line).
  for (size_t i = 0; i < L.tubes.size(); ++i)
    for (size_t j = 0; j < L.tubes.size(); ++j) {
      if (i == j || L.tubes[i].trunk || L.tubes[j].trunk || L.tubes[i].parts.size() != L.tubes[j].parts.size()) continue;
      const auto& a = L.tubes[i];
      const auto& b = L.tubes[j];
      if (a.type != b.type || a.parent_tube != b.parent_tube) continue;
      const double mirror = std::abs((a.attach.x - trunk_c.x) + (b.attach.x - trunk_c.x));
      if (mirror < 0.15 * d.W && std::abs(a.attach.y - b.attach.y) < 0.1 * d.H &&
          (L.tubes[i].partner < 0 || L.tubes[j].partner < 0))
        L.tubes[i].partner = static_cast<int>(j), L.tubes[j].partner = static_cast<int>(i);
    }
  for (auto& t : L.tubes) {
    if ((has_side || has_top) && t.depth_source == "inflation" && t.partner >= 0) {
      const TubeSpec& p = L.tubes[static_cast<size_t>(t.partner)];
      if (p.depth_source == "inflation") continue;
      // Same station positions along the spine: copy the partner's depth (a mirror image has the same depth).
      for (auto& s : t.dense) {
        const Station q = station_at(p.dense, s.s);
        s.z_c = q.z_c;
        s.half_d = q.half_d;
        s.measured = false;
      }
      t.depth_source = "partner";
    }
  }
  // Symmetric partners whose drawings mirror each other: model one side and mirror it
  // (a low-poly artist's habit), so both sides get the same rings. The leader is the
  // side with measured depth.
  auto rank = [](const TubeSpec& t) {
    return (t.depth_source.rfind("side", 0) == 0 || t.depth_source.rfind("top", 0) == 0) ? 2 : t.depth_source == "partner" ? 1 : 0;
  };
  for (size_t i = 0; i < L.tubes.size(); ++i) {
    if (L.tubes[i].partner <= static_cast<int>(i)) continue;
    TubeSpec& a = L.tubes[i];
    TubeSpec& b = L.tubes[static_cast<size_t>(a.partner)];
    if (b.partner != static_cast<int>(i) || a.parts.size() != b.parts.size()) continue;
    Mask2D ma, mb;
    for (int id : a.parts) ma = unite_masks(ma, info.at(id).front);
    for (int id : b.parts) mb = unite_masks(mb, info.at(id).front);
    const double axis = (a.attach.x + b.attach.x) / 2;
    Mask2D mir(ma.width(), ma.height());
    for (int y = 0; y < ma.height(); ++y)
      for (int x = 0; x < ma.width(); ++x)
        if (ma.get(x, y)) {
          const int xm = static_cast<int>(std::lround(2 * axis - (x + 0.5) - 0.5));
          if (xm >= 0 && xm < mir.width()) mir.set(xm, y, true);
        }
    const double iou = mask_iou(mir, mb);
    if (iou < 0.85) continue;
    const bool b_leads = rank(b) > rank(a);
    TubeSpec& lead = b_leads ? b : a;
    TubeSpec& fol = b_leads ? a : b;
    std::map<int, int> part_map;
    for (size_t k = 0; k < lead.parts.size(); ++k) part_map[lead.parts[k]] = fol.parts[k];
    fol.dense = lead.dense;
    for (auto& s : fol.dense) {
      s.centre.x = 2 * axis - s.centre.x;
      s.n.x = -s.n.x;
      s.owner = part_map[s.owner];
    }
    fol.spine = lead.spine;
    for (auto& p : fol.spine.pts) p.x = 2 * axis - p.x;
    for (auto& n : fol.spine.normals) n.x = -n.x;
    fol.extension = lead.extension;
    fol.attach = {2 * axis - lead.attach.x, lead.attach.y};
    fol.depth_source = "mirror:" + lead.depth_source;
    L.messages.push_back(Message{"lowpoly.mirrored", fol.name + " is modelled as the mirror image of " + lead.name,
                                 Severity::Info, {{"leader", lead.name}, {"follower", fol.name}, {"iou", iou}}});
  }

  // ---- inferred depth: parts drawn ON another part come forward -------------------
  // Without a Side/Top every part is inflated about the same centre, so a part drawn on a
  // bigger one (a face inside a hood, a belt on a tunic) ends up inside the bigger shell and
  // its drawing is hidden. A tube whose depth is inferred is moved toward the Front camera
  // until its surface is in front of every other tube's over most of its own drawn pixels.
  {
    const int W = subject.width(), Hm = subject.height();
    const double ninf = -std::numeric_limits<double>::infinity();
    // Front-most z of a tube's elliptic cross-sections per Front pixel (-inf = not covered);
    // `low`: the lowest front over the pixel instead (a cautious estimate of what hides).
    auto front_map = [&](const TubeSpec& t, bool low = false) {
      std::vector<double> z(static_cast<size_t>(W) * Hm, ninf);
      std::vector<double> lo(low ? z.size() : 0, std::numeric_limits<double>::infinity());
      for (const Station& st : t.dense) {
        if (st.s < 0 || st.half_w <= 0) continue;
        for (double a = -st.half_w; a <= st.half_w + 1e-9; a += 0.25) {
          const vec2d q = st.centre + st.n * a;
          const int x = static_cast<int>(std::floor(q.x)), y = static_cast<int>(std::floor(q.y));
          if (x < 0 || y < 0 || x >= W || y >= Hm) continue;
          const double r = a / st.half_w;
          const double f = st.z_c + st.half_d * std::sqrt(std::max(0.0, 1 - r * r));
          double& o = z[static_cast<size_t>(y) * W + x];
          o = std::max(o, f);
          if (low) lo[static_cast<size_t>(y) * W + x] = std::min(lo[static_cast<size_t>(y) * W + x], f);
        }
      }
      if (low)
        for (size_t k = 0; k < z.size(); ++k) z[k] = z[k] == ninf ? ninf : lo[k];
      return z;
    };
    auto inferred = [](const TubeSpec& t) {
      return std::none_of(t.dense.begin(), t.dense.end(), [](const Station& s) { return s.measured; });
    };
    const double margin = 0.35;  // source pixels in front of the covering surface
    for (int pass = 0; pass < 3; ++pass) {
      bool moved = false;
      std::vector<std::vector<double>> maps;
      for (const auto& t : L.tubes) maps.push_back(front_map(t));
      for (size_t i = 0; i < L.tubes.size(); ++i) {
        TubeSpec& t = L.tubes[i];
        if (t.trunk || !inferred(t)) continue;
        std::vector<double> need;
        int own = 0;
        for (int id : t.parts) {
          const Mask2D& m = info.at(id).front;
          for (int y = 0; y < m.height(); ++y)
            for (int x = 0; x < m.width(); ++x) {
              if (!m.get(x, y)) continue;
              ++own;
              const size_t k = static_cast<size_t>(y) * W + x;
              if (maps[i][k] == ninf) continue;
              double cover = ninf;
              for (size_t j = 0; j < maps.size(); ++j)
                if (j != i) cover = std::max(cover, maps[j][k]);
              if (cover != ninf && cover + margin > maps[i][k]) need.push_back(cover + margin - maps[i][k]);
            }
        }
        // Only a part mostly hidden by another one moves; by the shift that shows 3/4 of
        // its hidden pixels (its rim, where its own surface curves away, may stay behind).
        if (own == 0 || need.size() < 0.25 * own) continue;
        std::sort(need.begin(), need.end());
        const double dz = need[std::min(need.size() - 1, static_cast<size_t>(0.75 * need.size()))];
        if (dz <= 1e-6) continue;
        for (Station& st : t.dense) st.z_c += dz;
        maps[i] = front_map(t);
        moved = true;
        L.messages.push_back(Message{"lowpoly.brought_forward",
                                     t.name + " is drawn on another part: brought forward by " +
                                         std::to_string(dz).substr(0, 4) + " px (inferred depth)",
                                     Severity::Info, {{"tube", t.name}, {"dz", dz}}});
      }
      if (!moved) break;
    }
    // A part bridged over another part's pixels (a torso through the belt's rows, see
    // close_gaps) stays behind the parts drawn there: its stations whose centre is not on its
    // own drawing are thinned until their front is behind the other tubes' fronts.
    std::vector<std::vector<double>> maps;
    for (const auto& t : L.tubes) maps.push_back(front_map(t, true));
    for (size_t i = 0; i < L.tubes.size(); ++i) {
      TubeSpec& t = L.tubes[i];
      bool thinned = false;
      for (Station& st : t.dense) {
        bool own = false;
        for (int id : t.parts) own |= point_in(info.at(id).front, st.centre);
        if (own) continue;
        const int x = static_cast<int>(std::floor(st.centre.x)), y = static_cast<int>(std::floor(st.centre.y));
        if (x < 0 || y < 0 || x >= W || y >= Hm) continue;
        bool bridged = false;  // on the subject, drawn by another part
        for (int id : t.parts) bridged |= info.at(id).shape.get_or_false(x, y);
        if (!bridged) continue;
        // Across its whole cross line: half depth such that its front is behind the others'.
        double limit = st.half_d;
        for (double a = -st.half_w; a <= st.half_w + 1e-9; a += 0.25) {
          const vec2d q = st.centre + st.n * a;
          const int qx = static_cast<int>(std::floor(q.x)), qy = static_cast<int>(std::floor(q.y));
          if (qx < 0 || qy < 0 || qx >= W || qy >= Hm) continue;
          double other = ninf;
          for (size_t j = 0; j < maps.size(); ++j)
            if (j != i) other = std::max(other, maps[j][static_cast<size_t>(qy) * W + qx]);
          if (other == ninf) continue;
          const double r = st.half_w > 0 ? a / st.half_w : 0, c = std::sqrt(std::max(1e-3, 1 - r * r));
          limit = std::min(limit, (other - margin - st.z_c) / c);
        }
        if (limit >= st.half_d) continue;
        // Only its front moves back; its back stays (the Side silhouette is kept).
        const double back = st.z_c - st.half_d, front = std::max(back + 0.5, st.z_c + std::max(0.25, limit));
        st.z_c = (front + back) / 2;
        st.half_d = (front - back) / 2;
        thinned = true;
      }
      if (thinned)
        L.messages.push_back(Message{"lowpoly.bridged_behind", t.name + " runs behind the parts drawn across it",
                                     Severity::Info, {{"tube", t.name}}});
    }
  }

  for (auto& t : L.tubes) {
    t.error_floor.clear();
    for (const Station& st : t.dense)
      t.error_floor.push_back(type_error_floor(info.count(st.owner) ? info.at(st.owner).p->type : t.type));
  }
  // ---- round parts: one ellipsoid per head ---------------------------------------
  for (auto& t : L.tubes)
    for (int id : t.parts) {
      if (info.at(id).p->type != PartType::Head) continue;
      const Mask2D& m = info.at(id).front;
      const auto bx = m.bbox();
      // Widest row = the equator; the top of the mask = the pole.
      int best_w = 0;
      std::vector<int> rows;
      std::vector<std::pair<int, int>> ext(static_cast<size_t>(m.height()), {m.width(), -1});
      for (int y = bx.y0; y < bx.y1; ++y) {
        for (int x = bx.x0; x < bx.x1; ++x)
          if (m.get(x, y)) ext[static_cast<size_t>(y)] = {std::min(ext[static_cast<size_t>(y)].first, x), std::max(ext[static_cast<size_t>(y)].second, x + 1)};
        const int w = ext[static_cast<size_t>(y)].second - ext[static_cast<size_t>(y)].first;
        if (w > best_w) best_w = w, rows.clear();
        if (w == best_w) rows.push_back(y);
      }
      if (rows.empty()) continue;
      RoundFit f;
      f.part = id;
      const double eq_v = (rows.front() + rows.back() + 1) / 2.0;  // image v of the equator
      const auto e = ext[static_cast<size_t>(rows[rows.size() / 2])];
      f.x0 = (e.first + e.second) / 2.0;
      f.rx = best_w / 2.0;
      f.ry = eq_v - bx.y0;
      f.y0 = d.H - eq_v;
      // Depth: the head's own stations near the equator (measured or inferred).
      std::vector<double> hd, zc;
      for (const Station& st : t.dense)
        if (st.owner == id && std::abs(st.centre.y - eq_v) <= std::max(1.0, 0.25 * f.ry)) hd.push_back(st.half_d), zc.push_back(st.z_c);
      if (hd.empty() || f.rx <= 0 || f.ry <= 0) continue;
      std::sort(hd.begin(), hd.end());
      std::sort(zc.begin(), zc.end());
      f.rz = hd[hd.size() / 2];
      f.z0 = zc[zc.size() / 2];
      // How ellipse-like is the mask above the lowest row the ellipse reaches?
      int inter = 0, uni = 0;
      for (int y = bx.y0; y < bx.y1; ++y)
        for (int x = bx.x0 - 1; x <= bx.x1; ++x) {
          const double qx = (x + 0.5 - f.x0) / f.rx, qy = (y + 0.5 - eq_v) / f.ry;
          const bool in_e = qx * qx + qy * qy <= 1.0;
          const bool in_m = m.get_or_false(x, y);
          if (y + 0.5 > eq_v + f.ry) continue;  // below the ellipse (a neck): not part of the fit
          inter += in_e && in_m;
          uni += in_e || in_m;
        }
      f.iou = uni ? static_cast<double>(inter) / uni : 0;
      f.ok = f.iou >= 0.88;
      L.messages.push_back(Message{f.ok ? "lowpoly.round_part" : "lowpoly.round_part_rejected",
                                   info.at(id).p->name + (f.ok ? " is built as a regular ellipsoid" : " is not ellipse-like; built as a loft") +
                                       " (IoU " + std::to_string(f.iou).substr(0, 5) + ")",
                                   Severity::Info, {{"part", id}, {"iou", f.iou}}});
      t.round_fits.push_back(f);
    }

  // ---- skeleton --------------------------------------------------------------
  Skeleton& sk = L.skeleton;
  auto add_bone = [&](std::string name, int parent, vec3d h, vec3d tail, int part) {
    sk.bones.push_back({std::move(name), parent, h, tail, part});
    return static_cast<int>(sk.bones.size() - 1);
  };
  const int H = d.H;
  auto pname = [&](int id) { return info.at(id).p->name; };
  if (!L.tubes.empty() && L.tubes[0].trunk) {
    TubeSpec& t = L.tubes[0];
    const Station& bottom = t.dense.front();
    const Station& top_st = t.dense.back();
    // Segments by owner along the spine; joints where the owner changes.
    std::vector<Segment> segs;
    for (const auto& s : t.dense) {
      if (segs.empty() || segs.back().part != s.owner) segs.push_back({s.owner, -1, s.s, s.s});
      segs.back().s1 = s.s;
    }
    // Merge flicker: a segment shorter than 2 px joins its predecessor.
    std::vector<Segment> merged;
    for (const auto& g : segs) {
      if (!merged.empty() && (g.s1 - g.s0 < 2 || merged.back().part == g.part)) merged.back().s1 = g.s1;
      else merged.push_back(g);
    }
    t.segments = merged;
    const int root = add_bone(named ? "Hips" : "Root", -1, world_centre(bottom, H), world_centre(nearest_station(t.dense, bottom.s + 1), H), t.segments.front().part);
    int prev = root;
    for (size_t k = 0; k < t.segments.size(); ++k) {
      Segment& g = t.segments[k];
      const bool is_head = info.at(g.part).p->type == PartType::Head;
      std::string name = named ? (is_head ? "Head" : (k == 0 ? "Spine" : "Chest")) : pname(g.part);
      const double s_head = k == 0 ? bottom.s + 0.25 * (g.s1 - g.s0) : g.s0;
      const vec3d hpos = world_centre(nearest_station(t.dense, s_head), H);
      const vec3d tpos = world_centre(nearest_station(t.dense, k + 1 < t.segments.size() ? t.segments[k + 1].s0 : top_st.s), H);
      g.bone = add_bone(name, prev, hpos, tpos, g.part);
      if (k > 0) {
        const Station& js = nearest_station(t.dense, g.s0);
        t.joints.push_back({g.s0, prev, g.bone, false, false, 2 * js.half_w});
      }
      prev = g.bone;
    }
  }
  for (size_t ti = 1; ti < L.tubes.size(); ++ti) {
    TubeSpec& t = L.tubes[ti];
    const int parent_bone = bone_at(L, t, -0.25);
    if (!t.chain) {
      t.rigid_bone = parent_bone;
      t.segments = {{t.parts.front(), parent_bone, t.dense.front().s, t.dense.back().s}};
      continue;
    }
    // Segments by owner (s ≥ 0).
    std::vector<Segment> segs;
    for (const auto& s : t.dense) {
      if (s.s < 0) continue;
      if (segs.empty() || segs.back().part != s.owner) segs.push_back({s.owner, -1, s.s, s.s});
      segs.back().s1 = s.s;
    }
    std::vector<Segment> merged;
    for (const auto& g : segs) {
      if (!merged.empty() && (g.s1 - g.s0 < 2 || merged.back().part == g.part)) merged.back().s1 = g.s1;
      else merged.push_back(g);
    }
    segs = merged;
    std::vector<bool> inferred(segs.size(), false);
    // Templates want two bones per limb: split a one-part limb at its narrowest point.
    if (named && segs.size() == 1) {
      const double len = segs[0].s1 - segs[0].s0;
      double best_s = segs[0].s0 + 0.5 * len, best_w = std::numeric_limits<double>::infinity(), wmin = best_w, wmax = 0;
      for (const auto& s : t.dense)
        if (s.s >= segs[0].s0 + 0.35 * len && s.s <= segs[0].s0 + 0.65 * len) {
          wmin = std::min(wmin, s.half_w);
          wmax = std::max(wmax, s.half_w);
          if (s.half_w < best_w) best_w = s.half_w, best_s = s.s;
        }
      if (wmax - wmin < 0.08 * wmax) best_s = segs[0].s0 + 0.5 * len;  // no clear narrowing: the middle
      Segment second{segs[0].part, -1, best_s, segs[0].s1};
      segs[0].s1 = best_s;
      segs.push_back(second);
      inferred.push_back(true);
      L.messages.push_back(Message{"lowpoly.inferred_joint",
                                   "joint placed by the template inside part " + pname(segs[0].part) +
                                       " (no part boundary there)",
                                   Severity::Info, {{"part", segs[0].part}, {"s", best_s}}});
    }
    t.segments = segs;
    // Bone names.
    std::vector<std::string> names;
    int role = 0;
    for (size_t ci = 0; ci < chains.size(); ++ci)
      if (chain_tube[ci] == static_cast<int>(ti)) role = chain_role[ci];
    const bool left = t.attach.x > trunk_c.x;
    for (size_t k = 0; k < segs.size(); ++k) {
      if (named && role == 1) names.push_back(std::string(left ? "Left" : "Right") + (k == 0 ? "UpperArm" : k == 1 ? "LowerArm" : "Hand"));
      else if (named && role == 2) names.push_back(std::string(left ? "Left" : "Right") + (k == 0 ? "UpperLeg" : k == 1 ? "LowerLeg" : "Foot"));
      else names.push_back(pname(segs[k].part) + (segs.size() > 1 && segs[k].part == (k ? segs[k - 1].part : -1) ? "_2" : ""));
    }
    // Legs hang from the hips in the named templates.
    int prev = (named && role == 2 && !sk.bones.empty()) ? 0 : parent_bone;
    const Station& tip = t.dense.back();
    for (size_t k = 0; k < t.segments.size(); ++k) {
      Segment& g = t.segments[k];
      const vec3d hpos = world_centre(nearest_station(t.dense, g.s0), H);
      const vec3d tpos = world_centre(nearest_station(t.dense, k + 1 < t.segments.size() ? t.segments[k + 1].s0 : tip.s), H);
      g.bone = add_bone(names[k], prev, hpos, tpos, g.part);
      const Station& js = nearest_station(t.dense, g.s0);
      t.joints.push_back({g.s0, prev, g.bone, k == 0, inferred[k], 2 * js.half_w});
      prev = g.bone;
    }
    // A leaf end bone (hand/foot) for engines' humanoid mapping; it carries no vertices.
    if (named && (role == 1 || role == 2) && t.segments.size() == 2) {
      const double s_end = t.segments.back().s0 + 0.85 * (tip.s - t.segments.back().s0);
      add_bone(std::string(left ? "Left" : "Right") + (role == 1 ? "Hand" : "Foot"), prev,
               world_centre(nearest_station(t.dense, s_end), H), world_centre(tip, H), -1);
    }
  }
  return L;
}

}  // namespace pixel3d::lowpoly
