#include "pixel3d/lowpoly/types.hpp"

namespace pixel3d::lowpoly {

std::string_view to_string(PartType t) {
  switch (t) {
    case PartType::Head: return "head";
    case PartType::Torso: return "torso";
    case PartType::Limb: return "limb";
    case PartType::Accessory: return "accessory";
    case PartType::Cloth: return "cloth";
  }
  return "torso";
}

std::optional<PartType> part_type_from_string(std::string_view s) {
  for (PartType t : {PartType::Head, PartType::Torso, PartType::Limb, PartType::Accessory, PartType::Cloth})
    if (to_string(t) == s) return t;
  if (s == "limb_segment") return PartType::Limb;
  return std::nullopt;
}

std::string_view to_string(Template t) {
  switch (t) {
    case Template::Auto: return "auto";
    case Template::CloakedCharacter: return "cloaked_character";
    case Template::Humanoid: return "humanoid";
    case Template::GenericChain: return "generic_chain";
  }
  return "auto";
}

std::optional<Template> template_from_string(std::string_view s) {
  for (Template t : {Template::Auto, Template::CloakedCharacter, Template::Humanoid, Template::GenericChain})
    if (to_string(t) == s) return t;
  return std::nullopt;
}

std::string_view to_string(Style s) {
  switch (s) {
    case Style::Flat: return "flat";
    case Style::Cel: return "cel";
    case Style::Outline: return "outline";
    case Style::CelOutline: return "cel_outline";
  }
  return "cel_outline";
}

std::optional<Style> style_from_string(std::string_view s) {
  for (Style t : {Style::Flat, Style::Cel, Style::Outline, Style::CelOutline})
    if (to_string(t) == s) return t;
  return std::nullopt;
}

int Skeleton::find(std::string_view name) const {
  for (size_t i = 0; i < bones.size(); ++i)
    if (bones[i].name == name) return static_cast<int>(i);
  return -1;
}

}  // namespace pixel3d::lowpoly
