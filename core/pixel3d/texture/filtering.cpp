#include "pixel3d/texture/filtering.hpp"

#include <algorithm>
#include <deque>
#include <memory>

namespace pixel3d {

TextureResult pad_for_filtering(const TextureResult& tex, int scale) {
  TextureResult out = tex;
  if (!tex.mesh.texture) return out;
  const ImageRGBA& src = *tex.mesh.texture;
  const int W = src.width(), H = src.height();
  ImageRGBA filled = src;
  // Breadth first from every texel that has a colour (the atlas copy already
  // dilated a few texels into each gutter); the rest take the nearest colour.
  std::vector<uint8_t> done(static_cast<size_t>(W) * H, 0);
  std::deque<std::pair<int, int>> q;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (src.at(x, y).a == 255) done[static_cast<size_t>(y) * W + x] = 1, q.emplace_back(x, y);
  const int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
  while (!q.empty()) {
    const auto [x, y] = q.front();
    q.pop_front();
    for (int k = 0; k < 4; ++k) {
      const int nx = x + dx[k], ny = y + dy[k];
      if (nx < 0 || ny < 0 || nx >= W || ny >= H || done[static_cast<size_t>(ny) * W + nx]) continue;
      done[static_cast<size_t>(ny) * W + nx] = 1;
      filled.at(nx, ny) = filled.at(x, y);
      q.emplace_back(nx, ny);
    }
  }
  const int k = std::clamp(scale, 1, 16);
  ImageRGBA big(W * k, H * k);
  for (int y = 0; y < H * k; ++y)
    for (int x = 0; x < W * k; ++x) {
      Rgba8 c = filled.at(x / k, y / k);
      c.a = 255;
      big.at(x, y) = c;
    }
  auto image = std::make_shared<const ImageRGBA>(std::move(big));
  out.mesh.texture = image;
  out.atlas.image = image;
  return out;
}

}  // namespace pixel3d
