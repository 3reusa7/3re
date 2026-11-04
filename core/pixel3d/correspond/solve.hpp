#pragma once
// Part correspondence (spec §5.3). Two levels:
//  1. Blocks — (Front strand × Side strand) within a band. Every strand must be
//     explained by a kept block. Evidence, necessity, 1-to-1 continuation,
//     palette, and the 2×2 grid prior decide; anything else is AMBIGUOUS.
//  2. Pieces — candidates inside kept blocks. Only split atoms covered by two or
//     more different kept blocks (e.g. a stretcher whose depth no view fixes)
//     are re-examined: continuation or evidence supports a piece, otherwise
//     its piece is AMBIGUOUS. Pieces of a single block are fill, never ambiguous.
// Parts are kept blocks joined across bands where strands continue 1-to-1.
#include <map>
#include <string>
#include <vector>

#include "pixel3d/correspond/candidates.hpp"
#include "pixel3d/correspond/evidence.hpp"
#include "pixel3d/segment/regions.hpp"

namespace pixel3d {

struct Part3D {
  int id = 0;
  std::vector<int> candidates;
  CandStatus status = CandStatus::Certain;  // most uncertain member
  std::string label;
};

struct Ambiguity {
  std::string kind;  // ambiguous_correspondence | evidence_conflict | edit.uncovers_atom
  int band = -1;
  std::vector<int> candidates;
  std::vector<std::vector<int>> alternative_covers;  // candidate ids, minimal alternatives (≤ 8)
  std::string text;
};

struct CorrespondenceResult {
  std::vector<Part3D> parts;
  std::vector<int> part_of_candidate;  // -1 for removed candidates
  std::vector<Ambiguity> ambiguities;
  double inferred_volume = 0;
  double ambiguous_volume = 0;
};

struct UserDecision {
  int candidate = -1;
  bool keep = true;
  // Derived, never stored: the candidate lies inside a user part's mask, which
  // now explains its pixels. It is removed unconditionally, and the strands it
  // explained count as covered.
  bool claimed = false;
};

// Higher = less certain. Removed ranks lowest.
int status_rank(CandStatus s);

// Writes status/reason into `cands` and returns parts and ambiguities.
// `regions` must hold the aligned views' colour regions (Front/Side used for palette).
CorrespondenceResult solve_correspondence(CandidateSet& cands, const BandSet& bands,
                                          const std::vector<CandidateEvidence>& evidence,
                                          const std::map<ViewRole, RegionMap>& regions,
                                          const std::vector<UserDecision>& decisions);

}  // namespace pixel3d
