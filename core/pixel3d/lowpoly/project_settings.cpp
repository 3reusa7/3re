#include "pixel3d/lowpoly/project_settings.hpp"

#include <cmath>

namespace pixel3d {
namespace {

Message bad(const std::string& field, const std::string& text) {
  return Message{"input.invalid", "lowpoly " + field + " " + text, Severity::Error, {{"field", field}}};
}

bool finite_in(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

}  // namespace

std::optional<Message> check_lowpoly_settings(const LowpolyProjectSettings& s) {
  const lowpoly::Settings& b = s.build;
  if (b.tri_budget < lowpoly::kMinBudget || b.tri_budget > lowpoly::kMaxBudget)
    return bad("tri_budget", "must be between " + std::to_string(lowpoly::kMinBudget) + " and " +
                                 std::to_string(lowpoly::kMaxBudget) + " triangles");
  if (b.joint_loops < 1 || b.joint_loops > 3) return bad("joint_loops", "must be 1, 2 or 3");
  if (b.tones < 2 || b.tones > 3) return bad("tones", "must be 2 or 3");
  if (!finite_in(b.depth, 0, 4096)) return bad("depth", "must be between 0 (automatic) and 4096 source pixels");
  if (!finite_in(b.hull_width, 0.05, 8)) return bad("hull_width", "must be between 0.05 and 8 source pixels");
  if (b.min_sides < 3 || b.max_sides > 16 || b.min_sides > b.max_sides)
    return bad("sides", "must satisfy 3 <= min_sides <= max_sides <= 16");
  if (b.atlas_max < 64 || b.atlas_max > 4096) return bad("atlas_max", "must be between 64 and 4096");
  for (const auto& [id, r] : s.depth_ratio)
    if (!finite_in(r, 0.05, 2)) return bad("depth_ratio", "of part " + std::to_string(id) + " must be between 0.05 and 2");
  return std::nullopt;
}

nlohmann::json to_json(const LowpolyProjectSettings& s) {
  const lowpoly::Settings& b = s.build;
  nlohmann::json j{{"enabled", s.enabled},
                   {"tri_budget", b.tri_budget},
                   {"joint_loops", b.joint_loops},
                   {"style", lowpoly::to_string(b.style)},
                   {"tones", b.tones},
                   {"template", lowpoly::to_string(b.templ)},
                   {"baked_cel", b.baked_cel},
                   {"hull", b.hull},
                   {"hull_width", b.hull_width},
                   {"depth", b.depth},
                   {"user_depth", b.user_depth},
                   {"accept_inferred_depth", b.accept_inferred_depth},
                   {"min_sides", b.min_sides},
                   {"max_sides", b.max_sides},
                   {"opposite_projection_head_cloth", b.opposite_projection_head_cloth},
                   {"atlas_max", b.atlas_max}};
  j["depth_ratio"] = nlohmann::json::object();
  for (const auto& [id, r] : s.depth_ratio) j["depth_ratio"][std::to_string(id)] = r;
  return j;
}

Expected<LowpolyProjectSettings> lowpoly_settings_from_json(const nlohmann::json& j) {
  if (!j.is_object()) return make_error("input.invalid", "lowpoly settings must be an object");
  LowpolyProjectSettings s;
  lowpoly::Settings& b = s.build;
  try {
    s.enabled = j.value("enabled", s.enabled);
    b.tri_budget = j.value("tri_budget", b.tri_budget);
    b.joint_loops = j.value("joint_loops", b.joint_loops);
    if (j.contains("style")) {
      const auto v = lowpoly::style_from_string(j["style"].get<std::string>());
      if (!v) return Unexpected{bad("style", "must be flat, cel, outline or cel_outline")};
      b.style = *v;
    }
    b.tones = j.value("tones", b.tones);
    if (j.contains("template")) {
      const auto v = lowpoly::template_from_string(j["template"].get<std::string>());
      if (!v) return Unexpected{bad("template", "must be auto, cloaked_character, humanoid or generic_chain")};
      b.templ = *v;
    }
    b.baked_cel = j.value("baked_cel", b.baked_cel);
    b.hull = j.value("hull", b.hull);
    b.hull_width = j.value("hull_width", b.hull_width);
    b.depth = j.value("depth", b.depth);
    b.user_depth = j.value("user_depth", b.user_depth);
    b.accept_inferred_depth = j.value("accept_inferred_depth", b.accept_inferred_depth);
    b.min_sides = j.value("min_sides", b.min_sides);
    b.max_sides = j.value("max_sides", b.max_sides);
    b.opposite_projection_head_cloth = j.value("opposite_projection_head_cloth", b.opposite_projection_head_cloth);
    b.atlas_max = j.value("atlas_max", b.atlas_max);
    const nlohmann::json ratios = j.value("depth_ratio", nlohmann::json::object());  // keep alive while iterating
    if (!ratios.is_object()) return Unexpected{bad("depth_ratio", "must map part ids to ratios")};
    for (const auto& [k, v] : ratios.items()) s.depth_ratio[std::stoi(k)] = v.get<double>();
  } catch (const std::exception& e) {
    return make_error("input.invalid", std::string("lowpoly settings: ") + e.what());
  }
  if (auto e = check_lowpoly_settings(s)) return Unexpected{*e};
  return s;
}

nlohmann::json part_types_to_json(const std::map<int, lowpoly::PartType>& t) {
  nlohmann::json j = nlohmann::json::object();
  for (const auto& [id, type] : t) j[std::to_string(id)] = lowpoly::to_string(type);
  return j;
}

Expected<std::map<int, lowpoly::PartType>> part_types_from_json(const nlohmann::json& j) {
  if (!j.is_object()) return make_error("input.invalid", "part types must map part ids to types");
  std::map<int, lowpoly::PartType> out;
  for (const auto& [k, v] : j.items()) {
    if (k.empty() || k.find_first_not_of("0123456789") != std::string::npos)
      return make_error("input.invalid", "part type key is not a part id: " + k);
    if (!v.is_string()) return make_error("input.invalid", "part type of part " + k + " must be a string");
    const auto t = lowpoly::part_type_from_string(v.get<std::string>());
    if (!t) return make_error("input.invalid", "unknown part type " + v.dump() + " (head, torso, limb, accessory, cloth)");
    out[std::stoi(k)] = *t;
  }
  return out;
}

}  // namespace pixel3d
