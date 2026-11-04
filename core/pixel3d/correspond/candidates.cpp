#include "pixel3d/correspond/candidates.hpp"

namespace pixel3d {

std::string_view to_string(CandStatus s) {
  switch (s) {
    case CandStatus::Certain: return "certain";
    case CandStatus::CertainPalette: return "certain_palette";
    case CandStatus::InferredContinuation: return "inferred_continuation";
    case CandStatus::InferredGrid: return "inferred_grid";
    case CandStatus::InferredFill: return "inferred_fill";
    case CandStatus::Ambiguous: return "ambiguous";
    case CandStatus::Removed: return "removed";
  }
  return "certain";
}

bool is_kept(CandStatus s) { return s != CandStatus::Removed; }

const Candidate* CandidateSet::by_original_id(int oid) const {
  for (const Candidate& c : cands)
    if (c.original_id == oid) return &c;
  return nullptr;
}

CandidateSet build_candidates(const BandSet& bands, const std::map<ViewRole, ViewPolygons>& polys, Dims d) {
  CandidateSet cs;
  if (!polys.contains(ViewRole::Front) || !polys.contains(ViewRole::Side) || !polys.contains(ViewRole::Top)) return cs;
  // Views that constrain every candidate as a whole.
  manifold::Manifold common = make_prism(ViewRole::Top, d, polys.at(ViewRole::Top).simplified);
  for (ViewRole r : {ViewRole::Bottom, ViewRole::Back, ViewRole::Right})
    if (auto it = polys.find(r); it != polys.end()) common = common ^ make_prism(r, d, it->second.simplified);

  for (const Band& band : bands.bands) {
    const BandView* fv = bands.view(band.id, ViewRole::Front);
    const BandView* sv = bands.view(band.id, ViewRole::Side);
    if (!fv || !sv) continue;
    std::vector<manifold::Manifold> fp, sp;
    for (const Atom& a : fv->atoms)
      fp.push_back(make_prism(ViewRole::Front, d, atom_region(polys.at(ViewRole::Front), band, *fv, a, d.H)));
    for (const Atom& a : sv->atoms)
      sp.push_back(make_prism(ViewRole::Side, d, atom_region(polys.at(ViewRole::Side), band, *sv, a, d.H)));
    for (const Atom& fa : fv->atoms)
      for (const Atom& sa : sv->atoms) {
        manifold::Manifold solid = fp[fa.id] ^ sp[sa.id] ^ common;
        if (solid.IsEmpty()) continue;
        const double vol = solid.Volume();
        if (vol < 1e-6) continue;
        Candidate c;
        c.id = static_cast<int>(cs.cands.size());
        c.band = band.id;
        c.front_atom = fa.id;
        c.side_atom = sa.id;
        c.solid = solid.AsOriginal();
        c.original_id = c.solid.OriginalID();
        c.volume = vol;
        c.footprint = Mask2D::Box{fa.a, sa.a, fa.b, sa.b};
        cs.cands.push_back(std::move(c));
      }
  }
  return cs;
}

VoxelGrid voxelize_candidates(const CandidateSet& cs, const std::map<ViewRole, Mask2D>& masks, const BandSet& bands,
                              Dims d, bool include_removed) {
  VoxelGrid g(d);
  for (const Candidate& c : cs.cands) {
    if (!include_removed && !is_kept(c.status)) continue;
    const Band& band = bands.bands[c.band];
    const Atom& fa = bands.view(c.band, ViewRole::Front)->atoms[c.front_atom];
    const Atom& sa = bands.view(c.band, ViewRole::Side)->atoms[c.side_atom];
    for (int y = band.y0; y < band.y1; ++y)
      for (int z = sa.a; z < sa.b; ++z)
        for (int x = fa.a; x < fa.b; ++x) {
          bool in = true;
          for (const auto& [role, m] : masks) {
            const auto [u, v] = cell_pixel(role, d, x, y, z);
            if (!m.get_or_false(u, v)) {
              in = false;
              break;
            }
          }
          if (in) g.set(x, y, z, true);
        }
  }
  return g;
}

}  // namespace pixel3d
