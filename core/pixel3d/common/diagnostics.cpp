#include "pixel3d/common/diagnostics.hpp"

namespace pixel3d {

std::string_view to_string(Severity s) {
  switch (s) {
    case Severity::Info: return "info";
    case Severity::Warning: return "warning";
    case Severity::Error: return "error";
  }
  return "error";
}

Unexpected make_error(std::string code, std::string text, nlohmann::json where) {
  return Unexpected{Message{std::move(code), std::move(text), Severity::Error, std::move(where)}};
}

void to_json(nlohmann::json& j, const Message& m) {
  j = nlohmann::json{{"code", m.code}, {"text", m.text}, {"severity", to_string(m.severity)}};
  if (!m.where.is_null()) j["where"] = m.where;
}

}  // namespace pixel3d
