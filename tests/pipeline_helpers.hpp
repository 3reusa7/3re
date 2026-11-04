#pragma once
// Test-only: runs the reconstruction front half (align → bands → candidates →
// regions → evidence → correspondence) on fixtures or box objects.
#include <set>

#include "pixel3d/correspond/candidates.hpp"
#include "pixel3d/correspond/evidence.hpp"
#include "pixel3d/correspond/solve.hpp"
#include "pixel3d/image/palette.hpp"
#include "pixel3d/segment/regions.hpp"
#include "test_helpers.hpp"

namespace p3dtest {

using namespace pixel3d;


struct Corr {
  std::vector<int> part_of_candidate() const { return result.part_of_candidate; }
  std::vector<SourceView> views;
  Dims dims;
  std::map<ViewRole, Mask2D> masks;
  BandSet bands;
  CandidateSet cands;
  std::map<ViewRole, RegionMap> regions;
  std::vector<CandidateEvidence> evidence;
  CorrespondenceResult result;

  const Candidate* at(int band_y, int x, int z) const {  // candidate whose cells contain (x, ·, z) in the band at band_y
    const Band* b = bands.band_containing(band_y);
    for (const auto& c : cands.cands)
      if (c.band == b->id && x >= c.footprint.x0 && x < c.footprint.x1 && z >= c.footprint.y0 && z < c.footprint.y1)
        return &c;
    return nullptr;
  }
  std::vector<const Candidate*> in_band(int band_y) const {
    const Band* b = bands.band_containing(band_y);
    std::vector<const Candidate*> out;
    for (const auto& c : cands.cands)
      if (c.band == b->id) out.push_back(&c);
    return out;
  }
  int count_kind(const std::string& k) const {
    int n = 0;
    for (const auto& a : result.ambiguities) n += a.kind == k ? 1 : 0;
    return n;
  }
};

inline Corr run(std::vector<SourceView> views, Dims dims, const std::vector<UserDecision>& decisions = {}) {
  Corr c;
  c.views = std::move(views);
  c.dims = dims;
  std::map<ViewRole, Mask2D> ym;
  std::vector<const ImageRGBA*> imgs;
  for (const auto& v : c.views) {
    c.masks.emplace(v.role, opaque_mask(*v.aligned));
    if (v.role != ViewRole::Top && v.role != ViewRole::Bottom) ym.emplace(v.role, opaque_mask(*v.aligned));
    imgs.push_back(v.aligned.get());
  }
  const MaterialFamilies fam = analyze_palette(imgs);
  for (const auto& v : c.views) c.regions.emplace(v.role, compute_regions(*v.aligned, fam));
  c.bands = compute_bands(ym, dims.H);
  c.cands = build_candidates(c.bands, build_view_polygons(c.views, SimplifyOptions{}, 128), dims);
  const VoxelGrid hull = visual_hull(c.masks, dims);
  const RegionMap* top = c.regions.contains(ViewRole::Top) ? &c.regions.at(ViewRole::Top) : nullptr;
  const RegionMap* bottom = c.regions.contains(ViewRole::Bottom) ? &c.regions.at(ViewRole::Bottom) : nullptr;
  c.evidence = gather_evidence(c.cands, c.bands, hull, top, bottom, dims);
  c.result = solve_correspondence(c.cands, c.bands, c.evidence, c.regions, decisions);
  return c;
}

inline Corr run_fixture(const std::string& name, std::vector<ViewRole> roles = {ViewRole::Front, ViewRole::Side, ViewRole::Top},
                 const std::vector<UserDecision>& decisions = {}) {
  auto f = p3dtest::load_fixture(name, roles);
  return run(f.views, f.align.dims, decisions);
}

inline Corr run_boxes(const std::vector<Box>& boxes, Dims d) {
  std::vector<SourceView> views;
  for (auto& [r, img] : p3dtest::render_views(boxes, d)) {
    SourceView s;
    s.role = r;
    s.original = s.aligned = std::make_shared<const ImageRGBA>(img);
    views.push_back(s);
  }
  return run(views, d);
}


}  // namespace p3dtest
