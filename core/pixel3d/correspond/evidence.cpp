#include "pixel3d/correspond/evidence.hpp"

#include <map>
#include <set>

namespace pixel3d {
namespace {

constexpr double kMinUnoccluded = 0.5;
constexpr double kBelongs = 0.9;  // fraction of a region's cells that must lie in the band

using Cell = std::pair<int, int>;  // (x, z)

// Cells (x, z) where the candidate has volume.
std::set<Cell> candidate_cells(const Candidate& c, const BandSet& bands, const VoxelGrid& hull) {
  const Band& b = bands.bands[c.band];
  std::set<Cell> cells;
  for (int z = c.footprint.y0; z < c.footprint.y1; ++z)
    for (int x = c.footprint.x0; x < c.footprint.x1; ++x)
      for (int y = b.y0; y < b.y1; ++y)
        if (hull.get_or_false(x, y, z)) {
          cells.insert({x, z});
          break;
        }
  return cells;
}

struct ViewSide {
  const RegionMap* regions;
  bool from_top;
};

Evidence classify(const std::set<Cell>& visible, const Candidate& c, const std::set<Cell>& band_cells,
                  const ViewSide& vs, Dims d) {
  auto pixel_of = [&](Cell cell) { return std::pair<int, int>{cell.first, vs.from_top ? cell.second : d.D - 1 - cell.second}; };
  auto cell_of = [&](int u, int v) { return Cell{u, vs.from_top ? v : d.D - 1 - v}; };
  std::map<int, int> hits;  // region → visible cells it covers
  for (const Cell& cell : visible) {
    const auto [u, v] = pixel_of(cell);
    if (u < 0 || v < 0 || u >= vs.regions->w || v >= vs.regions->h) continue;
    const int r = vs.regions->at(u, v);
    if (r >= 0) ++hits[r];
  }
  if (hits.empty()) return Evidence::None;
  const double half = 0.5 * static_cast<double>(visible.size());
  // A region that lies mostly inside the footprint and covers half of it: the
  // artist drew this candidate's own surface.
  for (const auto& [r, n] : hits) {
    if (n < half) continue;
    const Region& reg = vs.regions->regions[r];
    int inside = 0;
    for (int i : reg.pixels) {
      const Cell cell = cell_of(i % vs.regions->w, i / vs.regions->w);
      if (cell.first >= c.footprint.x0 - 1 && cell.first < c.footprint.x1 + 1 && cell.second >= c.footprint.y0 - 1 &&
          cell.second < c.footprint.y1 + 1)
        ++inside;
    }
    if (inside * 2 >= static_cast<int>(reg.pixels.size())) return Evidence::Present;
  }
  // Otherwise: do the visible surfaces belong to this band?
  bool all_belong = true, dominant_foreign = false;
  for (const auto& [r, n] : hits) {
    const Region& reg = vs.regions->regions[r];
    int in_band = 0;
    for (int i : reg.pixels) in_band += band_cells.contains(cell_of(i % vs.regions->w, i / vs.regions->w)) ? 1 : 0;
    const bool belongs = in_band >= kBelongs * static_cast<double>(reg.pixels.size());
    all_belong &= belongs;
    if (!belongs && n >= half) dominant_foreign = true;
  }
  if (all_belong) return Evidence::Present;
  if (dominant_foreign) return Evidence::Absent;
  return Evidence::None;
}

}  // namespace

std::string_view to_string(Evidence e) {
  switch (e) {
    case Evidence::None: return "none";
    case Evidence::Present: return "present";
    case Evidence::Absent: return "absent";
  }
  return "none";
}

std::vector<CandidateEvidence> gather_evidence(const CandidateSet& cs, const BandSet& bands, const VoxelGrid& hull,
                                               const RegionMap* top, const RegionMap* bottom, Dims d) {
  std::vector<std::set<Cell>> cells(cs.cands.size());
  std::map<int, std::set<Cell>> band_cells;
  for (const Candidate& c : cs.cands) {
    cells[c.id] = candidate_cells(c, bands, hull);
    band_cells[c.band].insert(cells[c.id].begin(), cells[c.id].end());
  }
  std::vector<CandidateEvidence> ev(cs.cands.size());
  for (const Candidate& c : cs.cands) {
    const Band& b = bands.bands[c.band];
    if (cells[c.id].empty()) continue;
    for (const bool from_top : {true, false}) {
      const RegionMap* rm = from_top ? top : bottom;
      if (!rm) continue;
      std::set<Cell> visible;
      for (const Cell& cell : cells[c.id]) {
        bool occluded = false;
        if (from_top) {
          for (int y = b.y1; y < d.H && !occluded; ++y) occluded = hull.get(cell.first, y, cell.second);
        } else {
          for (int y = 0; y < b.y0 && !occluded; ++y) occluded = hull.get(cell.first, y, cell.second);
        }
        if (!occluded) visible.insert(cell);
      }
      const double frac = static_cast<double>(visible.size()) / static_cast<double>(cells[c.id].size());
      Evidence e = Evidence::None;
      if (frac >= kMinUnoccluded) e = classify(visible, c, band_cells[c.band], ViewSide{rm, from_top}, d);
      if (from_top) ev[c.id].top = e, ev[c.id].unoccluded_fraction_top = frac;
      else ev[c.id].bottom = e, ev[c.id].unoccluded_fraction_bottom = frac;
    }
  }
  return ev;
}

}  // namespace pixel3d
