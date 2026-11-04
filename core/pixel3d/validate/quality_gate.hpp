#pragma once
// Quality gate (spec §7): FAIL on any critical check, otherwise NEEDS_REVIEW on
// any blocking conflict (or when the GLB round trip was not run), otherwise PASS. Every check is reported with its value
// and threshold so a PASS is auditable.
#include <string>
#include <vector>

#include "pixel3d/analysis/conflicts.hpp"
#include "pixel3d/validate/metrics.hpp"

namespace pixel3d {

enum class ValidationStatus { Pass, NeedsReview, Fail, NotValidated };
std::string_view to_string(ValidationStatus s);  // "PASS", "NEEDS_REVIEW", "FAIL", "NOT_VALIDATED"

enum class ToleranceProfile { Strict, Hand };
std::string_view to_string(ToleranceProfile p);

struct Check {
  std::string name;
  bool critical = false;
  bool passed = false;
  double value = 0;
  double threshold = 0;
  std::string text;
};

struct GateConfig {
  ToleranceProfile profile = ToleranceProfile::Hand;
  double strict_iou = 0.98;
  double hand_rot_iou = 0.95;
  double hand_mismatch_frac = 0.02;
  int hand_mismatch_min = 3;
  double thickness_min = 0.2;
  double turntable_area_min = 0.25;
  double color_exact_min = 0.98;  // non-critical
  // SINGLE VIEW: the depth is chosen, not observed, so thickness and turntable
  // checks are informational (a Flat sprite is intended).
  bool single_view = false;
  // The user edited the geometry: a projection that no longer matches its
  // drawing is a deliberate deviation to review, not a broken model (the
  // pipeline adds a blocking edit.projection_deviation conflict).
  bool edited = false;
};

struct GateResult {
  ValidationStatus status = ValidationStatus::NotValidated;
  ToleranceProfile profile = ToleranceProfile::Hand;  // the profile the checks used
  std::vector<Check> checks;
};

GateResult run_quality_gate(const QualityMetrics& m, const std::vector<Conflict>& conflicts, const GateConfig& cfg);

}  // namespace pixel3d
