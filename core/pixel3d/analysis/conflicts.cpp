#include "pixel3d/analysis/conflicts.hpp"

namespace pixel3d {

void to_json(nlohmann::json& j, const Conflict& c) {
  j = nlohmann::json{{"kind", c.kind},
                     {"severity", to_string(c.severity)},
                     {"blocks_pass", c.blocks_pass},
                     {"text", c.text}};
  if (!c.where.is_null()) j["where"] = c.where;
}

std::vector<Conflict> collect_conflicts(const AlignResult& align, const CorrespondenceResult& corr,
                                        const CandidateSet& cands, const std::vector<RecessFinding>& recesses,
                                        const TextureResult* texture,
                                        const std::map<ViewRole, int>& silhouette_mismatch_px) {
  std::vector<Conflict> out;
  for (const Message& m : align.messages)
    if (m.code == "extent_mismatch")
      out.push_back(Conflict{"extent_mismatch", Severity::Error, true, m.text, m.where});

  const auto& mismatch = silhouette_mismatch_px.empty() ? align.profile_conflicts : silhouette_mismatch_px;
  for (const auto& [view, n] : mismatch)
    if (n > 0)
      out.push_back(Conflict{"silhouette_conflict", Severity::Warning, false,
                             std::to_string(n) + " drawn pixel(s) cannot be explained by any volume consistent with the other views",
                             {{"view", to_string(view)}, {"pixels", n}}});

  for (const Ambiguity& a : corr.ambiguities) {
    const bool blocking =
        a.kind == "ambiguous_correspondence" || a.kind == "evidence_conflict" || a.kind == "edit.uncovers_atom";
    nlohmann::json where{{"band", a.band}, {"candidates", a.candidates}};
    if (!a.alternative_covers.empty()) where["alternatives"] = a.alternative_covers;
    out.push_back(Conflict{a.kind, blocking ? Severity::Warning : Severity::Info, blocking, a.text, where});
  }

  // Inferred-by-prior candidates (info): one entry per kind listing the candidates.
  const std::pair<CandStatus, const char*> inferred[] = {{CandStatus::InferredGrid, "inferred_grid"},
                                                         {CandStatus::InferredContinuation, "inferred_continuation"},
                                                         {CandStatus::InferredFill, "inferred_fill"}};
  for (const auto& [status, kind] : inferred) {
    std::vector<int> ids;
    double volume = 0;
    for (const Candidate& c : cands.cands)
      if (c.status == status) ids.push_back(c.id), volume += c.volume;
    if (ids.empty()) continue;
    out.push_back(Conflict{kind, Severity::Info, false, cands.cands[ids.front()].reason,
                           {{"candidates", ids}, {"volume", volume}}});
  }

  for (const RecessFinding& r : recesses) {
    const bool recess = r.kind == "possible_recess";
    const char* text = recess ? "an enclosed darker region may be a recess or hole of unknown depth (use set_depth)"
                       : r.kind == "user_depth" ? "an enclosed region's depth was decided by a depth constraint"
                                                : "an enclosed painted region is assumed flush with its surroundings";
    out.push_back(Conflict{r.kind, recess ? Severity::Warning : Severity::Info, recess, text,
                           {{"view", to_string(r.view)},
                            {"region", r.region},
                            {"area_px", r.area},
                            {"luminance_ratio", r.luminance_ratio},
                            {"column_volume", r.column_volume},
                            {"box", {r.box.x0, r.box.y0, r.box.x1, r.box.y1}}}});
  }

  if (texture)
    for (const Message& m : texture->messages)
      out.push_back(Conflict{m.code, m.severity, false, m.text, m.where});
  return out;
}

}  // namespace pixel3d
