#include "pixel3d/lowpoly/suggest.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <set>
#include <string>

#include "pixel3d/lowpoly/profile.hpp"
#include "pixel3d/lowpoly/spine.hpp"
#include "pixel3d/segment/regions.hpp"

namespace pixel3d::lowpoly {
namespace {

constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

std::vector<Mask2D> components(const Mask2D& m, int min_px) {
  const int w = m.width(), h = m.height();
  std::vector<int> seen(static_cast<size_t>(w) * h, 0);
  std::vector<Mask2D> out;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (!m.get(x, y) || seen[static_cast<size_t>(y) * w + x]) continue;
      Mask2D c(w, h);
      int n = 0;
      std::deque<std::pair<int, int>> q{{x, y}};
      seen[static_cast<size_t>(y) * w + x] = 1;
      while (!q.empty()) {
        const auto [cx, cy] = q.front();
        q.pop_front();
        c.set(cx, cy, true);
        ++n;
        for (int k = 0; k < 8; ++k) {
          const int nx = cx + kDx[k], ny = cy + kDy[k];
          if (!m.get_or_false(nx, ny) || seen[static_cast<size_t>(ny) * w + nx]) continue;
          seen[static_cast<size_t>(ny) * w + nx] = 1;
          q.push_back({nx, ny});
        }
      }
      if (n >= min_px) out.push_back(std::move(c));
    }
  return out;
}

int contact(const Mask2D& a, const Mask2D& b) {
  int n = 0;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) {
      if (!a.get(x, y)) continue;
      for (int k = 0; k < 8; ++k)
        if (b.get_or_false(x + kDx[k], y + kDy[k])) {
          ++n;
          break;
        }
    }
  return n;
}

void merge_into(Mask2D& dst, const Mask2D& src) {
  for (int y = 0; y < src.height(); ++y)
    for (int x = 0; x < src.width(); ++x)
      if (src.get(x, y)) dst.set(x, y, true);
}

// Distance (px) from every pixel to the nearest pixel of `sites`, with no image border acting as a site.
DistanceField distance_to(const Mask2D& sites, int pad) {
  const int w = sites.width() + 2 * pad, h = sites.height() + 2 * pad;
  Mask2D not_sites(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) not_sites.set(x, y, !sites.get_or_false(x - pad, y - pad));
  const DistanceField big = distance_transform(not_sites);
  DistanceField d;
  d.w = sites.width();
  d.h = sites.height();
  d.d.resize(static_cast<size_t>(d.w) * d.h);
  for (int y = 0; y < d.h; ++y)
    for (int x = 0; x < d.w; ++x) d.d[static_cast<size_t>(y) * d.w + x] = big.at(x + pad, y + pad);
  return d;
}

// Cores (eroded at half the largest inscribed radius, each grown back over the opening)
// and protrusions (what the opening removes).
std::vector<Mask2D> split_by_shape(const Mask2D& m, int min_piece) {
  const DistanceField dt = distance_transform(m);
  const double r = 0.5 * dt.max();
  if (r < 1.5) return {m};
  Mask2D eroded(m.width(), m.height());
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) eroded.set(x, y, dt.at(x, y) >= r);
  const std::vector<Mask2D> cores = components(eroded, 3);
  if (cores.empty()) return {m};
  const DistanceField to_core = distance_to(eroded, static_cast<int>(std::ceil(r)) + 2);
  Mask2D opened(m.width(), m.height());
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) opened.set(x, y, m.get(x, y) && (eroded.get(x, y) || to_core.at(x, y) <= r + 0.5));
  // Grow every core over the opened pixels (nearest core wins: BFS in lockstep).
  const int w = m.width(), h = m.height();
  std::vector<int> owner(static_cast<size_t>(w) * h, -1);
  std::deque<std::pair<int, int>> q;
  for (size_t c = 0; c < cores.size(); ++c)
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (cores[c].get(x, y)) owner[static_cast<size_t>(y) * w + x] = static_cast<int>(c), q.push_back({x, y});
  while (!q.empty()) {
    const auto [x, y] = q.front();
    q.pop_front();
    for (int k = 0; k < 8; ++k) {
      const int nx = x + kDx[k], ny = y + kDy[k];
      if (!opened.get_or_false(nx, ny) || owner[static_cast<size_t>(ny) * w + nx] >= 0) continue;
      owner[static_cast<size_t>(ny) * w + nx] = owner[static_cast<size_t>(y) * w + x];
      q.push_back({nx, ny});
    }
  }
  std::vector<Mask2D> pieces(cores.size(), Mask2D(w, h));
  Mask2D rest(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (!m.get(x, y)) continue;
      const int o = owner[static_cast<size_t>(y) * w + x];
      if (o >= 0) pieces[static_cast<size_t>(o)].set(x, y, true);
      else rest.set(x, y, true);
    }
  // A protrusion is a part only if it sticks out: its contact with the opened shape is
  // shorter than most of its length (a corner shaved off by the opening touches along all of it).
  auto sticks_out = [&](const Mask2D& c) {
    double n = 0, sx = 0, sy = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (c.get(x, y)) n += 1, sx += x, sy += y;
    const double mx = sx / n, my = sy / n;
    double cxx = 0, cyy = 0, cxy = 0;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        if (c.get(x, y)) cxx += (x - mx) * (x - mx), cyy += (y - my) * (y - my), cxy += (x - mx) * (y - my);
    cxx /= n, cyy /= n, cxy /= n;
    const double l1 = (cxx + cyy) / 2 + std::sqrt(std::max(0.0, (cxx - cyy) * (cxx - cyy) / 4 + cxy * cxy));
    const double len = 2 * std::sqrt(3 * l1);  // length of a bar with this spread
    return contact(c, opened) < 0.6 * len;
  };
  std::vector<Mask2D> small;
  for (Mask2D& c : components(rest, 1)) (c.count() >= min_piece && sticks_out(c) ? pieces : small).push_back(std::move(c));
  // Small leftovers join the piece they touch most.
  for (const Mask2D& s : small) {
    size_t best = 0;
    int bc = -1;
    for (size_t i = 0; i < pieces.size(); ++i) {
      const int c = contact(s, pieces[i]);
      if (c > bc) bc = c, best = i;
    }
    merge_into(pieces[best], s);
  }
  return pieces;
}

// A colour family is a ramp, so shading never splits a part; but two shades of one ramp can
// be two parts: a sleeve against a tunic with an ink line drawn between them. Inside every
// region, the patches of one exact colour (tones) are grouped back together where they meet
// directly (shading). A group stays apart when most of its inner boundary is ink or another
// family: the artist drew it as a shape of its own. Groups smaller than `min_px` rejoin.
void split_family_regions(const ImageRGBA& img, const Mask2D& subject, std::vector<int>& lab, int min_px) {
  const int w = img.width(), h = img.height();
  auto at = [&](int x, int y) { return static_cast<size_t>(y) * w + x; };
  auto rgb = [&](int x, int y) {
    const Rgba8 c = img.at(x, y);
    return (uint32_t{c.r} << 16) | (uint32_t{c.g} << 8) | c.b;
  };
  std::vector<int> tone(static_cast<size_t>(w) * h, -1), tone_lab, tone_px;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (lab[at(x, y)] < 0 || tone[at(x, y)] >= 0) continue;
      const int t = static_cast<int>(tone_lab.size());
      tone_lab.push_back(lab[at(x, y)]);
      tone_px.push_back(0);
      std::deque<std::pair<int, int>> q{{x, y}};
      tone[at(x, y)] = t;
      while (!q.empty()) {
        const auto [cx, cy] = q.front();
        q.pop_front();
        ++tone_px[static_cast<size_t>(t)];
        for (int k = 0; k < 4; ++k) {
          const int nx = cx + kDx[k], ny = cy + kDy[k];
          if (nx < 0 || ny < 0 || nx >= w || ny >= h || tone[at(nx, ny)] >= 0 || lab[at(nx, ny)] != tone_lab[static_cast<size_t>(t)] ||
              rgb(nx, ny) != rgb(cx, cy))
            continue;
          tone[at(nx, ny)] = t;
          q.push_back({nx, ny});
        }
      }
    }
  const int n = static_cast<int>(tone_lab.size());
  if (n < 2 || n > 600) return;
  // Inner boundary per tone (edges to other opaque pixels; an ink pixel counts once, toward
  // what lies beyond it, and not at all when only transparency or the same tone lies
  // beyond); direct contacts and contacts across one ink pixel, between tones of one region.
  std::vector<double> inner(static_cast<size_t>(n), 0);
  std::map<std::pair<int, int>, double> direct, across;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int t = tone[at(x, y)];
      if (t < 0) continue;
      for (int k = 0; k < 4; ++k) {
        const int qx = x + kDx[k], qy = y + kDy[k];
        if (!subject.get_or_false(qx, qy)) continue;
        const int tq = tone[at(qx, qy)];
        if (tq == t) continue;
        if (tq >= 0) {
          inner[static_cast<size_t>(t)] += 1;
          if (tone_lab[static_cast<size_t>(tq)] == tone_lab[static_cast<size_t>(t)]) direct[{t, tq}] += 1;
          continue;
        }
        const int rx = qx + kDx[k], ry = qy + kDy[k];
        if (!subject.get_or_false(rx, ry)) continue;  // ink on the silhouette
        const int tr = tone[at(rx, ry)];
        if (tr == t) continue;  // a detail line inside the tone
        inner[static_cast<size_t>(t)] += 1;
        if (tr >= 0 && tone_lab[static_cast<size_t>(tr)] == tone_lab[static_cast<size_t>(t)]) across[{t, tr}] += 1;
      }
    }
  // Agglomerate: merge the pair whose direct contact is the largest share of either side's
  // inner boundary, while that share is at least a half.
  std::vector<int> grp(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) grp[static_cast<size_t>(i)] = i;
  std::vector<int> gpx = tone_px;
  auto merge = [&](int g, int hh) {
    inner[static_cast<size_t>(g)] += inner[static_cast<size_t>(hh)] - direct[{g, hh}] - direct[{hh, g}] - across[{g, hh}] - across[{hh, g}];
    gpx[static_cast<size_t>(g)] += gpx[static_cast<size_t>(hh)];
    for (auto* m : {&direct, &across}) {
      std::map<std::pair<int, int>, double> out;
      for (const auto& [k, v] : *m) {
        std::pair<int, int> nk{k.first == hh ? g : k.first, k.second == hh ? g : k.second};
        if (nk.first != nk.second) out[nk] += v;
      }
      *m = std::move(out);
    }
    for (int& x : grp)
      if (x == hh) x = g;
  };
  auto get = [](const std::map<std::pair<int, int>, double>& m, int a, int b) {
    const auto it = m.find({a, b});
    return it == m.end() ? 0.0 : it->second;
  };
  for (;;) {
    double best = 0;
    std::pair<int, int> pick{-1, -1};
    for (const auto& [k, v] : direct) {
      const auto [g, hh] = k;
      const double d = (get(direct, g, hh) + get(direct, hh, g)) / 2, a = (get(across, g, hh) + get(across, hh, g)) / 2;
      const double small = std::max(1.0, std::min(inner[static_cast<size_t>(g)], inner[static_cast<size_t>(hh)]));
      // Shading: they touch more directly than across ink, along a fair share of the smaller
      // one's inner edge (a sleeve touching the tunic only at one corner stays apart).
      if (d < a || d < 0.3 * small) continue;
      if (d / small > best) best = d / small, pick = k;
    }
    if (pick.first < 0) break;
    merge(pick.first, pick.second);
  }
  // Small groups rejoin the group of their region they touch most (directly or across ink).
  for (bool changed = true; changed;) {
    changed = false;
    for (int g = 0; g < n; ++g) {
      if (gpx[static_cast<size_t>(g)] == 0 || gpx[static_cast<size_t>(g)] >= min_px) continue;
      int to = -1;
      double bc = 0;
      for (const auto* m : {&direct, &across})
        for (const auto& [k, v] : *m)
          if (k.first == g && v > bc) bc = v, to = k.second;
      if (to < 0) continue;
      merge(to, g);
      gpx[static_cast<size_t>(g)] = 0;
      changed = true;
    }
  }
  // New labels: the largest group of a region keeps the region's label.
  std::map<int, int> keeper;  // region → group with most pixels
  for (int t = 0; t < n; ++t) {
    const int g = grp[static_cast<size_t>(t)], r = tone_lab[static_cast<size_t>(t)];
    if (!keeper.count(r) || gpx[static_cast<size_t>(g)] > gpx[static_cast<size_t>(keeper[r])]) keeper[r] = g;
  }
  int next = 0;
  for (int v : lab) next = std::max(next, v + 1);
  std::map<int, int> label_of;
  for (int t = 0; t < n; ++t) {
    const int g = grp[static_cast<size_t>(t)], r = tone_lab[static_cast<size_t>(t)];
    if (!label_of.count(g)) label_of[g] = keeper[r] == g ? r : next++;
  }
  for (size_t p = 0; p < lab.size(); ++p)
    if (tone[p] >= 0) lab[p] = label_of[grp[static_cast<size_t>(tone[p])]];
}

// The share of a mask's edge (4-neighbour) that is the subject's silhouette.
double silhouette_share(const Mask2D& m, const Mask2D& subject) {
  int edge = 0, sil = 0;
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) {
      if (!m.get(x, y)) continue;
      for (int k = 0; k < 4; ++k) {
        const int nx = x + kDx[k], ny = y + kDy[k];
        if (m.get_or_false(nx, ny)) continue;
        ++edge;
        sil += !subject.get_or_false(nx, ny);
      }
    }
  return edge ? static_cast<double>(sil) / edge : 0;
}

// A band painted across the whole shape: on most of its rows (or columns) it holds both
// ends of the subject's row (column).
bool crosses(const Mask2D& m, const Mask2D& subject, bool rows) {
  const int n = rows ? m.height() : m.width(), len = rows ? m.width() : m.height();
  int lines = 0, full = 0;
  for (int i = 0; i < n; ++i) {
    int a = -1, b = -1, own = 0;
    for (int j = 0; j < len; ++j) {
      const int x = rows ? j : i, y = rows ? i : j;
      if (subject.get(x, y)) b = j, a = a < 0 ? j : a;
      own += m.get(x, y);
    }
    if (own == 0 || a < 0) continue;
    ++lines;
    full += m.get(rows ? a : i, rows ? i : a) && m.get(rows ? b : i, rows ? i : b);
  }
  return lines > 0 && full >= 0.6 * lines;
}

struct Piece {
  Mask2D mask;
  int area = 0;
  vec2d centroid;
  Mask2D::Box box;
  double elongation = 1;
  PartType type = PartType::Accessory;
  std::string name;
};

int row_width(const Mask2D& m, int y) {
  int x0 = m.width(), x1 = -1;
  for (int x = 0; x < m.width(); ++x)
    if (m.get(x, y)) x0 = std::min(x0, x), x1 = std::max(x1, x);
  return x1 >= x0 ? x1 - x0 + 1 : 0;
}

}  // namespace

Suggestion suggest_parts(const SourceView& front, const MaterialFamilies& fam, uint8_t alpha, const Mask2D* subject_in,
                         const SourceView* side) {
  Suggestion s;
  const ImageRGBA& img = *front.aligned;
  const int w = img.width(), h = img.height();
  const Mask2D subject = subject_in ? *subject_in : opaque_mask(img, alpha);
  const int subject_px = std::max(1, subject.count());
  const int min_piece = std::max(10, static_cast<int>(0.012 * subject_px));
  // ---- 1. colour regions --------------------------------------------------------
  const RegionMap rm = compute_regions(img, fam, alpha);
  std::vector<int> lab(static_cast<size_t>(w) * h, -1);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (subject.get(x, y)) lab[static_cast<size_t>(y) * w + x] = rm.at(x, y);
  std::map<int, int> area;
  for (int v : lab)
    if (v >= 0) ++area[v];
  std::map<int, int> parent;  // region merges
  auto find = [&](int r) {
    while (parent.count(r)) r = parent[r];
    return r;
  };
  for (const Region& r : rm.regions) {
    const int a = area.count(r.id) ? area[r.id] : 0;
    const bool texture = (r.enclosed && a < 0.05 * subject_px) || a < min_piece / 2;
    if (!texture || r.neighbours.empty()) continue;
    int best = -1, ba = -1;
    for (int n : r.neighbours) {
      const int fn = find(n);
      if (fn == r.id) continue;
      if (area[fn] > ba) ba = area[fn], best = fn;
    }
    if (best >= 0) parent[r.id] = best, area[best] += a;
  }
  for (int& v : lab)
    if (v >= 0) v = find(v);
  split_family_regions(img, subject, lab, std::max(5, min_piece / 2));
  // Ink (and anything unlabelled) joins the nearest region.
  std::deque<std::pair<int, int>> q;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (lab[static_cast<size_t>(y) * w + x] >= 0) q.push_back({x, y});
  while (!q.empty()) {
    const auto [x, y] = q.front();
    q.pop_front();
    for (int k = 0; k < 8; ++k) {
      const int nx = x + kDx[k], ny = y + kDy[k];
      if (!subject.get_or_false(nx, ny) || lab[static_cast<size_t>(ny) * w + nx] >= 0) continue;
      lab[static_cast<size_t>(ny) * w + nx] = lab[static_cast<size_t>(y) * w + x];
      q.push_back({nx, ny});
    }
  }
  std::map<int, Mask2D> regions;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const int v = lab[static_cast<size_t>(y) * w + x];
      if (v < 0) continue;
      auto it = regions.try_emplace(v, w, h).first;
      it->second.set(x, y, true);
    }
  if (regions.empty() && subject.count()) regions.emplace(0, subject);
  // ---- 2. split by shape ---------------------------------------------------------
  std::vector<Piece> pieces;
  for (auto& [id, m] : regions)
    for (Mask2D& p : split_by_shape(m, min_piece)) {
      Piece pc;
      pc.mask = std::move(p);
      pieces.push_back(std::move(pc));
    }
  // Pieces too small to be parts join their best neighbour.
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < pieces.size(); ++i) {
      if (pieces[i].mask.count() >= min_piece || pieces.size() < 2) continue;
      size_t best = i;
      int bc = 0;
      for (size_t j = 0; j < pieces.size(); ++j) {
        if (j == i) continue;
        const int c = contact(pieces[i].mask, pieces[j].mask);
        if (c > bc) bc = c, best = j;
      }
      if (best == i) continue;
      merge_into(pieces[best].mask, pieces[i].mask);
      pieces.erase(pieces.begin() + static_cast<long>(i));
      changed = true;
      break;
    }
  }
  for (Piece& p : pieces) {
    p.area = p.mask.count();
    const Axis ax = principal_axis(p.mask);
    p.centroid = ax.centroid;
    p.elongation = ax.elongation;
    p.box = p.mask.bbox();
  }
  if (pieces.empty()) return s;
  // One solid shape painted in bands (a barrel's planks and hoops, stripes): every piece
  // runs across the whole shape in the same direction. Colour bands are not parts.
  if (pieces.size() > 1) {
    for (bool rows : {true, false}) {
      if (!std::all_of(pieces.begin(), pieces.end(), [&](const Piece& p) { return crosses(p.mask, subject, rows); })) continue;
      Piece one;
      one.mask = Mask2D(w, h);
      for (const Piece& p : pieces) merge_into(one.mask, p.mask);
      one.area = one.mask.count();
      const Axis ax = principal_axis(one.mask);
      one.centroid = ax.centroid, one.elongation = ax.elongation, one.box = one.mask.bbox();
      s.messages.push_back(Message{"lowpoly.bands_merged",
                                   std::to_string(pieces.size()) + " colour bands run across the whole shape: suggested as one part",
                                   Severity::Info, {{"bands", pieces.size()}}});
      pieces = {std::move(one)};
      break;
    }
  }
  // ---- 3. types ----------------------------------------------------------------
  int head = -1;
  for (size_t i = 0; i < pieces.size() && pieces.size() > 1; ++i)  // one piece is a body, not a head
    if (pieces[i].area >= 0.05 * subject_px && pieces[i].elongation < 1.8 &&
        (head < 0 || pieces[i].centroid.y < pieces[static_cast<size_t>(head)].centroid.y))
      head = static_cast<int>(i);
  // The body: the largest piece on the subject's centre line (a sleeve hanging by the body
  // can be larger than the tunic between the sleeves); any largest piece otherwise.
  const Mask2D::Box sb = subject.bbox();
  const int mid_x = (sb.x0 + sb.x1) / 2;
  auto on_centre = [&](const Piece& p) {
    for (int y = p.box.y0; y < p.box.y1; ++y)
      if (p.mask.get_or_false(mid_x, y)) return true;
    return false;
  };
  // (Below the head: a face drawn inside a hood is on the centre line too.)
  auto below_head = [&](const Piece& p) { return head < 0 || p.centroid.y > pieces[static_cast<size_t>(head)].box.y1; };
  int body = -1;
  for (int pass = 0; pass < 2 && body < 0; ++pass)
    for (size_t i = 0; i < pieces.size(); ++i)
      if (static_cast<int>(i) != head && (pass == 1 || (on_centre(pieces[i]) && below_head(pieces[i]))) &&
          (body < 0 || pieces[i].area > pieces[static_cast<size_t>(body)].area))
        body = static_cast<int>(i);
  if (head >= 0 && body >= 0 && pieces[static_cast<size_t>(head)].centroid.y > pieces[static_cast<size_t>(body)].centroid.y &&
      pieces[static_cast<size_t>(head)].area > pieces[static_cast<size_t>(body)].area)
    std::swap(head, body);  // the round blob below is the body, not the head
  if (head >= 0) pieces[static_cast<size_t>(head)].type = PartType::Head, pieces[static_cast<size_t>(head)].name = "head";
  if (body >= 0) {
    Piece& b = pieces[static_cast<size_t>(body)];
    const int top = row_width(b.mask, b.box.y0 + b.box.height() / 8), bot = row_width(b.mask, b.box.y1 - 1 - b.box.height() / 8);
    b.type = bot > 1.3 * top ? PartType::Cloth : PartType::Torso;
    b.name = "body";
  }
  // Limbs: elongated (or hanging) pieces sticking out of the shape, reachable from the body
  // through other limbs or through pieces drawn on the body (a belt between tunic and legs).
  std::vector<bool> is_limb(pieces.size(), false), bodyish(pieces.size(), false);
  if (body >= 0) bodyish[static_cast<size_t>(body)] = true;
  std::deque<int> lq;
  if (body >= 0) lq.push_back(body);
  std::vector<bool> visited(pieces.size(), false);
  if (body >= 0) visited[static_cast<size_t>(body)] = true;
  while (!lq.empty()) {
    const int cur = lq.front();
    lq.pop_front();
    for (size_t j = 0; j < pieces.size(); ++j) {
      if (visited[j] || static_cast<int>(j) == head) continue;
      if (contact(pieces[j].mask, pieces[static_cast<size_t>(cur)].mask) == 0) continue;
      const bool below = body >= 0 && pieces[j].centroid.y > pieces[static_cast<size_t>(body)].box.y1 - 0.2 * pieces[static_cast<size_t>(body)].box.height();
      // A limb sticks out: part of its edge is the silhouette. A belt or a pouch drawn on
      // the body (edge all inside the shape) is an accessory however long it is.
      const bool outside = silhouette_share(pieces[j].mask, subject) >= 0.2;
      if (outside && (pieces[j].elongation >= 1.6 || below)) {
        visited[j] = true;
        is_limb[j] = true;
        lq.push_back(static_cast<int>(j));
      } else if (!outside && bodyish[static_cast<size_t>(cur)]) {
        visited[j] = true;
        bodyish[j] = true;
        lq.push_back(static_cast<int>(j));
      }
    }
  }
  const double cx = body >= 0 ? pieces[static_cast<size_t>(body)].centroid.x : w / 2.0;
  std::map<std::string, int> counts;
  auto unique = [&](const std::string& base) {
    const int n = ++counts[base];
    return n == 1 ? base : base + "_" + std::to_string(n);
  };
  // Name limbs by side and level, nearest to the body first.
  std::vector<size_t> limb_order;
  for (size_t i = 0; i < pieces.size(); ++i)
    if (is_limb[i]) limb_order.push_back(i);
  auto dist_body = [&](size_t i) { return body >= 0 ? length(pieces[i].centroid - pieces[static_cast<size_t>(body)].centroid) : 0.0; };
  std::sort(limb_order.begin(), limb_order.end(), [&](size_t a, size_t b) { return dist_body(a) < dist_body(b); });
  auto touches_body = [&](size_t i) {
    for (size_t j = 0; j < pieces.size(); ++j)
      if (bodyish[j] && contact(pieces[i].mask, pieces[j].mask) > 0) return true;
    return false;
  };
  // Side and level come from the chain's first link (the limb touching the body), so a
  // forearm hanging low stays an arm.
  std::vector<std::string> base(pieces.size());
  for (size_t i : limb_order) {
    Piece& p = pieces[i];
    p.type = PartType::Limb;
    if (body >= 0 && touches_body(i)) {
      // Level from where the limb starts: an arm starts above the body's middle (a hanging
      // arm with its hand can reach below the body's lower fifth), a leg below it.
      const Piece& b = pieces[static_cast<size_t>(body)];
      const bool lower = p.box.y0 >= b.box.y0 + 0.5 * b.box.height();
      base[i] = std::string(lower ? "leg" : "arm") + (p.centroid.x > cx ? "_l" : "_r");
    } else {
      int most = 0;
      for (size_t j : limb_order) {  // the already named limb it touches most
        const int c = base[j].empty() ? 0 : contact(p.mask, pieces[j].mask);
        if (c > most) most = c, base[i] = base[j];
      }
      if (base[i].empty()) base[i] = std::string("limb") + (p.centroid.x > cx ? "_l" : "_r");
    }
    p.name = unique(base[i]);
  }
  for (size_t i = 0; i < pieces.size(); ++i) {
    Piece& p = pieces[i];
    if (!p.name.empty()) continue;
    p.type = PartType::Accessory;
    const bool on_head = head >= 0 && contact(p.mask, pieces[static_cast<size_t>(head)].mask) > 0 &&
                         silhouette_share(p.mask, subject) >= 0.2;  // a horn sticks out; a scarf does not
    p.name = unique(on_head && p.elongation >= 1.6 ? std::string("horn") + (p.centroid.x > cx ? "_l" : "_r") : std::string("accessory"));
  }
  // ---- template guess (same rule as the layout) ----------------------------------
  int upper_l = 0, upper_r = 0, lower_l = 0, lower_r = 0;
  for (size_t i : limb_order) {
    if (body < 0 || !touches_body(i)) continue;  // chain starts only
    const bool lower = pieces[i].name.rfind("leg", 0) == 0, left = pieces[i].centroid.x > cx;
    (lower ? (left ? lower_l : lower_r) : (left ? upper_l : upper_r))++;
  }
  const bool cloth = body >= 0 && pieces[static_cast<size_t>(body)].type == PartType::Cloth;
  if (head >= 0 && body >= 0 && upper_l == 1 && upper_r == 1 && lower_l == 1 && lower_r == 1) s.templ = Template::Humanoid;
  else if (head >= 0 && body >= 0 && upper_l + upper_r == 0 && lower_l == 1 && lower_r == 1 && cloth) s.templ = Template::CloakedCharacter;
  // ---- parts ---------------------------------------------------------------------
  std::vector<size_t> order(pieces.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  auto rank = [&](size_t i) {
    switch (pieces[i].type) {
      case PartType::Head: return 0;
      case PartType::Torso:
      case PartType::Cloth: return 1;
      case PartType::Limb: return 2;
      case PartType::Accessory: return 3;
    }
    return 4;
  };
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rank(a) != rank(b) ? rank(a) < rank(b) : pieces[a].name < pieces[b].name; });
  int next_id = kFirstSuggestedPart;
  std::string list;
  for (size_t i : order) {
    Part p;
    p.id = next_id++;
    p.type = pieces[i].type;
    p.name = pieces[i].name;
    p.masks.emplace(ViewRole::Front, pieces[i].mask);
    if (side && side->aligned) {
      // Derived: the Side pixels of the part's dominant colour family within its rows.
      std::map<int, int> fc;
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
          if (pieces[i].mask.get(x, y)) {
            const int f = fam.family_of(img.at(x, y), alpha);
            if (f >= 0 && f != fam.outline_family) ++fc[f];
          }
      // Its colour families (each at least 15% of its drawn colour; a barrel is wood and
      // metal); one part alone is the whole Side drawing, outline included.
      int total = 0, best = 0;
      for (const auto& [f, n] : fc) total += n, best = std::max(best, n);
      std::set<int> fams;
      for (const auto& [f, n] : fc)
        if (n == best || n >= 0.15 * total) fams.insert(f);
      const ImageRGBA& simg = *side->aligned;
      Mask2D sm(simg.width(), simg.height());
      if (pieces.size() == 1) {
        sm = opaque_mask(simg, alpha);
      } else {
        for (int y = pieces[i].box.y0; y < pieces[i].box.y1 && y < simg.height(); ++y)
          for (int x = 0; x < simg.width(); ++x)
            if (fams.contains(fam.family_of(simg.at(x, y), alpha))) sm.set(x, y, true);
      }
      if (sm.count() > 0) p.masks.emplace(ViewRole::Side, std::move(sm));
    }
    list += (list.empty() ? "" : ", ") + p.name + " (" + std::string(to_string(p.type)) + ")";
    s.parts.push_back(std::move(p));
  }
  s.messages.push_back(Message{"lowpoly.suggestion",
                               std::to_string(s.parts.size()) + " parts suggested: " + list + " — confirm or edit them before building",
                               Severity::Info, {{"template", to_string(s.templ)}, {"parts", s.parts.size()}}});
  if (side && side->aligned)
    s.messages.push_back(Message{"lowpoly.side_masks_derived",
                                 "Side masks are derived from colour families and rows: check overlapping parts", Severity::Info, {}});
  return s;
}

}  // namespace pixel3d::lowpoly
