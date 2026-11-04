#include "pixel3d/lowpoly/build.hpp"

#include <algorithm>
#include <map>

namespace pixel3d::lowpoly {

void append_inverted_hull(SkinnedMesh& m, double width) {
  const size_t nv = m.positions.size(), ni = m.indices.size();
  m.hull_appended = true;
  m.hull_first_index = ni;
  // Hull vertices share the welded smooth normal of their position (so the shell stays closed at seams).
  std::map<std::tuple<double, double, double>, vec3d> welded;
  for (size_t v = 0; v < nv; ++v) {
    auto& n = welded[{m.positions[v].x, m.positions[v].y, m.positions[v].z}];
    n = n + m.normals[v];
  }
  for (size_t v = 0; v < nv; ++v) {
    const vec3d n = normalize(welded[{m.positions[v].x, m.positions[v].y, m.positions[v].z}]);
    m.positions.push_back(m.positions[v] + n * width);
    m.normals.push_back(n * -1.0);
    m.uvs.push_back(m.uvs.empty() ? vec2d{} : m.uvs[v]);
    m.joints.push_back(m.joints[v]);
    m.weights.push_back(m.weights[v]);
    m.vertex_shell.push_back(m.vertex_shell[v]);
    m.vertex_part.push_back(m.vertex_part[v]);
    m.vertex_joint_loop.push_back(m.vertex_joint_loop[v]);
  }
  const size_t nt = ni / 3;
  for (size_t t = 0; t < nt; ++t) {
    m.indices.push_back(static_cast<uint32_t>(m.indices[3 * t] + nv));
    m.indices.push_back(static_cast<uint32_t>(m.indices[3 * t + 2] + nv));
    m.indices.push_back(static_cast<uint32_t>(m.indices[3 * t + 1] + nv));
    m.tri_shell.push_back(m.tri_shell[t]);
  }
}

Expected<Result> build_lowpoly(const Input& in) {
  Result r;
  auto layout = build_layout(in);
  if (!layout) return Unexpected{layout.error()};
  r.layout = std::move(*layout);
  auto alloc = allocate(r.layout, in.settings);
  if (!alloc) return Unexpected{alloc.error()};
  r.alloc = std::move(*alloc);
  r.assembly = assemble(r.layout, r.alloc, in.dims.H);
  r.pack = pack_islands(r.assembly, in.settings.atlas_max);
  if (!r.pack.ok) return make_error("lowpoly.atlas_too_small", "the UV islands do not fit the atlas");
  std::vector<const ImageRGBA*> imgs;
  for (const auto& v : in.views)
    if (v.aligned) imgs.push_back(v.aligned.get());
  r.families = analyze_palette(imgs, in.alpha_threshold);
  BakeOptions bo;
  bo.alpha_threshold = in.alpha_threshold;
  bo.opposite_head_cloth = in.settings.opposite_projection_head_cloth;
  bo.baked_cel = in.settings.baked_cel;
  bo.tones = in.settings.tones;
  r.bake = bake_texture(r.assembly, r.pack, in, r.layout, r.families, bo);
  r.assembly.mesh.uvs.shrink_to_fit();
  if (in.settings.hull) {
    const int before = static_cast<int>(r.assembly.mesh.triangle_count());
    append_inverted_hull(r.assembly.mesh, in.settings.hull_width);
    r.hull_triangles = static_cast<int>(r.assembly.mesh.triangle_count()) - before;
    r.messages.push_back(Message{"lowpoly.hull", "inverted-hull outline: " + std::to_string(r.hull_triangles) +
                                                     " triangles, counted in the budget",
                                 Severity::Info, {{"triangles", r.hull_triangles}}});
  }
  r.triangles = static_cast<int>(r.assembly.mesh.triangle_count());
  // Reports.
  std::map<int, PartReport> parts;
  for (const Part& p : in.parts) parts[p.id] = {p.id, p.name, p.type, 0, 0};
  const SkinnedMesh& M = r.assembly.mesh;
  for (size_t t = 0; t < M.triangle_count(); ++t) {
    const int part = M.vertex_part[M.indices[3 * t]];
    if (parts.count(part)) ++parts[part].triangles;
  }
  for (size_t i = 0; i < r.layout.tubes.size(); ++i) {
    const TubeSpec& t = r.layout.tubes[i];
    const TubePlan& p = r.alloc.tubes[i];
    TubeReport tr;
    tr.name = t.name;
    tr.parts = t.parts;
    tr.branch = t.branch;
    tr.parent_tube = t.parent_tube;
    tr.sides = p.sides;
    tr.rings = p.rings;
    tr.joints = static_cast<int>(t.joints.size());
    tr.joint_loops = t.joints.empty() ? 0 : p.loops;
    tr.triangles = p.triangles;
    tr.profile_error_px = p.profile_error;
    tr.depth_source = t.depth_source;
    r.tubes.push_back(tr);
    for (int id : t.parts) {
      if (!parts.count(id)) continue;
      parts[id].importance = p.importance;
      parts[id].depth_source = t.depth_source;
      // Median depth/width of the part's own cross-sections (outside any parent).
      std::vector<double> ratios;
      for (const Station& st : t.dense)
        if (st.owner == id && st.s >= 0 && st.half_w >= 0.75) ratios.push_back(st.half_d / st.half_w);
      if (!ratios.empty()) {
        std::nth_element(ratios.begin(), ratios.begin() + static_cast<long>(ratios.size() / 2), ratios.end());
        parts[id].depth_ratio = ratios[ratios.size() / 2];
      }
    }
  }
  for (auto& [id, pr] : parts) r.parts.push_back(pr);
  for (const auto& m : r.layout.messages) r.messages.push_back(m);
  for (const auto& m : r.alloc.messages) r.messages.push_back(m);
  for (const auto& m : r.bake.messages) r.messages.push_back(m);
  return r;
}

}  // namespace pixel3d::lowpoly
