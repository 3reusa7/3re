#include "pixel3d/style/style_settings.hpp"

#include <cmath>

namespace pixel3d {
namespace {

// The presets as measured on fixtures A–D (docs/superpowers/plans/2026-10-12-pixel3d-polished-styles-decisions.md).
StyleParams preset_params(StylePreset p) {
  switch (p) {
    case StylePreset::Crisp: return StyleParams{};
    case StylePreset::Soft: return StyleParams{1.5, 2, 0.2, 40, 2.0, true, true};
    case StylePreset::Lowpoly: return StyleParams{1.0, 1, 0.2, 0, 0, false, true};
    case StylePreset::Clay: return StyleParams{1.5, 2, 0.2, 40, 3.0, true, true};
  }
  return StyleParams{};
}

Message bad(const std::string& field, const std::string& text) {
  return Message{"input.invalid", "style " + field + " " + text, Severity::Error, {{"field", field}}};
}

bool in(std::optional<double> v, double lo, double hi) { return !v || (std::isfinite(*v) && *v >= lo && *v <= hi); }

}  // namespace

std::string_view to_string(StylePreset s) {
  switch (s) {
    case StylePreset::Crisp: return "crisp";
    case StylePreset::Soft: return "soft";
    case StylePreset::Lowpoly: return "lowpoly";
    case StylePreset::Clay: return "clay";
  }
  return "crisp";
}

std::optional<StylePreset> style_preset_from_string(std::string_view s) {
  for (StylePreset p : {StylePreset::Crisp, StylePreset::Soft, StylePreset::Lowpoly, StylePreset::Clay})
    if (to_string(p) == s) return p;
  return std::nullopt;
}

StyleParams style_params(const StyleSettings& s) {
  StyleParams p = preset_params(s.preset);
  if (s.preset == StylePreset::Crisp) return p;  // crisp has nothing to tune
  if (s.radius) p.radius = *s.radius;
  if (s.exponent) p.exponent = *s.exponent;
  if (s.bevel) p.bevel = *s.bevel;
  if (s.smooth_angle) p.smooth_angle = *s.smooth_angle;
  if (s.shade_radius) p.shade_radius = *s.shade_radius;
  if (s.ao) p.ao = *s.ao;
  if (s.symmetry) p.symmetry = *s.symmetry;
  return p;
}

std::optional<Message> check_style_settings(const StyleSettings& s) {
  if (!in(s.radius, 0, 8)) return bad("radius", "must be between 0 and 8 px");
  if (!in(s.exponent, 1, 8)) return bad("exponent", "must be between 1 and 8");
  if (!in(s.bevel, 0, 2)) return bad("bevel", "must be between 0 and 2 px");
  if (!in(s.smooth_angle, 0, 90)) return bad("smooth_angle", "must be between 0 and 90 degrees");
  if (!in(s.shade_radius, 0, 8)) return bad("shade_radius", "must be between 0 and 8 px");
  for (const FamilyMaterial& m : s.materials) {
    if (m.family < 0 || m.family > 4096) return bad("materials.family", "must be a material family index (0..4096)");
    if (!in(m.roughness, 0, 1)) return bad("materials.roughness", "must be between 0 and 1");
    if (!in(m.metallic, 0, 1)) return bad("materials.metallic", "must be between 0 and 1");
    if (!in(m.emissive, 0, 100)) return bad("materials.emissive", "must be between 0 and 100");
  }
  for (size_t i = 0; i < s.materials.size(); ++i)
    for (size_t k = i + 1; k < s.materials.size(); ++k)
      if (s.materials[i].family == s.materials[k].family) return bad("materials", "names a family twice");
  return std::nullopt;
}

nlohmann::json to_json(const StyleSettings& s) {
  nlohmann::json j{{"preset", to_string(s.preset)}};
  auto opt = [&](const char* k, const auto& v) {
    if (v) j[k] = *v;
  };
  opt("radius", s.radius);
  opt("exponent", s.exponent);
  opt("bevel", s.bevel);
  opt("smooth_angle", s.smooth_angle);
  opt("shade_radius", s.shade_radius);
  opt("ao", s.ao);
  opt("symmetry", s.symmetry);
  if (!s.materials.empty()) {
    j["materials"] = nlohmann::json::array();
    for (const FamilyMaterial& m : s.materials) {
      nlohmann::json mj{{"family", m.family}};
      if (m.roughness) mj["roughness"] = *m.roughness;
      if (m.metallic) mj["metallic"] = *m.metallic;
      if (m.emissive) mj["emissive"] = *m.emissive;
      j["materials"].push_back(std::move(mj));
    }
  }
  return j;
}

Expected<StyleSettings> style_settings_from_json(const nlohmann::json& j) {
  if (!j.is_object()) return make_error("input.invalid", "style must be an object");
  StyleSettings s;
  try {
    if (j.contains("preset")) {
      const auto p = style_preset_from_string(j["preset"].get<std::string>());
      if (!p) return make_error("input.invalid", "unknown style " + j["preset"].dump() + " (crisp, soft, lowpoly, clay)");
      s.preset = *p;
    }
    auto num = [&](const char* k, std::optional<double>& v) {
      if (j.contains(k)) v = j[k].get<double>();
    };
    num("radius", s.radius);
    num("exponent", s.exponent);
    num("bevel", s.bevel);
    num("smooth_angle", s.smooth_angle);
    num("shade_radius", s.shade_radius);
    if (j.contains("ao")) s.ao = j["ao"].get<bool>();
    if (j.contains("symmetry")) s.symmetry = j["symmetry"].get<bool>();
    for (const auto& mj : j.value("materials", nlohmann::json::array())) {
      FamilyMaterial m;
      m.family = mj.at("family").get<int>();
      if (mj.contains("roughness")) m.roughness = mj["roughness"].get<double>();
      if (mj.contains("metallic")) m.metallic = mj["metallic"].get<double>();
      if (mj.contains("emissive")) m.emissive = mj["emissive"].get<double>();
      s.materials.push_back(m);
    }
  } catch (const nlohmann::json::exception& e) {
    return make_error("input.invalid", std::string("style: ") + e.what());
  }
  if (auto e = check_style_settings(s)) return Unexpected{*e};
  return s;
}

}  // namespace pixel3d
