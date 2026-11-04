#include "pixel3d/texture/atlas.hpp"

#include <algorithm>
#include <array>
#include <deque>

namespace pixel3d {

const AtlasCell* Atlas::cell(ViewRole r) const {
  for (const AtlasCell& c : cells)
    if (c.role == r) return &c;
  return nullptr;
}

vec2d Atlas::uv(ViewRole r, double u, double v) const {
  const AtlasCell* c = cell(r);
  if (!c || !image) return {0, 0};
  return {(c->x0 + u) / image->width(), (c->y0 + v) / image->height()};
}

Atlas build_atlas(const std::vector<SourceView>& views, const MaterialFamilies& fam, int gutter, int dilate,
                  uint8_t alpha_threshold) {
  constexpr int kSwatch = 2;
  Atlas atlas;
  int x = gutter, row_h = 0;
  for (ViewRole r : kAllViewRoles)
    for (const SourceView& v : views) {
      if (v.role != r || !v.aligned) continue;
      atlas.cells.push_back(AtlasCell{r, x, gutter, v.aligned->width(), v.aligned->height()});
      x += v.aligned->width() + 2 * gutter;
      row_h = std::max(row_h, v.aligned->height());
    }
  const int views_w = x - gutter;
  const int swatch_y = row_h + 3 * gutter;
  const int swatch_w = fam.family_count * (kSwatch + 2 * gutter);
  const int W = std::max(views_w, swatch_w), H = fam.family_count > 0 ? swatch_y + kSwatch + gutter : row_h + 2 * gutter;
  ImageRGBA img(W, H);
  std::vector<uint8_t> opaque(static_cast<size_t>(W) * H, 0);
  std::vector<int> owner(opaque.size(), -1);  // cell (or swatch) id that may write the pixel
  auto idx = [&](int px, int py) { return static_cast<size_t>(py) * W + px; };

  int region_id = 0;
  std::vector<std::array<int, 4>> limits;  // per owner: x0 y0 x1 y1 (inclusive-exclusive) incl. gutter
  for (const AtlasCell& c : atlas.cells) {
    const SourceView* src = nullptr;
    for (const SourceView& v : views)
      if (v.role == c.role) src = &v;
    for (int py = 0; py < c.h; ++py)
      for (int px = 0; px < c.w; ++px) {
        const Rgba8 p = src->aligned->at(px, py);
        if (p.a < alpha_threshold) continue;
        img.at(c.x0 + px, c.y0 + py) = p;
        opaque[idx(c.x0 + px, c.y0 + py)] = 1;
        owner[idx(c.x0 + px, c.y0 + py)] = region_id;
      }
    limits.push_back({c.x0 - gutter, c.y0 - gutter, c.x0 + c.w + gutter, c.y0 + c.h + gutter});
    ++region_id;
  }
  // Swatches: the most used colour of each family.
  for (int f = 0; f < fam.family_count; ++f) {
    const PaletteEntry* best = nullptr;
    for (const PaletteEntry& e : fam.entries)
      if (e.family == f && (!best || e.count > best->count)) best = &e;
    const int sx = gutter + f * (kSwatch + 2 * gutter);
    for (int py = 0; py < kSwatch; ++py)
      for (int px = 0; px < kSwatch; ++px) {
        img.at(sx + px, swatch_y + py) = best ? best->color : Rgba8{128, 128, 128, 255};
        opaque[idx(sx + px, swatch_y + py)] = 1;
        owner[idx(sx + px, swatch_y + py)] = region_id;
      }
    limits.push_back({sx - gutter, swatch_y - gutter, sx + kSwatch + gutter, swatch_y + kSwatch + gutter});
    atlas.swatch_uv[f] = {(sx + kSwatch / 2.0) / W, (swatch_y + kSwatch / 2.0) / H};
    ++region_id;
  }

  // Dilation: breadth-first from opaque pixels, never leaving the owner's cell + gutter.
  std::deque<std::pair<int, int>> q;
  std::vector<int> dist(opaque.size(), -1);
  for (int py = 0; py < H; ++py)
    for (int px = 0; px < W; ++px)
      if (opaque[idx(px, py)]) dist[idx(px, py)] = 0, q.emplace_back(px, py);
  const int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
  while (!q.empty()) {
    const auto [px, py] = q.front();
    q.pop_front();
    const int d = dist[idx(px, py)];
    if (d >= dilate) continue;
    const auto& lim = limits[owner[idx(px, py)]];
    for (int k = 0; k < 4; ++k) {
      const int nx = px + dx[k], ny = py + dy[k];
      if (nx < 0 || ny < 0 || nx >= W || ny >= H || nx < lim[0] || ny < lim[1] || nx >= lim[2] || ny >= lim[3]) continue;
      if (dist[idx(nx, ny)] >= 0) continue;
      dist[idx(nx, ny)] = d + 1;
      owner[idx(nx, ny)] = owner[idx(px, py)];
      Rgba8 c = img.at(px, py);
      c.a = 255;
      img.at(nx, ny) = c;
      q.emplace_back(nx, ny);
    }
  }
  atlas.image = std::make_shared<const ImageRGBA>(std::move(img));
  return atlas;
}

}  // namespace pixel3d
