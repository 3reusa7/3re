#include "agent_select.hpp"

#include <cstdio>
#include <limits>

#include "agent_recovery.hpp"

namespace p3dagent {

using nlohmann::json;

namespace {
std::string fmt(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.4f", v);
  return b;
}
}  // namespace

Choice choose_texture_candidate(const std::vector<TextureCandidate>& c, double margin) {
  if (c.empty()) return {SIZE_MAX, "no texture candidate"};
  size_t own = 0;
  for (size_t i = 0; i < c.size(); ++i)
    if (c[i].own) own = i;
  int best_rank = 99;
  for (const auto& k : c) best_rank = std::min(best_rank, verdict_rank(k.verdict));
  // Best agreement among the best verdict.
  size_t best = SIZE_MAX;
  for (size_t i = 0; i < c.size(); ++i) {
    if (verdict_rank(c[i].verdict) != best_rank) continue;
    if (best == SIZE_MAX) best = i;
    else {
      const double a = c[i].agreement_de, b = c[best].agreement_de;
      if (a >= 0 && (b < 0 || a < b)) best = i;
    }
  }
  if (verdict_rank(c[own].verdict) == best_rank) {
    const double a = c[best].agreement_de, o = c[own].agreement_de;
    if (best == own || a < 0 || o < 0 || o - a <= margin)
      return {own, "kept the run's own texture (" + c[own].verdict + ", seen-side agreement dE " + fmt(o) +
                       "): no candidate with the same verdict agrees with the picture better by more than " + fmt(margin)};
    return {best, "chose " + c[best].name + " (" + c[best].verdict + "): its TRELLIS colours agree with the picture on the seen side at dE " +
                      fmt(a) + " vs " + fmt(o) + " for the run's own"};
  }
  return {best, "chose " + c[best].name + ": verdict " + c[best].verdict + " beats the run's own " + c[own].verdict +
                    " (seen-side agreement dE " + fmt(c[best].agreement_de) + ")"};
}

double seen_side_agreement(const json& r) {
  if (!r.is_object() || !r.contains("attempts") || !r["attempts"].is_array()) return -1;
  for (const auto& a : r["attempts"]) {
    if (!a.is_object() || !a.contains("bake") || !a["bake"].is_object() || !a["bake"].contains("native")) continue;
    const auto& n = a["bake"]["native"];
    if (!n.is_object() || !n.contains("calibration") || !n["calibration"].is_object()) continue;
    const auto& cal = n["calibration"];
    const auto method = cal.value("method", std::string());
    if (method.find("lightness_only") != std::string::npos) return cal.value("uncalibrated_heldout_delta_e", -1.0);
    for (const auto& t : cal.value("tried", json::array()))
      if (t.is_object() && t.value("method", std::string()) == method && t.contains("heldout_delta_e") && t["heldout_delta_e"].is_number())
        return t["heldout_delta_e"].get<double>();
    return cal.value("uncalibrated_heldout_delta_e", -1.0);
  }
  return -1;
}

Choice choose_decimation(const std::vector<DecimationCandidate>& c, Intersections mode) {
  auto usable = [&](const DecimationCandidate& k) { return k.ran && (mode == Intersections::Report || k.crossings == 0); };
  for (size_t i = 0; i < c.size(); ++i)
    if (usable(c[i]) && c[i].budget_met && c[i].fidelity_pass)
      return {i, c[i].name + ": " + std::to_string(c[i].triangles) + " triangles, within the budget and the fidelity limits" +
                     (c[i].crossings > 0 ? " (" + std::to_string(c[i].crossings) + " self-crossings, reported)" : std::string())};
  size_t best = SIZE_MAX;
  for (size_t i = 0; i < c.size(); ++i)
    if (usable(c[i]) && c[i].budget_met && (best == SIZE_MAX || c[i].distance_p95_pct < c[best].distance_p95_pct)) best = i;
  if (best != SIZE_MAX)
    return {best, c[best].name + ": within the budget but outside the fidelity limits (the most faithful candidate that meets the budget, "
                                 "distance p95 " + fmt(c[best].distance_p95_pct) + " %); a person should look at it"};
  for (size_t i = 0; i < c.size(); ++i)
    if (usable(c[i])) return {i, c[i].name + ": no candidate reached the budget" + std::string(mode == Intersections::Block ? " without new crossings" : "")};
  return {SIZE_MAX, "no decimation candidate produced a mesh"};
}

std::string game_geometry_verdict(const json& t, const json& x, json& reported) {
  reported = json::object();
  if (!t.is_object() || !t.value("complete", false) || t.value("triangles", int64_t(0)) <= 0) return "";
  for (const char* k : {"overfull_edges", "invalid_vertex_links", "inconsistent_winding_edges", "duplicate_faces", "internal_faces", "zero_area_faces"}) {
    if (!t.contains(k) || !t[k].is_number_integer()) return "";
    reported[k] = t[k];
  }
  const auto pc = x.is_object() ? x.value("pair_classification", json::object()) : json::object();
  const auto st = x.is_object() ? x.value("edge_star_classification", json::object()) : json::object();
  for (const char* k : {"transverse_crossing", "coplanar_area_overlap", "edge_face_contact", "point_contact", "edge_contact"})
    reported[k] = pc.contains(k) ? pc[k] : json(nullptr);
  reported["crossing_sheet_stars"] = st.contains("crossing_sheet_stars") ? st["crossing_sheet_stars"] : json(nullptr);
  reported["boundary_edges"] = t.value("boundary_edges", int64_t(0));
  for (const char* k : {"overfull_edges", "invalid_vertex_links", "inconsistent_winding_edges", "duplicate_faces", "internal_faces", "zero_area_faces"})
    if (t[k].get<int64_t>() > 0) return "UNSAFE";
  return t.value("boundary_edges", int64_t(0)) > 0 ? "REVIEW" : "PASS";
}

}  // namespace p3dagent
