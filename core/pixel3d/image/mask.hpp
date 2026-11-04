#pragma once
#include <cstdint>
#include <vector>

#include "pixel3d/image/image.hpp"

namespace pixel3d {

class Mask2D {
 public:
  struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) × [y0, y1)
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool operator==(const Box&) const = default;
  };

  Mask2D() = default;
  Mask2D(int w, int h) : w_(w), h_(h), bits_(static_cast<size_t>(w) * static_cast<size_t>(h), 0) {}

  int width() const { return w_; }
  int height() const { return h_; }
  bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
  bool get(int x, int y) const { return bits_[static_cast<size_t>(y) * w_ + x] != 0; }
  // Out-of-bounds reads return false (handy for neighbourhood scans).
  bool get_or_false(int x, int y) const { return in_bounds(x, y) && get(x, y); }
  void set(int x, int y, bool v) { bits_[static_cast<size_t>(y) * w_ + x] = v ? 1 : 0; }
  int count() const;
  Box bbox() const;
  bool operator==(const Mask2D&) const = default;

 private:
  int w_ = 0, h_ = 0;
  std::vector<uint8_t> bits_;
};

Mask2D opaque_mask(const ImageRGBA& img, uint8_t alpha_threshold = 128);
double mask_iou(const Mask2D& a, const Mask2D& b);  // 1.0 when both are empty
int mask_xor_count(const Mask2D& a, const Mask2D& b);
Mask2D flip_x(const Mask2D& m);
ImageRGBA crop(const ImageRGBA& img, Mask2D::Box box);  // regions outside the image are transparent
Mask2D crop(const Mask2D& m, Mask2D::Box box);

}  // namespace pixel3d
