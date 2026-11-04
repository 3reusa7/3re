#include "pixel3d/io/migrate.hpp"

#include <functional>
#include <vector>

namespace pixel3d {
namespace {

// steps[i] migrates version i+1 → i+2. Version 1 is the first released format.
const std::vector<std::function<nlohmann::json(nlohmann::json)>>& steps() {
  static const std::vector<std::function<nlohmann::json(nlohmann::json)>> s = {
      // 1 → 2: the editor inputs are new (absent = none); weld options never had an effect.
      [](nlohmann::json j) {
        auto upgrade = [](nlohmann::json& inputs) {
          if (!inputs.is_object()) return;
          if (inputs.contains("cleanup") && inputs["cleanup"].is_object()) {
            inputs["cleanup"].erase("weld_vertices");
            inputs["cleanup"].erase("weld_eps");
          }
          for (const char* k : {"edits"}) if (!inputs.contains(k)) inputs[k] = nlohmann::json::array();
        };
        if (j.contains("inputs")) upgrade(j["inputs"]);
        if (j.contains("checkpoints") && j["checkpoints"].is_object())
          for (auto& [name, cp] : j["checkpoints"].items()) upgrade(cp);
        return j;
      },
      // 2 → 3: input normalisation settings (absent = the defaults, normalisation on);
      // source images may be stored under their own extension ("image", absent = .png).
      [](nlohmann::json j) {
        auto upgrade = [](nlohmann::json& inputs) {
          if (!inputs.is_object()) return;
          if (!inputs.contains("settings") || !inputs["settings"].is_object()) return;
          if (!inputs["settings"].contains("normalize"))
            inputs["settings"]["normalize"] = {{"enabled", true}, {"target_size", 128}, {"bg_tolerance", 24},
                                               {"axis_tolerance", 0.15}};
        };
        if (j.contains("inputs")) upgrade(j["inputs"]);
        if (j.contains("checkpoints") && j["checkpoints"].is_object())
          for (auto& [name, cp] : j["checkpoints"].items()) upgrade(cp);
        return j;
      },
      // 3 → 4: polished styles; older projects are crisp (the model they always had).
      [](nlohmann::json j) {
        auto upgrade = [](nlohmann::json& inputs) {
          if (!inputs.is_object()) return;
          if (!inputs.contains("settings") || !inputs["settings"].is_object()) return;
          if (!inputs["settings"].contains("style")) inputs["settings"]["style"] = {{"preset", "crisp"}};
        };
        if (j.contains("inputs")) upgrade(j["inputs"]);
        if (j.contains("checkpoints") && j["checkpoints"].is_object())
          for (auto& [name, cp] : j["checkpoints"].items()) upgrade(cp);
        return j;
      },
      // 4 → 5: LOW-POLY STUDIO; older projects are standard projects without part types.
      [](nlohmann::json j) {
        auto upgrade = [](nlohmann::json& inputs) {
          if (!inputs.is_object()) return;
          if (!inputs.contains("lowpoly")) inputs["lowpoly"] = {{"enabled", false}};
          if (!inputs.contains("part_types")) inputs["part_types"] = nlohmann::json::object();
        };
        if (j.contains("inputs")) upgrade(j["inputs"]);
        if (j.contains("checkpoints") && j["checkpoints"].is_object())
          for (auto& [name, cp] : j["checkpoints"].items()) upgrade(cp);
        return j;
      },
  };
  return s;
}

}  // namespace

Expected<nlohmann::json> migrate_project_json(nlohmann::json j, int from_version) {
  if (from_version > kProjectFormatVersion)
    return make_error("project.too_new",
                      "this project was saved by a newer Pixel3D (format " + std::to_string(from_version) +
                          "); this version reads up to " + std::to_string(kProjectFormatVersion));
  if (from_version < 1) return make_error("project.invalid", "unknown project format version");
  for (int v = from_version; v < kProjectFormatVersion; ++v) j = steps().at(static_cast<size_t>(v - 1))(std::move(j));
  j["format_version"] = kProjectFormatVersion;
  return j;
}

}  // namespace pixel3d
