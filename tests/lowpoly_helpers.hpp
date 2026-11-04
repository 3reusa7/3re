#pragma once
// Test-only: loads fixture E (stylised characters) as LOW-POLY STUDIO input,
// with the ground-truth part masks from its part-id maps.
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "pixel3d/lowpoly/types.hpp"
#include "pixel3d/lowpoly/views.hpp"

namespace p3dtest {

inline std::string lowpoly_fixture_dir(const std::string& variant) {
  // Fixture F (1.2.0 input B) lives at the fixtures root; E's variants under E_lowpoly_character.
  if (variant.rfind("F_", 0) == 0) return std::string(PIXEL3D_FIXTURES_DIR) + "/" + variant;
  return std::string(PIXEL3D_FIXTURES_DIR) + "/E_lowpoly_character/" + variant;
}

inline nlohmann::json lowpoly_fixture_meta(const std::string& variant) {
  std::ifstream f(lowpoly_fixture_dir(variant) + "/parts.json");
  if (!f) throw std::runtime_error("missing parts.json for " + variant);
  return nlohmann::json::parse(f);
}

// variant: "cloaked" | "humanoid" | "F_hooded_reference"; with_side = false gives the front-only variant.
inline pixel3d::lowpoly::Input load_lowpoly_fixture(const std::string& variant, bool with_side = true,
                                                     double front_only_depth = 0) {
  using namespace pixel3d;
  const std::string dir = lowpoly_fixture_dir(variant);
  std::vector<SourceView> views;
  std::vector<ViewRole> roles{ViewRole::Front};
  if (with_side) roles.push_back(ViewRole::Side);
  for (ViewRole r : roles) {
    auto img = load_png(dir + "/" + std::string(to_string(r)) + ".png");
    if (!img) throw std::runtime_error(img.error().text);
    SourceView s;
    s.role = r;
    s.original = std::make_shared<const ImageRGBA>(std::move(*img));
    views.push_back(std::move(s));
  }
  const nlohmann::json meta = lowpoly_fixture_meta(variant);
  auto al = lowpoly::align_views(views, AlignOptions{}, with_side ? 0 : front_only_depth);
  if (!al) throw std::runtime_error("align failed: " + al.error().text);
  lowpoly::Input in;
  in.views = views;
  in.dims = al->dims;
  for (const auto& jp : meta["parts"]) {
    lowpoly::Part p;
    p.id = jp["id"].get<int>();
    p.name = jp["name"].get<std::string>();
    p.type = *lowpoly::part_type_from_string(jp["type"].get<std::string>());
    const Rgba8 key{jp["key"][0].get<uint8_t>(), jp["key"][1].get<uint8_t>(), jp["key"][2].get<uint8_t>(), 255};
    for (ViewRole r : roles) {
      auto map = load_png(dir + "/" + std::string(to_string(r)) + "_parts.png");
      if (!map) throw std::runtime_error(map.error().text);
      Mask2D m(map->width(), map->height());
      for (int v = 0; v < map->height(); ++v)
        for (int u = 0; u < map->width(); ++u) m.set(u, v, map->at(u, v) == key);
      if (m.count() > 0) p.masks.emplace(r, std::move(m));
    }
    in.parts.push_back(std::move(p));
  }
  if (!with_side) {
    in.settings.depth = front_only_depth;
    in.settings.user_depth = front_only_depth > 0;
  }
  return in;
}

}  // namespace p3dtest
