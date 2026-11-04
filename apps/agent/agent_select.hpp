#pragma once
// Agent milestone 2 choices between candidates (pure; the runner produces the candidates and logs every one).
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_spec.hpp"

namespace p3dagent {

struct Choice {
  size_t index = 0;
  std::string reason;
};

// B2 — TRELLIS texture candidates (the run's own texture + texture-only replays with other seeds), each
// re-textured by the worker. The texture verdict decides first; between equal verdicts, the candidate whose own
// TRELLIS colours agree best with the picture where the picture sees the surface (held-out dE after calibration):
// a texture model that paints the seen side like the picture is the most likely to paint the hidden side right
// (measured on the benchmark, docs/agent/TEXTURE-CANDIDATES.md). The run's own texture is kept unless another
// beats it by more than `margin`.
struct TextureCandidate {
  std::string name;
  std::string verdict;          // PASS / REVIEW / FAIL / "" (not measured)
  double agreement_de = -1;     // seen-side agreement, lower is better; < 0 = not measured
  bool own = false;             // the run's own texture
};
Choice choose_texture_candidate(const std::vector<TextureCandidate>& c, double margin = 0.005);
// Seen-side agreement from a texture worker report (.texture.json): the chosen calibration's held-out dE, or the
// uncalibrated one when the calibration guard fell back to lightness only (the pairs disagree). < 0 = not there.
double seen_side_agreement(const nlohmann::json& texture_report);

// A2 — decimation candidates in the order tried (guarded QEM, QEM without the crossing guard, ...).
struct DecimationCandidate {
  std::string name;
  bool ran = false;
  int64_t triangles = 0;
  bool budget_met = false;
  bool fidelity_pass = false;
  double distance_p95_pct = 1e9;
  int64_t crossings = -1;       // transverse crossings (exact audit), -1 = not measured
};
// The first that meets budget AND fidelity. Otherwise the most faithful one that meets the budget (its fidelity
// failure is reported), otherwise the first that ran. With Intersections::Block a candidate with crossings (or
// unmeasured crossings) is never chosen. index == SIZE_MAX: nothing usable.
Choice choose_decimation(const std::vector<DecimationCandidate>& c, Intersections mode);

// A1 — geometry verdict for a game target: the file must be sound (no overfull edges, invalid vertex links,
// inconsistent winding, duplicate / internal / zero-area faces -> UNSAFE; open boundary -> REVIEW); self-crossings
// and contacts are counted and reported, never part of the verdict. Inputs: pixel3d-asset-inspect topology.json and
// pixel3d-geometry-audit intersections.json. "" = not measurable.
std::string game_geometry_verdict(const nlohmann::json& topology, const nlohmann::json& intersections, nlohmann::json& reported);

}  // namespace p3dagent
