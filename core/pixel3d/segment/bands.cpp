#include "pixel3d/segment/bands.hpp"

#include <algorithm>
#include <optional>
#include <set>

namespace pixel3d {
namespace {

using RowRuns = std::vector<std::pair<int, int>>;

RowRuns runs_of_row(const Mask2D& m, int v) {
  RowRuns r;
  int u = 0;
  while (u < m.width()) {
    if (!m.get(u, v)) {
      ++u;
      continue;
    }
    const int a = u;
    while (u < m.width() && m.get(u, v)) ++u;
    r.emplace_back(a, u);
  }
  return r;
}

// Runs touch if they overlap or meet diagonally (8-connectivity).
bool touch(std::pair<int, int> x, std::pair<int, int> y) { return x.first <= y.second && y.first <= x.second; }

// Index map lower→upper if the runs of two rows match 1-to-1, else empty.
std::optional<std::vector<int>> match_rows(const RowRuns& lo, const RowRuns& hi) {
  if (lo.size() != hi.size()) return std::nullopt;
  std::vector<int> map(lo.size(), -1);
  for (size_t i = 0; i < lo.size(); ++i) {
    int found = -1, count = 0;
    for (size_t j = 0; j < hi.size(); ++j)
      if (touch(lo[i], hi[j])) found = static_cast<int>(j), ++count;
    if (count != 1) return std::nullopt;
    map[i] = found;
  }
  for (size_t j = 0; j < hi.size(); ++j) {
    int count = 0;
    for (size_t i = 0; i < lo.size(); ++i) count += touch(lo[i], hi[j]) ? 1 : 0;
    if (count != 1) return std::nullopt;
  }
  return map;
}

}  // namespace

const BandView* BandSet::view(int band, ViewRole r) const {
  if (band < 0 || band >= static_cast<int>(bands.size())) return nullptr;
  for (const BandView& v : bands[band].views)
    if (v.role == r) return &v;
  return nullptr;
}

const Band* BandSet::band_containing(int y) const {
  for (const Band& b : bands)
    if (y >= b.y0 && y < b.y1) return &b;
  return nullptr;
}

BandSet compute_bands(const std::map<ViewRole, Mask2D>& y_views, int H) {
  // Runs per view per world row.
  std::map<ViewRole, std::vector<RowRuns>> rows;
  for (const auto& [role, m] : y_views) {
    auto& rr = rows[role];
    rr.resize(H);
    for (int y = 0; y < H; ++y)
      if (H - 1 - y < m.height()) rr[y] = runs_of_row(m, H - 1 - y);
  }
  auto has_content = [&](int y) {
    for (const auto& [role, rr] : rows)
      if (!rr[y].empty()) return true;
    return false;
  };
  auto continuous = [&](int y) {  // rows y and y+1
    for (const auto& [role, rr] : rows)
      if (!match_rows(rr[y], rr[y + 1])) return false;
    return true;
  };

  BandSet bs;
  for (int y = 0; y < H;) {
    if (!has_content(y)) {
      ++y;
      continue;
    }
    Band b;
    b.id = static_cast<int>(bs.bands.size());
    b.y0 = y;
    while (y + 1 < H && has_content(y + 1) && continuous(y)) ++y;
    b.y1 = y + 1;
    for (const auto& [role, rr] : rows) {
      BandView bv;
      bv.role = role;
      // Chain runs bottom-up through the 1-to-1 row matchings.
      for (size_t i = 0; i < rr[b.y0].size(); ++i) {
        Strand s;
        s.runs.push_back(Run{b.y0, rr[b.y0][i].first, rr[b.y0][i].second});
        bv.strands.push_back(std::move(s));
      }
      std::vector<int> idx(bv.strands.size());
      for (size_t i = 0; i < idx.size(); ++i) idx[i] = static_cast<int>(i);  // strand → run index in current row
      for (int yy = b.y0; yy + 1 < b.y1; ++yy) {
        const auto map = *match_rows(rr[yy], rr[yy + 1]);
        for (size_t s = 0; s < bv.strands.size(); ++s) {
          idx[s] = map[idx[s]];
          bv.strands[s].runs.push_back(Run{yy + 1, rr[yy + 1][idx[s]].first, rr[yy + 1][idx[s]].second});
        }
      }
      for (Strand& s : bv.strands) {
        s.a = s.runs.front().a;
        s.b = s.runs.front().b;
        for (const Run& r : s.runs) s.a = std::min(s.a, r.a), s.b = std::max(s.b, r.b);
      }
      std::sort(bv.strands.begin(), bv.strands.end(), [](const Strand& x, const Strand& y) { return x.a < y.a; });
      for (size_t s = 0; s < bv.strands.size(); ++s) bv.strands[s].id = static_cast<int>(s);
      b.views.push_back(std::move(bv));
    }
    bs.bands.push_back(std::move(b));
    ++y;
  }

  // Atoms: split strands where an adjacent band (touching rows) branches into
  // more strands in the same view.
  for (Band& b : bs.bands)
    for (BandView& bv : b.views) {
      std::set<int> cuts;
      for (const Band& nb : bs.bands) {
        if (nb.y1 != b.y0 && nb.y0 != b.y1) continue;
        const BandView* nv = bs.view(nb.id, bv.role);
        if (!nv || nv->strands.size() <= bv.strands.size()) continue;
        for (const Strand& s : nv->strands) cuts.insert(s.a), cuts.insert(s.b);
      }
      for (const Strand& s : bv.strands) {
        std::vector<int> pts = {s.a};
        for (int c : cuts)
          if (c > s.a && c < s.b) pts.push_back(c);
        pts.push_back(s.b);
        const bool split = pts.size() > 2;
        for (size_t k = 0; k + 1 < pts.size(); ++k)
          bv.atoms.push_back(Atom{static_cast<int>(bv.atoms.size()), s.id, pts[k], pts[k + 1], split});
      }
    }
  return bs;
}

}  // namespace pixel3d
