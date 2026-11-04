#include "pixel3d/image/pixel_scale.hpp"

#include <algorithm>
#include <numeric>
#include <set>

namespace pixel3d {
namespace {

Rgba8 canonical(Rgba8 c) { return c.a == 0 ? Rgba8{} : c; }

// Positions p (1..n-1) where pixel p-1 differs from pixel p along an axis.
// Block uniformity holds iff every change position is a block boundary.
struct Changes {
  std::set<int> xs, ys;
};

Changes find_changes(const ImageRGBA& img) {
  Changes c;
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x) {
      const Rgba8 p = canonical(img.at(x, y));
      if (x > 0 && canonical(img.at(x - 1, y)) != p) c.xs.insert(x);
      if (y > 0 && canonical(img.at(x, y - 1)) != p) c.ys.insert(y);
    }
  return c;
}

// gcd of differences between change positions; 0 if fewer than two changes.
int spacing_gcd(const std::set<int>& pos) {
  int g = 0, prev = -1;
  for (int p : pos) {
    if (prev >= 0) g = std::gcd(g, p - prev);
    prev = p;
  }
  return g;
}

int block_count(int extent, int k, int off) { return (off > 0 ? 1 : 0) + (extent - off + k - 1) / k; }
int block_start(int index, int k, int off) { return off > 0 ? (index == 0 ? 0 : off + (index - 1) * k) : index * k; }

}  // namespace

int change_spacing_gcd(const ImageRGBA& img) {
  const Changes c = find_changes(img);
  return std::gcd(spacing_gcd(c.xs), spacing_gcd(c.ys));
}

int scale_from_gcd(int g, int max_k) {
  for (int d = std::min(g, max_k); d >= 2; --d)
    if (g % d == 0) return d;
  return 1;
}

ScaleInfo scale_with_k(const ImageRGBA& img, int k) {
  if (k <= 1) return ScaleInfo{};
  const Changes c = find_changes(img);
  return ScaleInfo{k, c.xs.empty() ? 0 : *c.xs.begin() % k, c.ys.empty() ? 0 : *c.ys.begin() % k};
}

ScaleInfo detect_pixel_scale(const ImageRGBA& img, int max_k) {
  return scale_with_k(img, scale_from_gcd(change_spacing_gcd(img), max_k));
}

int block_source(int block_index, int k, int offset) { return k <= 1 ? block_index : block_start(block_index, k, offset); }

ImageRGBA downscale_nearest(const ImageRGBA& img, const ScaleInfo& s) {
  if (s.k <= 1) return img;
  const int w = block_count(img.width(), s.k, s.ox), h = block_count(img.height(), s.k, s.oy);
  ImageRGBA out(w, h);
  for (int by = 0; by < h; ++by)
    for (int bx = 0; bx < w; ++bx) out.at(bx, by) = img.at(block_start(bx, s.k, s.ox), block_start(by, s.k, s.oy));
  return out;
}

}  // namespace pixel3d
