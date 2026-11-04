#pragma once
// Bounded recovery controller (PLAN.md, component 6). Pure: it only decides; the runner acts.
// Every decision carries the rule that fired and the reason. Each rule has its own cap, the
// whole run has a time budget and a decision cap, so it can never loop.
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "agent_spec.hpp"

namespace p3dagent {

enum class Action {
  Accept,             // nothing to recover
  Decimate,           // guarded QEM to target_triangles
  DecimateAllowTouching,  // the strict guard stopped above the budget: once more, accepting new touching
                          // (point/edge) contacts the audit rates REVIEW, still refusing every UNSAFE one
  DecimateGentler,    // guarded QEM to a gentler target (more triangles), then re-texture
  LowerTextureSize,   // re-run the texture worker with texture_size
  RetryAfterWait,     // generation ran out of memory: wait for free RAM once, then retry
  RerunCleanup,       // saved run cleaned by an older cleanup: run the pinned GeometrySafe on raw.ply
  Regenerate,         // UNSAFE geometry: generate again with seed
  AlternativeTexture, // texture REVIEW: try the other texture candidate (picture-only vs hybrid)
  HumanReview,        // stop recovering this problem; the report lists it for a person
  Stop                // the run cannot continue (time budget, decision cap, nothing left to try)
};
const char* to_string(Action a);

struct Decision {
  Action action = Action::Accept;
  std::string rule, reason;
  int target_triangles = 0, texture_size = 0;
  uint32_t seed = 0;
  int attempt = 0;
  nlohmann::json to_json() const;
};

// PASS 0, REVIEW 1, FAIL/UNSAFE 2, anything else (unknown, missing) 3.
int verdict_rank(const std::string& verdict);

class RecoveryController {
 public:
  explicit RecoveryController(const AssetSpec& spec, int max_decisions = 12);
  void set_elapsed(double seconds) { elapsed_ = seconds; }
  // The mesh has `triangles`; `decimated` = a decimation already ran and stopped above the budget
  // (first time: one touching-allowed pass, then a person).
  Decision on_over_budget(int triangles, bool decimated);
  // Texture verdict before (the full mesh) vs after decimation + re-texture.
  // `achieved` = triangles the decimation actually reached (0 = unknown): no gentler step when it already
  // stopped at or above the gentler target.
  Decision on_texture_compared(const std::string& before, const std::string& after, int original_triangles, int achieved = 0);
  Decision on_out_of_memory(const std::string& stage, int texture_size);
  Decision on_unsafe(bool cleanup_outdated, bool raw_available, bool can_regenerate, uint32_t current_seed);
  Decision on_texture_review(bool alternative_available);
  const std::vector<Decision>& history() const { return history_; }

 private:
  Decision decide(Decision d);
  bool guard(Decision& d);  // time budget and decision cap
  const AssetSpec& spec_;
  int max_decisions_;
  double elapsed_ = 0;
  int decimations_ = 0, touching_ = 0, gentler_ = 0, texture_lowered_ = 0, generate_waits_ = 0, cleanup_reruns_ = 0,
      regenerations_ = 0, alternatives_ = 0;
  std::vector<Decision> history_;
};

}  // namespace p3dagent
