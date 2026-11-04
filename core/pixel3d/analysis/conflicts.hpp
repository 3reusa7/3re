#pragma once
// Aggregates everything that is not fully determined by the drawings into one
// list (spec §6.1). `blocks_pass` marks kinds that keep the status from PASS.
#include <map>
#include <string>
#include <vector>

#include "pixel3d/analysis/recess.hpp"
#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/correspond/solve.hpp"
#include "pixel3d/texture/projector.hpp"
#include "pixel3d/views/align.hpp"

namespace pixel3d {

struct Conflict {
  std::string kind;
  Severity severity = Severity::Info;
  bool blocks_pass = false;
  std::string text;
  nlohmann::json where;
};

void to_json(nlohmann::json& j, const Conflict& c);

// silhouette_mismatch_px: per-view re-render mismatch when known; otherwise the
// alignment's unexplainable-pixel counts are reported.
std::vector<Conflict> collect_conflicts(const AlignResult& align, const CorrespondenceResult& corr,
                                        const CandidateSet& cands, const std::vector<RecessFinding>& recesses,
                                        const TextureResult* texture,
                                        const std::map<ViewRole, int>& silhouette_mismatch_px);

}  // namespace pixel3d
