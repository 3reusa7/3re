#include "pixel3d/image/mask.hpp"

#include <algorithm>
#include <cassert>

namespace pixel3d {

int Mask2D::count() const {
  return static_cast<int>(std::count(bits_.begin(), bits_.end(), uint8_t{1}));
}

Mask2D::Box Mask2D::bbox() const {
  Box b{w_, h_, 0, 0};
  for (int y = 0; y < h_; ++y)
    for (int x = 0; x < w_; ++x)
      if (get(x, y)) {
        b.x0 = std::min(b.x0, x);
        b.y0 = std::min(b.y0, y);
        b.x1 = std::max(b.x1, x + 1);
        b.y1 = std::max(b.y1, y + 1);
      }
  if (b.x1 <= b.x0) return Box{};
  return b;
}

Mask2D opaque_mask(const ImageRGBA& img, uint8_t alpha_threshold) {
  Mask2D m(img.width(), img.height());
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x) m.set(x, y, img.at(x, y).a >= alpha_threshold);
  return m;
}

double mask_iou(const Mask2D& a, const Mask2D& b) {
  assert(a.width() == b.width() && a.height() == b.height());
  int inter = 0, uni = 0;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) {
      const bool p = a.get(x, y), q = b.get(x, y);
      inter += (p && q) ? 1 : 0;
      uni += (p || q) ? 1 : 0;
    }
  return uni == 0 ? 1.0 : static_cast<double>(inter) / uni;
}

int mask_xor_count(const Mask2D& a, const Mask2D& b) {
  assert(a.width() == b.width() && a.height() == b.height());
  int n = 0;
  for (int y = 0; y < a.height(); ++y)
    for (int x = 0; x < a.width(); ++x) n += a.get(x, y) != b.get(x, y) ? 1 : 0;
  return n;
}

Mask2D flip_x(const Mask2D& m) {
  Mask2D out(m.width(), m.height());
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) out.set(m.width() - 1 - x, y, m.get(x, y));
  return out;
}

ImageRGBA crop(const ImageRGBA& img, Mask2D::Box box) {
  ImageRGBA out(box.width(), box.height());
  for (int y = 0; y < box.height(); ++y)
    for (int x = 0; x < box.width(); ++x)
      if (img.in_bounds(box.x0 + x, box.y0 + y)) out.at(x, y) = img.at(box.x0 + x, box.y0 + y);
  return out;
}

Mask2D crop(const Mask2D& m, Mask2D::Box box) {
  Mask2D out(box.width(), box.height());
  for (int y = 0; y < box.height(); ++y)
    for (int x = 0; x < box.width(); ++x) out.set(x, y, m.get_or_false(box.x0 + x, box.y0 + y));
  return out;
}

}  // namespace pixel3d
