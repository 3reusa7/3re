#pragma once
// PBR materials per material family for polished styles (step 8). Families are
// colour groups from the palette analysis; nothing is known about what they are
// made of, so defaults are the same for every family and depend only on the
// style. Metal and glow are never guessed: the user sets them per family.
#include <string>
#include <vector>

#include "pixel3d/image/palette.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

// One StyleMaterial per family, named "family_<n>" (outline: "outline").
std::vector<StyleMaterial> default_materials(const MaterialFamilies& fam, const std::string& style);

// Family of each triangle: the family of its texture colour at the centroid
// (−1 only for colours outside the palette).
std::vector<int> triangle_families(const TexturedMesh& m, const MaterialFamilies& fam);

}  // namespace pixel3d
