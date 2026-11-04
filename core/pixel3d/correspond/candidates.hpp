#pragma once
// Candidate 3D parts (spec §5.3): within a Y-band every (Front atom, Side atom)
// pair is a candidate whose solid is the intersection of the two atom prisms
// with the full Top (and optional view) prisms. The union of all candidates is
// the visual hull; correspondence decides which candidates are real.
#include <map>
#include <string>
#include <vector>

#include <manifold/manifold.h>

#include "pixel3d/reconstruct/prism.hpp"
#include "pixel3d/reconstruct/volume.hpp"
#include "pixel3d/segment/bands.hpp"

namespace pixel3d {

enum class CandStatus { Certain, CertainPalette, InferredContinuation, InferredGrid, InferredFill, Ambiguous, Removed };
std::string_view to_string(CandStatus s);
bool is_kept(CandStatus s);

struct Candidate {
  int id = 0;
  int band = 0;
  int front_atom = 0;
  int side_atom = 0;
  manifold::Manifold solid;  // AsOriginal(): triangles trace back via original_id
  int original_id = -1;
  double volume = 0;
  Mask2D::Box footprint;  // X × Z cells in Top image coordinates (u = x, v = z)
  CandStatus status = CandStatus::Certain;
  std::string reason;
};

struct CandidateSet {
  std::vector<Candidate> cands;
  const Candidate* by_original_id(int oid) const;
};

CandidateSet build_candidates(const BandSet& bands, const std::map<ViewRole, ViewPolygons>& polys, Dims d);

// Cells of the kept candidates (all candidates if include_removed) at pixel-centre sampling.
VoxelGrid voxelize_candidates(const CandidateSet& cs, const std::map<ViewRole, Mask2D>& masks, const BandSet& bands,
                              Dims d, bool include_removed = false);

}  // namespace pixel3d
