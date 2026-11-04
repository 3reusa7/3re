#pragma once
#include <nlohmann/json.hpp>

#include "pixel3d/analysis/conflicts.hpp"
#include "pixel3d/validate/quality_gate.hpp"

namespace pixel3d {

// Report schema "pixel3d.report/1": schema, status, profile, inputs, parts,
// conflicts, metrics, checks.
nlohmann::json make_report(const GateResult& gate, const QualityMetrics& m, const std::vector<Conflict>& conflicts,
                           const CorrespondenceResult& corr, const nlohmann::json& inputs,
                           ToleranceProfile profile = ToleranceProfile::Hand);

nlohmann::json metrics_json(const QualityMetrics& m);

}  // namespace pixel3d
