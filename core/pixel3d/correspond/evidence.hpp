#pragma once
// Y-view evidence for candidates (spec §5.3 rule 3): where Top (or Bottom) can
// see a candidate's footprint without the maximal hull in the way, its colour
// regions tell whether the visible surface belongs to this band.
#include <vector>

#include "pixel3d/correspond/candidates.hpp"
#include "pixel3d/segment/regions.hpp"

namespace pixel3d {

enum class Evidence { None, Present, Absent };
std::string_view to_string(Evidence e);

struct CandidateEvidence {
  Evidence top = Evidence::None;
  Evidence bottom = Evidence::None;
  double unoccluded_fraction_top = 0;
  double unoccluded_fraction_bottom = 0;
  bool present() const { return top == Evidence::Present || bottom == Evidence::Present; }
  bool absent() const { return !present() && (top == Evidence::Absent || bottom == Evidence::Absent); }
};

std::vector<CandidateEvidence> gather_evidence(const CandidateSet& cs, const BandSet& bands, const VoxelGrid& max_hull,
                                               const RegionMap* top, const RegionMap* bottom, Dims d);

}  // namespace pixel3d
