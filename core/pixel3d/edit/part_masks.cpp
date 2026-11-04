#include "pixel3d/edit/part_masks.hpp"

#include <algorithm>
#include <cmath>

#include "pixel3d/common/limits.hpp"
#include "pixel3d/polygon/contour.hpp"
#include "pixel3d/reconstruct/prism.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

using nlohmann::json;

std::vector<ViewRole> PartMasks::views_of(int part) const {
  std::vector<ViewRole> out;
  const auto it = parts.find(part);
  if (it == parts.end()) return out;
  for (const auto& [role, m] : it->second.masks)
    if (m.count() > 0) out.push_back(role);
  return out;
}

namespace {

json mask_rle(const Mask2D& m) {
  // Alternating run lengths, starting with an "off" run (which may be 0).
  json runs = json::array();
  bool cur = false;
  int run = 0;
  for (int v = 0; v < m.height(); ++v)
    for (int u = 0; u < m.width(); ++u) {
      if (m.get(u, v) == cur) {
        ++run;
        continue;
      }
      runs.push_back(run);
      cur = !cur;
      run = 1;
    }
  runs.push_back(run);
  return {{"w", m.width()}, {"h", m.height()}, {"runs", runs}};
}

Expected<Mask2D> mask_from(const json& j) {
  const int w = j.at("w").get<int>(), h = j.at("h").get<int>();
  if (w <= 0 || h <= 0 || w > limits::kMaxImageSide || h > limits::kMaxImageSide ||
      static_cast<int64_t>(w) * h > limits::kMaxImagePixels)
    return make_error("project.invalid", "part mask size out of range");
  const json& runs = j.at("runs");
  if (!runs.is_array()) return make_error("project.invalid", "malformed part mask");
  Mask2D m(w, h);
  const int64_t total = static_cast<int64_t>(w) * h;
  int64_t pos = 0;
  bool on = false;
  for (const auto& r : runs) {
    const int64_t n = r.get<int64_t>();
    if (n < 0 || pos + n > total) return make_error("project.invalid", "malformed part mask");
    if (on)
      for (int64_t q = pos; q < pos + n; ++q) m.set(static_cast<int>(q % w), static_cast<int>(q / w), true);
    pos += n;
    on = !on;
  }
  if (pos != total) return make_error("project.invalid", "part mask runs do not cover the image");
  return m;
}

Mesh submesh(const Mesh& solid, int part) {
  Mesh m;
  m.positions = solid.positions;
  for (size_t t = 0; t < solid.tris.size(); ++t)
    if (t < solid.tri_part.size() && solid.tri_part[t] == part) {
      m.tris.push_back(solid.tris[t]);
      m.tri_part.push_back(part);
    }
  return m;
}

bool size_ok(const Mask2D& m, ViewRole r, Dims d) {
  const ImageSize sz = view_image_size(r, d);
  return m.width() == sz.w && m.height() == sz.h;
}

}  // namespace

json part_masks_to_json(const PartMasks& m) {
  json j = json::object();
  for (const auto& [id, p] : m.parts) {
    json pj{{"define", p.define}, {"masks", json::object()}};
    if (p.depth) pj["depth"] = *p.depth;
    for (const auto& [role, mask] : p.masks)
      if (mask.count() > 0) pj["masks"][std::string(to_string(role))] = mask_rle(mask);
    j[std::to_string(id)] = pj;
  }
  return j;
}

Expected<PartMasks> part_masks_from_json(const json& j) {
  PartMasks m;
  if (j.is_null()) return m;
  try {
    if (!j.is_object()) return make_error("project.invalid", "part masks must be an object");
    if (j.size() > 10000) return make_error("project.invalid", "too many user parts");
    for (const auto& [k, p] : j.items()) {
      const int id = std::stoi(k);
      if (id < kFirstUserPart) return make_error("project.invalid", "user part ids start at 1000");
      UserPart up;
      up.define = p.value("define", false);
      if (p.contains("depth") && p["depth"].is_number()) {
        up.depth = p["depth"].get<double>();
        if (!std::isfinite(*up.depth) || *up.depth <= 0 || *up.depth > 4096)
          return make_error("project.invalid", "user part depth out of range");
      }
      const json masks = p.value("masks", json::object());
      for (const auto& [rk, mj] : masks.items()) {
        const auto role = view_role_from_string(rk);
        if (!role) return make_error("project.invalid", "unknown view role in part masks");
        auto mask = mask_from(mj);
        if (!mask) return Unexpected{mask.error()};
        up.masks[*role] = std::move(*mask);
      }
      m.parts[id] = std::move(up);
    }
  } catch (const std::exception& e) {
    return make_error("project.invalid", std::string("malformed part masks: ") + e.what());
  }
  return m;
}

Polygons2D mask_polygons(const Mask2D& m, const SimplifyOptions& opt) {
  return simplify_feature_preserving(mask_to_polygons(m), m, opt);
}

std::vector<int> claimed_candidates(const PartMasks& masks, const CandidateSet& cs, Dims d) {
  std::vector<int> out;
  std::map<ViewRole, std::vector<Mask2D>> define_masks;
  for (const auto& [id, up] : masks.parts)
    if (up.define)
      for (const auto& [role, m] : up.masks)
        if (m.count() > 0 && size_ok(m, role, d)) define_masks[role].push_back(m);
  if (define_masks.empty()) return out;
  for (const Candidate& c : cs.cands) {
    const Mesh cm = mesh_from_manifold(c.solid, {});
    bool claimed = false;
    for (const auto& [role, list] : define_masks) {
      const RenderTarget rt = render_mesh_flat(cm, camera_for_view(role, d, 1));
      const int covered = rt.coverage.count();
      if (covered == 0) continue;
      for (const Mask2D& m : list) {
        int inside = 0;
        for (int v = 0; v < m.height(); ++v)
          for (int u = 0; u < m.width(); ++u) inside += (m.get(u, v) && rt.coverage.get_or_false(u, v)) ? 1 : 0;
        if (inside >= 0.9 * covered) claimed = true;
      }
      if (claimed) break;
    }
    if (claimed) out.push_back(c.id);
  }
  return out;
}

void add_user_parts(PartGeometry& g, const PartMasks& masks, const manifold::Manifold& hull, Dims d,
                    const SimplifyOptions& opt, std::vector<Message>* notes) {
  auto region_of = [&](int id, const UserPart& up, bool* any) {
    manifold::Manifold region;
    *any = false;
    for (const auto& [role, m] : up.masks) {
      if (m.count() == 0) continue;
      if (!size_ok(m, role, d)) {
        if (notes)
          notes->push_back(Message{"mask.size_mismatch", "a part mask no longer fits its view (the drawing changed size)",
                                   Severity::Warning, {{"part", id}, {"view", to_string(role)}}});
        continue;
      }
      const manifold::Manifold prism = make_prism(role, d, mask_polygons(m, opt));
      region = *any ? region ^ prism : prism;
      *any = true;
    }
    return region;
  };
  for (const bool define_pass : {true, false})
    for (const auto& [id, up] : masks.parts) {
      if (up.define != define_pass) continue;
      bool any = false;
      const manifold::Manifold region = region_of(id, up, &any);
      if (!any) {
        if (notes)
          notes->push_back(Message{"mask.empty_part", "a user part has no mask pixels yet", Severity::Info, {{"part", id}}});
        continue;
      }
      if (up.define) {
        const manifold::Manifold geom = region ^ hull;
        if (!geom.IsEmpty()) g.parts[id] = geom;
        continue;
      }
      const manifold::Manifold geom = g.all() ^ region;
      for (auto& [pid, m] : g.parts)
        if (pid != id) m = m - region;
      if (!geom.IsEmpty()) g.parts[id] = geom;
    }
  g.normalize();
}

std::vector<MaskConflict> mask_conflicts(const PartMasks& masks, const Mesh& solid, Dims d) {
  std::vector<MaskConflict> out;
  for (const auto& [id, up] : masks.parts) {
    const Mesh part = submesh(solid, id);
    for (const auto& [role, m] : up.masks) {
      if (m.count() == 0 || !size_ok(m, role, d)) continue;
      const RenderTarget rt = render_mesh_flat(part, camera_for_view(role, d, 1));
      MaskConflict c;
      c.part = id;
      c.view = role;
      c.unmatched = Mask2D(m.width(), m.height());
      for (int v = 0; v < m.height(); ++v)
        for (int u = 0; u < m.width(); ++u) {
          if (!m.get(u, v)) continue;
          ++c.mask_px;
          if (!rt.coverage.get_or_false(u, v)) c.unmatched.set(u, v, true), ++c.unmatched_px;
        }
      c.box = c.unmatched.bbox();
      if (c.unmatched_px > 0) out.push_back(std::move(c));
    }
  }
  return out;
}

std::vector<ViewRole> sync_part_masks(PartMasks& masks, int part, const Mesh& solid, Dims d,
                                      const std::map<ViewRole, Mask2D>& opaque) {
  std::vector<ViewRole> changed;
  auto pit = masks.parts.find(part);
  if (pit == masks.parts.end()) return changed;
  const std::vector<ViewRole> have = masks.views_of(part);
  const Mesh pm = submesh(solid, part);
  if (pm.tris.empty()) return changed;
  for (const auto& [role, op] : opaque) {
    if (std::find(have.begin(), have.end(), role) != have.end()) continue;
    const RenderTarget rt = render_mesh_flat(pm, camera_for_view(role, d, 1));
    Mask2D m(op.width(), op.height());
    for (int v = 0; v < m.height(); ++v)
      for (int u = 0; u < m.width(); ++u)
        if (rt.coverage.get_or_false(u, v) && op.get(u, v)) m.set(u, v, true);
    if (m.count() == 0) continue;
    pit->second.masks[role] = std::move(m);
    changed.push_back(role);
  }
  return changed;
}

}  // namespace pixel3d
