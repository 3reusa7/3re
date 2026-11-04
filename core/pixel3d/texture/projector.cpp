#include "pixel3d/texture/projector.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <tuple>

#include "pixel3d/mesh/cleanup.hpp"
#include "pixel3d/render/camera.hpp"

namespace pixel3d {
namespace {

constexpr int kVisPpu = 4;            // depth-buffer resolution for visibility tests
constexpr double kOpaqueValid = 0.9;  // Opposite Projection: share of samples on opaque art
constexpr double kEdgeInset = 0.25;   // texels: Edge Extend samples just inside the seen neighbour

struct ViewInfo {
  ViewRole role;
  const SourceView* src;
  Camera cam;
  RenderTarget rt;
};

std::vector<vec3d> sample_points(vec3d a, vec3d b, vec3d c, int n) {
  int r = 1;
  while (r * (r + 1) / 2 < n) ++r;
  std::vector<vec3d> pts;
  for (int i = 0; i < r; ++i)
    for (int j = 0; i + j < r; ++j) {
      const double u = (i + 1.0 / 3.0) / r, v = (j + 1.0 / 3.0) / r;
      pts.push_back(a + (b - a) * u + (c - a) * v);
    }
  return pts;
}

}  // namespace

std::string_view to_string(SurfaceMode m) {
  switch (m) {
    case SurfaceMode::Seen: return "seen";
    case SurfaceMode::OppositeProjection: return "opposite_projection";
    case SurfaceMode::EdgeExtend: return "edge_extend";
    case SurfaceMode::FlatPalette: return "flat_palette";
    case SurfaceMode::Manual: return "manual";
  }
  return "seen";
}

TextureResult project_textures(const Mesh& in, const std::vector<SourceView>& views, const MaterialFamilies& fam,
                               Dims d, const TextureOptions& opt, const std::vector<TextureAssignment>& manual,
                               uint8_t alpha_threshold) {
  TextureResult res;
  res.atlas = build_atlas(views, fam, 2, 2, alpha_threshold);
  const Atlas& atlas = res.atlas;
  Mesh mesh = in;
  if (mesh.tri_region.size() != mesh.tris.size()) label_planar_regions(mesh);
  const size_t T = mesh.tris.size();

  std::vector<ViewInfo> vis;
  for (const SourceView& v : views) {
    if (!v.aligned || !atlas.cell(v.role)) continue;
    ViewInfo vi{v.role, &v, camera_for_view(v.role, d, kVisPpu), {}};
    vi.rt = render_mesh_flat(mesh, vi.cam);
    vis.push_back(std::move(vi));
  }
  auto find_view = [&](ViewRole r) -> const ViewInfo* {
    for (const ViewInfo& v : vis)
      if (v.role == r) return &v;
    return nullptr;
  };
  auto corner = [&](size_t t, int k) { return mesh.positions[mesh.tris[t][k]]; };
  auto source_pixel = [&](const ViewInfo& v, vec3d p, bool* opaque) {
    const vec3d uvd = view_from_world(v.role, d, p);
    const int px = static_cast<int>(std::floor(uvd.x)), py = static_cast<int>(std::floor(uvd.y));
    if (!v.src->aligned->in_bounds(px, py)) {
      *opaque = false;
      return Rgba8{};
    }
    const Rgba8 c = v.src->aligned->at(px, py);
    *opaque = c.a >= alpha_threshold;
    return c;
  };

  // 1. Seen faces. Visibility is judged per planar region: the pixels the region
  // actually covers in a view's depth buffer against its projected area. A
  // whole planar face therefore takes one view (the most frontal seeing it).
  std::vector<vec3d> normal(T);
  std::vector<double> area(T);
  res.faces.assign(T, FaceTexture{});
  std::vector<uint8_t> seen(T, 0);
  int regions = 0;
  for (size_t t = 0; t < T; ++t) {
    const vec3d raw = cross(corner(t, 1) - corner(t, 0), corner(t, 2) - corner(t, 0));
    area[t] = 0.5 * length(raw);
    normal[t] = normalize(raw);
    regions = std::max(regions, mesh.tri_region[t] + 1);
  }
  std::vector<double> best(regions, 0);
  std::vector<int> best_view(regions, -1);
  for (size_t vi = 0; vi < vis.size(); ++vi) {
    const ViewInfo& v = vis[vi];
    std::vector<double> expected(regions, 0), covered(regions, 0), cosine(regions, 0);
    for (size_t t = 0; t < T; ++t) {
      const double c = -dot(normal[t], view_dir(v.role));
      if (c <= 1e-6) continue;
      const vec3d a = v.cam.project(corner(t, 0)), b = v.cam.project(corner(t, 1)), cc = v.cam.project(corner(t, 2));
      expected[mesh.tri_region[t]] += 0.5 * std::abs((b.x - a.x) * (cc.y - a.y) - (b.y - a.y) * (cc.x - a.x));
      cosine[mesh.tri_region[t]] = c;
    }
    for (int hit : v.rt.tri_id)
      if (hit >= 0) covered[mesh.tri_region[hit]] += 1;
    for (int r = 0; r < regions; ++r) {
      if (expected[r] <= 0 || cosine[r] <= best[r]) continue;
      // Thin regions lose up to half a pixel along their outline when rasterized.
      if (covered[r] > 0 && covered[r] >= opt.visibility_fraction * expected[r] - 2.0) {
        best[r] = cosine[r];
        best_view[r] = static_cast<int>(vi);
      }
    }
  }
  for (size_t t = 0; t < T; ++t) {
    const int bv = best_view[mesh.tri_region[t]];
    if (bv < 0) continue;
    res.faces[t] = FaceTexture{vis[bv].role, SurfaceMode::Seen, false};
    seen[t] = 1;
  }

  // 2. Dominant material family per part, from the pixels its seen faces use.
  std::map<int, std::map<int, double>> part_fam;
  std::map<int, double> all_fam;
  for (size_t t = 0; t < T; ++t) {
    if (!seen[t]) continue;
    const ViewInfo& v = *find_view(res.faces[t].view);
    for (const vec3d& p : sample_points(corner(t, 0), corner(t, 1), corner(t, 2), opt.visibility_samples)) {
      bool op = false;
      const int f = fam.family_of(source_pixel(v, p, &op), alpha_threshold);
      if (!op || f < 0 || f == fam.outline_family) continue;
      const int part = t < mesh.tri_part.size() ? mesh.tri_part[t] : -1;
      part_fam[part][f] += area[t];
      all_fam[f] += area[t];
    }
  }
  auto argmax = [](const std::map<int, double>& h) {
    int best = -1;
    double bv = -1;
    for (const auto& [k, v] : h)
      if (v > bv) best = k, bv = v;
    return best;
  };
  const int global_family = argmax(all_fam);
  auto part_family = [&](size_t t) {
    const int part = t < mesh.tri_part.size() ? mesh.tri_part[t] : -1;
    const auto it = part_fam.find(part);
    return it == part_fam.end() ? -1 : argmax(it->second);
  };

  // Edge adjacency for Edge Extend.
  std::map<std::pair<uint32_t, uint32_t>, std::vector<size_t>> edges;
  for (size_t t = 0; t < T; ++t)
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = mesh.tris[t][k], b = mesh.tris[t][(k + 1) % 3];
      edges[{std::min(a, b), std::max(a, b)}].push_back(t);
    }

  // 3. Inferred surfaces.
  std::vector<std::array<vec2d, 3>> fixed_uv(T);  // Edge Extend / Flat Palette UVs
  std::vector<uint8_t> has_fixed(T, 0);
  auto flat_uv = [&](size_t t) {
    int f = part_family(t);
    if (f < 0) f = global_family;
    const auto it = atlas.swatch_uv.find(f);
    const vec2d uv = it != atlas.swatch_uv.end() ? it->second : vec2d{0, 0};
    fixed_uv[t] = {uv, uv, uv};
    has_fixed[t] = 1;
  };
  for (size_t t = 0; t < T; ++t) {
    if (seen[t]) continue;
    FaceTexture& ft = res.faces[t];
    ft.inferred = true;
    // a) Opposite Projection along the dominant axis (a facing view first).
    const vec3d n = normal[t];
    const int axis = (std::abs(n.x) >= std::abs(n.y) && std::abs(n.x) >= std::abs(n.z)) ? 0
                     : (std::abs(n.y) >= std::abs(n.z))                                ? 1
                                                                                       : 2;
    const ViewInfo* chosen = nullptr;
    for (bool facing : {true, false})
      for (const ViewInfo& v : vis) {
        const vec3d vd = view_dir(v.role);
        const double comp = axis == 0 ? vd.x : axis == 1 ? vd.y : vd.z;
        const double nc = axis == 0 ? n.x : axis == 1 ? n.y : n.z;
        if (std::abs(comp) < 0.5 || ((comp * nc < 0) != facing) || chosen) continue;
        chosen = &v;
      }
    bool valid = false;
    if (opt.allow_opposite_projection && chosen) {
      // The art it would reuse must lie on the drawing (slope faces may cover up
      // to half-pixel corners outside the staircase) and be the face's material.
      std::map<int, double> h;
      const auto pts = sample_points(corner(t, 0), corner(t, 1), corner(t, 2), opt.visibility_samples);
      int opaque_n = 0;
      for (const vec3d& p : pts) {
        bool op = false;
        const int f = fam.family_of(source_pixel(*chosen, p, &op), alpha_threshold);
        opaque_n += op ? 1 : 0;
        if (op && f >= 0 && f != fam.outline_family) h[f] += 1;
      }
      valid = opaque_n >= kOpaqueValid * static_cast<double>(pts.size());
      const int own = part_family(t);
      if (valid && own >= 0 && !h.empty() && argmax(h) != own) valid = false;
    }
    if (valid) {
      ft.view = chosen->role;
      ft.mode = SurfaceMode::OppositeProjection;
      continue;
    }
    // b) Edge Extend from a seen neighbour.
    bool extended = false;
    for (int k = 0; k < 3 && !extended; ++k) {
      const uint32_t ia = mesh.tris[t][k], ib = mesh.tris[t][(k + 1) % 3], ic = mesh.tris[t][(k + 2) % 3];
      for (size_t tn : edges[{std::min(ia, ib), std::max(ia, ib)}]) {
        if (tn == t || !seen[tn]) continue;
        const ViewRole vr = res.faces[tn].view;
        auto px = [&](vec3d p) {
          const vec3d uvd = view_from_world(vr, d, p);
          const AtlasCell* c = atlas.cell(vr);
          return vec2d{c->x0 + uvd.x, c->y0 + uvd.y};
        };
        const vec3d A = mesh.positions[ia], B = mesh.positions[ib], C = mesh.positions[ic];
        const vec3d AB = B - A;
        const double len2 = dot(AB, AB);
        const vec3d Cp = len2 > 0 ? A + AB * (dot(C - A, AB) / len2) : A;
        uint32_t idd = mesh.tris[tn][0];
        for (uint32_t q : mesh.tris[tn])
          if (q != ia && q != ib) idd = q;
        const vec2d pa = px(A), pb = px(B), pc = px(Cp), pd = px(mesh.positions[idd]);
        vec2d dir = pd - (pa + pb) * 0.5;
        const double dl = length(dir);
        dir = dl > 1e-9 ? dir * (kEdgeInset / dl) : vec2d{0, 0};
        const double W = atlas.image->width(), H = atlas.image->height();
        auto to_uv = [&](vec2d p) { return vec2d{(p.x + dir.x) / W, (p.y + dir.y) / H}; };
        std::array<vec2d, 3> uv;
        uv[k] = to_uv(pa);
        uv[(k + 1) % 3] = to_uv(pb);
        uv[(k + 2) % 3] = to_uv(pc);
        // Only stretch real art: every corner must sample an opaque atlas texel.
        bool on_art = true;
        for (const vec2d& q : uv) {
          const int tx = std::clamp(static_cast<int>(std::floor(q.x * W)), 0, static_cast<int>(W) - 1);
          const int ty = std::clamp(static_cast<int>(std::floor(q.y * H)), 0, static_cast<int>(H) - 1);
          on_art &= atlas.image->at(tx, ty).a == 255;
        }
        if (!on_art) continue;
        fixed_uv[t] = uv;
        has_fixed[t] = 1;
        ft.view = vr;
        ft.mode = SurfaceMode::EdgeExtend;
        extended = true;
        break;
      }
    }
    if (extended) continue;
    // c) Flat Palette Color.
    ft.mode = SurfaceMode::FlatPalette;
    flat_uv(t);
  }

  // 4. Manual assignments override.
  int skipped = 0;
  for (const TextureAssignment& a : manual)
    for (int t : a.tris) {
      if (t < 0 || static_cast<size_t>(t) >= T || (a.mode != SurfaceMode::FlatPalette && !atlas.cell(a.view))) {
        ++skipped;
        continue;
      }
      res.faces[t] = FaceTexture{a.view, SurfaceMode::Manual, false};
      has_fixed[t] = 0;
      if (a.mode == SurfaceMode::FlatPalette) flat_uv(t);
    }

  if (skipped > 0)
    res.messages.push_back(Message{"texture_assignment.skipped",
                                   "manual texture assignments for missing triangles or views were skipped",
                                   Severity::Warning, {{"triangles", skipped}}});

  // 5. Build the render/export mesh (vertices split by UV and planar region).
  std::map<std::tuple<uint32_t, long long, long long, int>, uint32_t> vmap;
  TexturedMesh& tm = res.mesh;
  tm.texture = atlas.image;
  for (size_t t = 0; t < T; ++t) {
    for (int k = 0; k < 3; ++k) {
      const uint32_t vi = mesh.tris[t][k];
      vec2d uv;
      if (has_fixed[t]) {
        uv = fixed_uv[t][k];
      } else {
        const vec3d uvd = view_from_world(res.faces[t].view, d, mesh.positions[vi]);
        uv = atlas.uv(res.faces[t].view, uvd.x, uvd.y);
      }
      const auto key = std::make_tuple(vi, std::llround(uv.x * 1e9), std::llround(uv.y * 1e9), mesh.tri_region[t]);
      auto [it, ins] = vmap.try_emplace(key, static_cast<uint32_t>(tm.positions.size()));
      if (ins) {
        tm.positions.push_back(mesh.positions[vi]);
        tm.normals.push_back(normal[t]);
        tm.uvs.push_back(uv);
      }
      tm.indices.push_back(it->second);
    }
    tm.tri_part.push_back(t < mesh.tri_part.size() ? mesh.tri_part[t] : -1);
  }

  // 6. Statistics and messages.
  for (SurfaceMode m : {SurfaceMode::Seen, SurfaceMode::OppositeProjection, SurfaceMode::EdgeExtend,
                        SurfaceMode::FlatPalette, SurfaceMode::Manual})
    res.area_by_mode[m] = 0;
  double total = 0, inferred = 0;
  res.texel_density_min = 1e30;
  res.texel_density_max = 0;
  for (size_t t = 0; t < T; ++t) {
    res.area_by_mode[res.faces[t].mode] += area[t];
    total += area[t];
    if (res.faces[t].inferred) inferred += area[t];
    if (has_fixed[t]) continue;
    const double c = std::abs(dot(normal[t], view_dir(res.faces[t].view)));
    res.texel_density_min = std::min(res.texel_density_min, c);
    res.texel_density_max = std::max(res.texel_density_max, 1.0);
  }
  if (res.texel_density_min > res.texel_density_max) res.texel_density_min = res.texel_density_max = 0;
  if (inferred > 0) {
    nlohmann::json where;
    for (SurfaceMode m : {SurfaceMode::OppositeProjection, SurfaceMode::EdgeExtend, SurfaceMode::FlatPalette})
      where[std::string(to_string(m))] = total > 0 ? res.area_by_mode[m] / total : 0;
    res.messages.push_back(Message{"inferred_surface",
                                   "surfaces no source view shows are textured from existing art (never repainted)",
                                   Severity::Info, where});
  }
  if (total > 0 && res.area_by_mode[SurfaceMode::FlatPalette] > 0.10 * total)
    res.messages.push_back(Message{"inferred_surface_flat", "more than 10% of the surface uses flat palette colour",
                                   Severity::Warning, {{"fraction", res.area_by_mode[SurfaceMode::FlatPalette] / total}}});
  return res;
}

}  // namespace pixel3d
