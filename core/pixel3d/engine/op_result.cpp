#include "pixel3d/engine/op_result.hpp"

namespace pixel3d {

void OpResult::add(const Message& m) {
  switch (m.severity) {
    case Severity::Error: errors.push_back(m); break;
    case Severity::Warning: warnings.push_back(m); break;
    case Severity::Info: info.push_back(m); break;
  }
}

OpResult OpResult::fail(Message m) {
  OpResult r;
  m.severity = Severity::Error;
  r.errors.push_back(std::move(m));
  return r;
}

nlohmann::json to_json(const OpResult& r) {
  return {{"success", r.success},
          {"errors", r.errors},
          {"warnings", r.warnings},
          {"info", r.info},
          {"changed", r.changed},
          {"changed_ids", r.changed_ids.is_null() ? nlohmann::json::object() : r.changed_ids},
          {"revision", r.revision},
          {"stats",
           {{"triangles", r.stats.triangles},
            {"vertices", r.stats.vertices},
            {"parts", r.stats.parts},
            {"volume", r.stats.volume},
            {"watertight", r.stats.watertight}}},
          {"metrics", r.metrics},
          {"status", std::string(to_string(r.status))},
          {"validation", r.validation},
          {"data", r.data}};
}

}  // namespace pixel3d
