#pragma once
// build_lowpoly: drawings + typed parts → a skinned, textured low-poly character.
// Deterministic, CPU only. See the spec for every rule; the report says what
// was measured, what was inferred and where quality is limited by the budget.
#include <vector>

#include "pixel3d/image/palette.hpp"
#include "pixel3d/lowpoly/allocate.hpp"
#include "pixel3d/lowpoly/assemble.hpp"
#include "pixel3d/lowpoly/layout.hpp"
#include "pixel3d/lowpoly/texture_bake.hpp"

namespace pixel3d::lowpoly {

struct Result {
  Layout layout;
  Allocation alloc;
  Assembly assembly;   // assembly.mesh is the final mesh (with the hull appended when enabled)
  PackResult pack;
  BakeResult bake;
  MaterialFamilies families;
  int triangles = 0;   // exported triangles, hull included
  int hull_triangles = 0;
  std::vector<TubeReport> tubes;
  std::vector<PartReport> parts;
  std::vector<Message> messages;
  const SkinnedMesh& mesh() const { return assembly.mesh; }
};

// Errors: lowpoly.* from the layout and the allocator, lowpoly.atlas_too_small.
Expected<Result> build_lowpoly(const Input& in);

// Inverted-hull outline: every triangle again, pushed out along the normals by
// `width` and wound the other way (counts toward the budget).
void append_inverted_hull(SkinnedMesh& m, double width);

}  // namespace pixel3d::lowpoly
