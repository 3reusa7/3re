#include "pixel3d/reconstruct/part_geometry.hpp"

#include <vector>

namespace pixel3d {

void PartGeometry::normalize() {
  std::erase_if(parts, [](const auto& kv) { return kv.second.IsEmpty(); });
}

manifold::Manifold PartGeometry::all() const {
  std::vector<manifold::Manifold> v;
  for (const auto& [id, m] : parts) v.push_back(m);
  return v.empty() ? manifold::Manifold() : manifold::Manifold::BatchBoolean(v, manifold::OpType::Add);
}

SolidResult assemble_parts(const PartGeometry& g, const CleanupOptions& opt) {
  std::vector<manifold::Manifold> solids;
  std::map<int, int> oid_to_part;
  for (const auto& [id, m] : g.parts) {
    if (m.IsEmpty()) continue;
    const manifold::Manifold o = m.AsOriginal();
    oid_to_part[o.OriginalID()] = id;
    solids.push_back(o);
  }
  if (solids.empty()) return SolidResult{};
  const manifold::Manifold all = manifold::Manifold::BatchBoolean(solids, manifold::OpType::Add).Simplify();
  return finish_solid(mesh_from_manifold(all, oid_to_part), opt);
}

PartGeometry true_solid_parts(const CandidateSet& cs, const std::vector<DepthConstraint>& constraints, Dims d,
                              const std::vector<int>& part_of_candidate) {
  std::map<int, std::vector<manifold::Manifold>> by_part;
  for (const Candidate& c : cs.cands) {
    if (!is_kept(c.status)) continue;
    const int part = c.id < static_cast<int>(part_of_candidate.size()) ? part_of_candidate[c.id] : -1;
    by_part[part].push_back(c.solid);
  }
  manifold::Manifold carve;
  for (const DepthConstraint& c : constraints) carve = carve + carve_volume(c, d);
  PartGeometry g;
  for (auto& [part, solids] : by_part) {
    manifold::Manifold m = manifold::Manifold::BatchBoolean(solids, manifold::OpType::Add);
    if (!carve.IsEmpty()) m = m - carve;
    g.parts[part] = m;
  }
  g.normalize();
  return g;
}

PartGeometry voxel_parts(const VoxelGrid& g) {
  const Dims d = g.dims();
  // Row runs along X become boxes; Manifold merges them into one watertight
  // solid (cells touching only along an edge stay manifold).
  std::vector<manifold::Manifold> boxes;
  for (int z = 0; z < d.D; ++z)
    for (int y = 0; y < d.H; ++y) {
      int x = 0;
      while (x < d.W) {
        if (!g.get(x, y, z)) {
          ++x;
          continue;
        }
        const int x0 = x;
        while (x < d.W && g.get(x, y, z)) ++x;
        boxes.push_back(manifold::Manifold::Cube({static_cast<double>(x - x0), 1.0, 1.0}).Translate(
            {static_cast<double>(x0), static_cast<double>(y), static_cast<double>(z)}));
      }
    }
  PartGeometry out;
  if (!boxes.empty()) out.parts[0] = manifold::Manifold::BatchBoolean(boxes, manifold::OpType::Add);
  return out;
}

}  // namespace pixel3d
