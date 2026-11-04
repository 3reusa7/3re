#include "pixel3d/style/materials.hpp"

#include <algorithm>

namespace pixel3d {

std::vector<StyleMaterial> default_materials(const MaterialFamilies& fam, const std::string& style) {
  // Toys and clay are matte; soft plastic a little glossier; lowpoly in between.
  const double roughness = style == "clay" ? 0.95 : style == "soft" ? 0.7 : style == "lowpoly" ? 0.85 : 1.0;
  std::vector<StyleMaterial> out;
  for (int f = 0; f < fam.family_count; ++f)
    out.push_back(StyleMaterial{f == fam.outline_family ? std::string("outline") : "family_" + std::to_string(f), roughness, 0, 0});
  if (out.empty()) out.push_back(StyleMaterial{"default", roughness, 0, 0});
  return out;
}

std::vector<int> triangle_families(const TexturedMesh& m, const MaterialFamilies& fam) {
  std::vector<int> out(m.indices.size() / 3, -1);
  if (!m.texture || m.uvs.size() != m.positions.size()) return out;
  const ImageRGBA& tex = *m.texture;
  for (size_t t = 0; t < out.size(); ++t) {
    const vec2d uv = (m.uvs[m.indices[3 * t]] + m.uvs[m.indices[3 * t + 1]] + m.uvs[m.indices[3 * t + 2]]) * (1.0 / 3);
    const int x = std::clamp(static_cast<int>(uv.x * tex.width()), 0, tex.width() - 1);
    const int y = std::clamp(static_cast<int>(uv.y * tex.height()), 0, tex.height() - 1);
    out[t] = fam.family_of(tex.at(x, y));
  }
  return out;
}

}  // namespace pixel3d
