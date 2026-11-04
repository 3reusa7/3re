#include "pixel3d/lowpoly/texture_bake.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>

#include "pixel3d/lowpoly/profile.hpp"
#include "pixel3d/lowpoly/style_render.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d::lowpoly {
namespace {

uint32_t pack_rgb(Rgba8 c) { return (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b; }

const Mask2D* part_mask(const Input& in, int part, ViewRole r) {
  for (const Part& p : in.parts)
    if (p.id == part) {
      auto it = p.masks.find(r);
      return it == p.masks.end() ? nullptr : &it->second;
    }
  return nullptr;
}

PartType part_type(const Input& in, int part) {
  for (const Part& p : in.parts)
    if (p.id == part) return p.type;
  return PartType::Torso;
}

bool near_mask(const Mask2D& m, int x, int y) {
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
      if (m.get_or_false(x + dx, y + dy)) return true;
  return false;
}

}  // namespace

PackResult pack_islands(Assembly& as, int max_side, double max_density, int gutter) {
  PackResult r;
  r.gutter = gutter;
  std::vector<size_t> order(as.islands.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  for (double rho : {6.0, 5.0, 4.0, 3.0, 2.5, 2.0, 1.5, 1.0, 0.75, 0.5, 0.35, 0.25}) {
    if (rho > max_density + 1e-9) continue;
    for (int side = 64; side <= max_side; side *= 2) {
      // Shelf packing by height.
      std::vector<std::pair<int, int>> size(as.islands.size());
      for (size_t i = 0; i < as.islands.size(); ++i) {
        const Island& is = as.islands[i];
        size[i] = {static_cast<int>(std::ceil((is.hi.x - is.lo.x) * rho)) + 1, static_cast<int>(std::ceil((is.hi.y - is.lo.y) * rho)) + 1};
      }
      std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return size[a].second > size[b].second; });
      int x = gutter, y = gutter, row = 0;
      bool fit = true;
      std::vector<std::pair<int, int>> at(as.islands.size());
      for (size_t i : order) {
        const auto [w, h] = size[i];
        if (w + 2 * gutter > side) {
          fit = false;
          break;
        }
        if (x + w + gutter > side) x = gutter, y += row + gutter, row = 0;
        if (y + h + gutter > side) {
          fit = false;
          break;
        }
        at[i] = {x, y};
        x += w + gutter;
        row = std::max(row, h);
      }
      if (!fit) continue;
      r.ok = true;
      r.width = r.height = side;
      r.density = rho;
      for (size_t i = 0; i < as.islands.size(); ++i) {
        Island& is = as.islands[i];
        is.x0 = at[i].first;
        is.y0 = at[i].second;
        is.w = size[i].first;
        is.h = size[i].second;
      }
      for (size_t v = 0; v < as.mesh.uvs.size(); ++v) {
        const Island& is = as.islands[static_cast<size_t>(as.vertex_island[v])];
        const vec2d l = as.mesh.uvs[v];
        as.mesh.uvs[v] = {(is.x0 + 0.5 + (l.x - is.lo.x) * rho) / side, (is.y0 + 0.5 + (l.y - is.lo.y) * rho) / side};
      }
      return r;
    }
  }
  return r;
}

ImageRGBA remove_silhouette_ink(const ImageRGBA& img, const MaterialFamilies& fam, uint8_t alpha, int band, int* replaced,
                                const std::vector<int>* labels) {
  ImageRGBA out = img;
  if (replaced) *replaced = 0;
  if (fam.outline_family < 0 || band <= 0) return out;
  const Mask2D opaque = opaque_mask(img, alpha);
  const DistanceField dt = distance_transform(opaque);
  const int w = img.width(), h = img.height();
  std::vector<uint8_t> is_band(static_cast<size_t>(w) * h, 0);
  std::deque<std::pair<int, int>> q;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (!opaque.get(x, y)) continue;
      const bool ink = fam.family_of(img.at(x, y), alpha) == fam.outline_family;
      bool contour = ink && dt.at(x, y) <= band - 0.5f + 1e-3f;
      if (ink && !contour && labels) {
        const int me = (*labels)[static_cast<size_t>(y) * w + x];
        for (int dy = -1; dy <= 1 && !contour; ++dy)
          for (int dx = -1; dx <= 1 && !contour; ++dx) {
            const int xx = x + dx, yy = y + dy;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            const int other = (*labels)[static_cast<size_t>(yy) * w + xx];
            contour = other >= 0 && me >= 0 && other != me;
          }
      }
      if (contour) is_band[static_cast<size_t>(y) * w + x] = 1;
      else q.push_back({x, y});
    }
  // Multi-source BFS from the inner colours into the band.
  std::vector<uint8_t> done(is_band.size(), 0);
  for (const auto& [x, y] : q) done[static_cast<size_t>(y) * w + x] = 1;
  while (!q.empty()) {
    const auto [x, y] = q.front();
    q.pop_front();
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        const int xx = x + dx, yy = y + dy;
        if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
        const size_t k = static_cast<size_t>(yy) * w + xx;
        if (!is_band[k] || done[k]) continue;
        if (labels && (*labels)[k] != (*labels)[static_cast<size_t>(y) * w + x]) continue;  // same part only
        done[k] = 1;
        out.at(xx, yy) = out.at(x, y);
        if (replaced) ++*replaced;
        q.push_back({xx, yy});
      }
  }
  return out;
}

BakeResult bake_texture(const Assembly& as, const PackResult& pk, const Input& in, const Layout& l,
                        const MaterialFamilies& fam, const BakeOptions& o) {
  BakeResult res;
  const int W = pk.width, H = pk.height;
  const SkinnedMesh& M = as.mesh;
  // Drawings without their silhouette ink, and depth buffers of the whole mesh per view.
  struct ViewData {
    const SourceView* v = nullptr;
    ImageRGBA clean;
    Camera cam;
    RenderTarget depth;
  };
  std::vector<ViewData> views;
  Mesh flat;
  flat.positions = M.positions;
  for (size_t t = 0; t + 2 < M.indices.size(); t += 3) flat.tris.push_back({M.indices[t], M.indices[t + 1], M.indices[t + 2]});
  flat.tri_part.assign(flat.tris.size(), 0);
  constexpr int kPpu = 4;
  for (const SourceView& v : in.views) {
    if (!v.aligned) continue;
    ViewData vd;
    vd.v = &v;
    int n = 0;
    std::vector<int> labels(static_cast<size_t>(v.aligned->width()) * v.aligned->height(), -1);
    for (const Part& p : in.parts) {
      auto it = p.masks.find(v.role);
      if (it == p.masks.end()) continue;
      for (int y = 0; y < it->second.height() && y < v.aligned->height(); ++y)
        for (int x = 0; x < it->second.width() && x < v.aligned->width(); ++x)
          if (it->second.get(x, y)) labels[static_cast<size_t>(y) * v.aligned->width() + x] = p.id;
    }
    vd.clean = remove_silhouette_ink(*v.aligned, fam, o.alpha_threshold, o.ink_band, &n, &labels);
    res.ink_band_px += n;
    vd.cam = camera_for_view(v.role, in.dims, kPpu);
    vd.depth = render_mesh_flat(flat, vd.cam);
    views.push_back(std::move(vd));
  }
  // Dominant non-ink colour of every part (Flat Palette).
  std::map<int, Rgba8> dominant;
  for (const Part& p : in.parts) {
    std::unordered_map<uint32_t, int> count;
    std::unordered_map<uint32_t, Rgba8> col;
    for (const auto& vd : views) {
      const Mask2D* m = part_mask(in, p.id, vd.v->role);
      if (!m) continue;
      for (int y = 0; y < m->height(); ++y)
        for (int x = 0; x < m->width(); ++x) {
          if (!m->get(x, y)) continue;
          const Rgba8 c = vd.clean.at(x, y);
          if (c.a < o.alpha_threshold || fam.family_of(c, o.alpha_threshold) == fam.outline_family) continue;
          ++count[pack_rgb(c)];
          col[pack_rgb(c)] = c;
        }
    }
    uint32_t best = 0;
    int bn = -1;
    for (const auto& [k, n] : count)
      if (n > bn || (n == bn && k < best)) best = k, bn = n;
    dominant[p.id] = bn > 0 ? Rgba8{col[best].r, col[best].g, col[best].b, 255} : Rgba8{128, 128, 128, 255};
  }
  ImageRGBA tex(W, H, {0, 0, 0, 0});
  std::vector<TexelMode> mode(static_cast<size_t>(W) * H, TexelMode::None);
  std::vector<int> island(static_cast<size_t>(W) * H, -1), owner(static_cast<size_t>(W) * H, -1);
  std::vector<vec3d> tnormal(static_cast<size_t>(W) * H);
  std::map<std::string, int> seen_by;
  const vec3d L = normalize(o.light);
  for (size_t t = 0; t + 2 < M.indices.size(); t += 3) {
    const uint32_t i0 = M.indices[t], i1 = M.indices[t + 1], i2 = M.indices[t + 2];
    const vec2d a{M.uvs[i0].x * W, M.uvs[i0].y * H}, b{M.uvs[i1].x * W, M.uvs[i1].y * H}, c{M.uvs[i2].x * W, M.uvs[i2].y * H};
    const double area = cross(b - a, c - a);
    if (std::abs(area) < 1e-12) continue;
    const vec3d nf = normalize(cross(M.positions[i1] - M.positions[i0], M.positions[i2] - M.positions[i0]));
    const int part = M.vertex_part[i0];
    const PartType type = part_type(in, part);
    const bool may_oppose = o.opposite_head_cloth || (type != PartType::Head && type != PartType::Cloth);
    const int isl = as.vertex_island[i0];
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x})))),
              x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y})))),
              y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const vec2d p{x + 0.5, y + 0.5};
        double w1 = cross(p - a, c - a) / area, w2 = cross(b - a, p - a) / area, w0 = 1 - w1 - w2;
        const double eps = -0.02;
        if (w0 < eps || w1 < eps || w2 < eps) continue;
        const size_t k = static_cast<size_t>(y) * W + x;
        if (mode[k] != TexelMode::None) continue;
        w0 = std::max(0.0, w0), w1 = std::max(0.0, w1), w2 = std::max(0.0, w2);
        const double s = w0 + w1 + w2;
        w0 /= s, w1 /= s, w2 /= s;
        const vec3d P = M.positions[i0] * w0 + M.positions[i1] * w1 + M.positions[i2] * w2;
        const vec3d ns = normalize(M.normals[i0] * w0 + M.normals[i1] * w1 + M.normals[i2] * w2);
        island[k] = isl;
        owner[k] = part;
        tnormal[k] = ns;
        // The most frontal view that sees this point and shows this part there.
        double best = 0.2;
        const ViewData* bv = nullptr;
        int bx = 0, by = 0;
        for (const auto& vd : views) {
          const double facing = dot(nf, -view_dir(vd.v->role));
          if (facing <= best) continue;
          const vec3d q = view_from_world(vd.v->role, in.dims, P);
          const int px = static_cast<int>(std::floor(q.x)), py = static_cast<int>(std::floor(q.y));
          if (px < 0 || py < 0 || px >= vd.clean.width() || py >= vd.clean.height()) continue;
          const int dx = std::clamp(static_cast<int>(std::floor(q.x * kPpu)), 0, vd.cam.width - 1);
          const int dy = std::clamp(static_cast<int>(std::floor(q.y * kPpu)), 0, vd.cam.height - 1);
          const float zb = vd.depth.depth[static_cast<size_t>(dy) * vd.cam.width + dx];
          if (!(q.z <= zb + 0.75)) continue;
          if (vd.clean.at(px, py).a < o.alpha_threshold) continue;
          const Mask2D* pm = part_mask(in, part, vd.v->role);
          if (pm && !near_mask(*pm, px, py)) continue;
          best = facing, bv = &vd, bx = px, by = py;
        }
        if (bv) {
          tex.at(x, y) = bv->clean.at(bx, by);
          mode[k] = TexelMode::Seen;
          ++seen_by[std::string(to_string(bv->v->role))];
          continue;
        }
        if (may_oppose)
          for (const auto& vd : views) {
            if (dot(nf, view_dir(vd.v->role)) <= 0.2) continue;  // faces away from this view
            const vec3d q = view_from_world(vd.v->role, in.dims, P);
            const int px = static_cast<int>(std::floor(q.x)), py = static_cast<int>(std::floor(q.y));
            const Mask2D* pm = part_mask(in, part, vd.v->role);
            if (!pm || !pm->get_or_false(px, py)) continue;
            const Rgba8 col = vd.clean.at(px, py);
            if (col.a < o.alpha_threshold || fam.family_of(col, o.alpha_threshold) == fam.outline_family) continue;
            tex.at(x, y) = col;
            mode[k] = TexelMode::Opposite;
            break;
          }
      }
  }
  // Edge Extend inside each island from seen/opposite non-ink texels; Flat Palette for islands with none.
  std::deque<size_t> q;
  std::vector<uint8_t> src(mode.size(), 0);
  for (size_t k = 0; k < mode.size(); ++k)
    if ((mode[k] == TexelMode::Seen || mode[k] == TexelMode::Opposite) &&
        fam.family_of(tex.at(static_cast<int>(k % W), static_cast<int>(k / W)), o.alpha_threshold) != fam.outline_family)
      q.push_back(k), src[k] = 1;
  while (!q.empty()) {
    const size_t k = q.front();
    q.pop_front();
    const int x = static_cast<int>(k % W), y = static_cast<int>(k / W);
    for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
      const int xx = x + dx, yy = y + dy;
      if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
      const size_t n = static_cast<size_t>(yy) * W + xx;
      if (island[n] != island[k] || mode[n] != TexelMode::None) continue;
      mode[n] = TexelMode::EdgeExtend;
      tex.at(xx, yy) = tex.at(x, y);
      q.push_back(n);
    }
  }
  for (size_t k = 0; k < mode.size(); ++k)
    if (island[k] >= 0 && mode[k] == TexelMode::None) {
      mode[k] = TexelMode::Flat;
      tex.at(static_cast<int>(k % W), static_cast<int>(k / W)) = dominant.count(owner[k]) ? dominant[owner[k]] : Rgba8{128, 128, 128, 255};
    }
  // Baked cel ramp (fixed light): for engines without custom shaders.
  if (o.baked_cel)
    for (size_t k = 0; k < mode.size(); ++k) {
      if (island[k] < 0) continue;
      Rgba8& c = tex.at(static_cast<int>(k % W), static_cast<int>(k / W));
      const double f = cel_factor(dot(tnormal[k], L), o.tones);
      c = {static_cast<uint8_t>(std::lround(c.r * f)), static_cast<uint8_t>(std::lround(c.g * f)), static_cast<uint8_t>(std::lround(c.b * f)), 255};
    }
  // Gutters: every empty texel takes the nearest island colour (no bleeding under filtering).
  std::vector<uint8_t> filled(mode.size(), 0);
  for (size_t k = 0; k < mode.size(); ++k)
    if (island[k] >= 0) q.push_back(k), filled[k] = 1;
  while (!q.empty()) {
    const size_t k = q.front();
    q.pop_front();
    const int x = static_cast<int>(k % W), y = static_cast<int>(k / W);
    for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
      const int xx = x + dx, yy = y + dy;
      if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
      const size_t n = static_cast<size_t>(yy) * W + xx;
      if (filled[n]) continue;
      filled[n] = 1;
      tex.at(xx, yy) = tex.at(x, y);
      q.push_back(n);
    }
  }
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) tex.at(x, y).a = 255;
  // Shares.
  int total = 0, opp = 0, ext = 0, flat_n = 0;
  for (size_t k = 0; k < mode.size(); ++k) {
    if (island[k] < 0) continue;
    ++total;
    opp += mode[k] == TexelMode::Opposite;
    ext += mode[k] == TexelMode::EdgeExtend;
    flat_n += mode[k] == TexelMode::Flat;
  }
  const double tot = std::max(1, total);
  for (const auto& [v, n] : seen_by) res.share["seen:" + v] = n / tot;
  res.share["opposite"] = opp / tot;
  res.share["edge_extend"] = ext / tot;
  res.share["flat_palette"] = flat_n / tot;
  const double inferred = (opp + ext + flat_n) / tot;
  res.messages.push_back(Message{"lowpoly.inferred_surface",
                                 std::to_string(static_cast<int>(std::lround(inferred * 100))) +
                                     " % of the surface is not seen in any drawing (opposite projection, edge extend, flat colour)",
                                 Severity::Info, {{"share", res.share}}});
  if (flat_n / tot > 0.10)
    res.messages.push_back(Message{"lowpoly.flat_palette_large", "more than 10 % of the surface is a flat part colour",
                                   Severity::Warning, {{"share", flat_n / tot}}});
  if (res.ink_band_px > 0)
    res.messages.push_back(Message{"lowpoly.ink_band_removed",
                                   std::to_string(res.ink_band_px) +
                                       " contour ink pixels (silhouette and part overlaps) were replaced by the inner colour of their part (the outline style draws these lines)",
                                   Severity::Info, {{"px", res.ink_band_px}}});
  if (o.baked_cel)
    res.messages.push_back(Message{"lowpoly.baked_lighting",
                                   "the cel ramp is baked into the texture with a fixed light; it will not follow the engine's lights",
                                   Severity::Info, {}});
  (void)l;
  res.mode = std::move(mode);
  res.texture = std::make_shared<const ImageRGBA>(std::move(tex));
  return res;
}

}  // namespace pixel3d::lowpoly
