#include "agent_recovery.hpp"

#include <cmath>

namespace p3dagent {

const char* to_string(Action a) {
  switch (a) {
    case Action::Accept: return "Accept";
    case Action::Decimate: return "Decimate";
    case Action::DecimateAllowTouching: return "DecimateAllowTouching";
    case Action::DecimateGentler: return "DecimateGentler";
    case Action::LowerTextureSize: return "LowerTextureSize";
    case Action::RetryAfterWait: return "RetryAfterWait";
    case Action::RerunCleanup: return "RerunCleanup";
    case Action::Regenerate: return "Regenerate";
    case Action::AlternativeTexture: return "AlternativeTexture";
    case Action::HumanReview: return "HumanReview";
    case Action::Stop: return "Stop";
  }
  return "?";
}

nlohmann::json Decision::to_json() const {
  nlohmann::json j = {{"action", p3dagent::to_string(action)}, {"rule", rule}, {"reason", reason}, {"attempt", attempt}};
  if (target_triangles) j["target_triangles"] = target_triangles;
  if (texture_size) j["texture_size"] = texture_size;
  if (action == Action::Regenerate) j["seed"] = seed;
  return j;
}

int verdict_rank(const std::string& v) {
  if (v == "PASS") return 0;
  if (v == "REVIEW") return 1;
  if (v == "FAIL" || v == "UNSAFE") return 2;
  return 3;
}

RecoveryController::RecoveryController(const AssetSpec& spec, int max_decisions) : spec_(spec), max_decisions_(max_decisions) {}

bool RecoveryController::guard(Decision& d) {
  if (elapsed_ > spec_.limits.total_seconds) {
    d = {Action::Stop, "time_budget",
         "time budget of " + std::to_string(int(spec_.limits.total_seconds)) + " s exhausted after " +
             std::to_string(int(elapsed_)) + " s: no further recovery"};
    return false;
  }
  if (int(history_.size()) >= max_decisions_) {
    d = {Action::Stop, "decision_cap", "decision cap of " + std::to_string(max_decisions_) + " reached: no further recovery"};
    return false;
  }
  return true;
}

Decision RecoveryController::decide(Decision d) {
  Decision stop;
  if (!guard(stop)) d = stop;
  history_.push_back(d);
  return d;
}

Decision RecoveryController::on_over_budget(int triangles, bool decimated) {
  const int budget = spec_.triangle_budget;
  if (triangles <= budget) return decide({Action::Accept, "over_budget", std::to_string(triangles) + " <= budget " + std::to_string(budget)});
  if ((decimated || decimations_ >= 1) && touching_ == 0) {
    ++touching_;
    Decision d{Action::DecimateAllowTouching, "over_budget",
               "the strict guard stopped at " + std::to_string(triangles) + " triangles (budget " + std::to_string(budget) +
                   "): one more pass accepting new touching contacts (audit REVIEW), still refusing crossings (UNSAFE)"};
    d.target_triangles = budget;
    d.attempt = 1;
    return decide(d);
  }
  if (decimated || decimations_ >= 1)
    return decide({Action::HumanReview, "over_budget",
                   "the guarded decimation stopped at " + std::to_string(triangles) + " triangles, above the budget of " +
                       std::to_string(budget) + ": every further collapse would make the surface cross itself; the mesh is "
                       "kept at this count (never forced into self-intersections)"});
  ++decimations_;
  Decision d{Action::Decimate, "over_budget",
             std::to_string(triangles) + " triangles > budget " + std::to_string(budget) +
                 ": guarded QEM to the budget (no new self-contact), then re-texture from the picture + TRELLIS texture"};
  d.target_triangles = budget;
  d.attempt = decimations_;
  return decide(d);
}

Decision RecoveryController::on_texture_compared(const std::string& before, const std::string& after, int original, int achieved) {
  if (verdict_rank(after) <= verdict_rank(before))
    return decide({Action::Accept, "texture_after_decimation",
                   "texture verdict after decimation " + after + " is not worse than before (" + before + ")"});
  const int gentle = int(std::ceil(spec_.triangle_budget * 1.5));
  if (gentler_ >= 1 || gentle >= original || (achieved > 0 && achieved >= gentle))
    return decide({Action::HumanReview, "texture_after_decimation",
                   "texture verdict got worse after decimation (" + before + " -> " + after + ")" +
                       (gentler_ ? " also at the gentler target"
                                 : achieved >= gentle ? ": the decimation already stopped above the gentler target"
                                                      : ": no gentler target below the original exists") +
                       "; a person decides between budget and texture"});
  ++gentler_;
  Decision d{Action::DecimateGentler, "texture_after_decimation",
             "texture verdict got worse after decimation (" + before + " -> " + after + "): one gentler target " +
                 std::to_string(gentle) + " (1.5x the budget; the budget is then not met)"};
  d.target_triangles = gentle;
  d.attempt = gentler_;
  return decide(d);
}

Decision RecoveryController::on_out_of_memory(const std::string& stage, int texture_size) {
  if (stage == "generate") {
    if (generate_waits_ >= 1)
      return decide({Action::Stop, "out_of_memory", "generation ran out of memory twice: the machine needs more free RAM/VRAM"});
    ++generate_waits_;
    Decision d{Action::RetryAfterWait, "out_of_memory",
               "generation ran out of memory: wait until the wizard's admission RAM is free, then retry once "
               "(the wizard itself already falls back to geometry only when the texture stage fails)"};
    d.attempt = generate_waits_;
    return decide(d);
  }
  if (texture_size <= 1024 || texture_lowered_ >= 2)
    return decide({Action::HumanReview, "out_of_memory",
                   stage + " ran out of memory at the lowest setting (" + std::to_string(texture_size) + ")"});
  ++texture_lowered_;
  Decision d{Action::LowerTextureSize, "out_of_memory",
             stage + " ran out of memory at " + std::to_string(texture_size) + ": lower setting " +
                 std::to_string(texture_size / 2)};
  d.texture_size = texture_size / 2;
  d.attempt = texture_lowered_;
  return decide(d);
}

Decision RecoveryController::on_unsafe(bool cleanup_outdated, bool raw_available, bool can_regenerate, uint32_t seed) {
  if (cleanup_outdated && raw_available && cleanup_reruns_ == 0) {
    ++cleanup_reruns_;
    Decision d{Action::RerunCleanup, "unsafe_geometry",
               "UNSAFE from an older cleanup: run the pinned GeometrySafe on the run's raw.ply first (same generation)"};
    d.attempt = 1;
    return decide(d);
  }
  if (can_regenerate && regenerations_ < spec_.limits.max_regenerations) {
    ++regenerations_;
    Decision d{Action::Regenerate, "unsafe_geometry",
               "geometry is UNSAFE: generate again with seed " + std::to_string(seed + 1) + " (attempt " +
                   std::to_string(regenerations_) + " of " + std::to_string(spec_.limits.max_regenerations) + ")"};
    d.seed = seed + 1;
    d.attempt = regenerations_;
    return decide(d);
  }
  return decide({Action::HumanReview, "unsafe_geometry",
                 std::string("geometry stays UNSAFE: the mesh is kept as generated and reported (never forced SAFE); ") +
                     (can_regenerate ? "regenerations used up" : "no picture/GPU to regenerate")});
}

Decision RecoveryController::on_texture_review(bool alternative_available) {
  if (alternative_available && alternatives_ == 0) {
    ++alternatives_;
    Decision d{Action::AlternativeTexture, "texture_review",
               "texture REVIEW: score the other candidate (picture-only vs hybrid) with the same gate and keep the better"};
    d.attempt = 1;
    return decide(d);
  }
  return decide({Action::HumanReview, "texture_review",
                 alternative_available ? "texture still REVIEW after the alternative candidate: a person looks at it"
                                       : "texture REVIEW and no alternative candidate: a person looks at it"});
}

}  // namespace p3dagent
