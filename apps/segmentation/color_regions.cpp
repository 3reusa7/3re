#include "segmentation/color_regions.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace p3dseg {
namespace {

int distance(QRgb a, QRgb b) {
  return std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
}

// 4-connected region of colours within `tolerance` of the seed; transparent pixels never join.
std::vector<int> flood(const QImage& img, QPoint p, int tolerance, std::vector<uint8_t>& visited) {
  const int w = img.width(), h = img.height();
  std::vector<int> out;
  if (p.x() < 0 || p.y() < 0 || p.x() >= w || p.y() >= h) return out;
  const QRgb seed = img.pixel(p);
  if (qAlpha(seed) < 16) return out;
  std::vector<int> stack{p.y() * w + p.x()};
  visited[size_t(p.y()) * w + p.x()] = 1;
  while (!stack.empty()) {
    const int i = stack.back();
    stack.pop_back();
    out.push_back(i);
    const int x = i % w, y = i / w;
    const int nx[4] = {x + 1, x - 1, x, x}, ny[4] = {y, y, y + 1, y - 1};
    for (int k = 0; k < 4; ++k) {
      if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
      const size_t j = size_t(ny[k]) * w + nx[k];
      if (visited[j]) continue;
      const QRgb c = reinterpret_cast<const QRgb*>(img.constScanLine(ny[k]))[nx[k]];
      if (qAlpha(c) < 16 || distance(c, seed) > tolerance) continue;
      visited[j] = 1;
      stack.push_back(int(j));
    }
  }
  return out;
}

QImage to_mask(QSize s, const std::vector<int>& pixels) {
  QImage m(s, QImage::Format_Grayscale8);
  m.fill(0);
  for (int i : pixels) m.scanLine(i / s.width())[i % s.width()] = 255;
  return m;
}

}  // namespace

QImage color_region_at(const QImage& image, QPoint p, int tolerance) {
  const QImage img = image.convertToFormat(QImage::Format_ARGB32);
  std::vector<uint8_t> visited(size_t(img.width()) * img.height(), 0);
  return to_mask(img.size(), flood(img, p, tolerance, visited));
}

QList<QImage> suggest_color_regions(const QImage& image, int minArea, int tolerance) {
  const QImage img = image.convertToFormat(QImage::Format_ARGB32);
  const int w = img.width(), h = img.height();
  std::vector<uint8_t> visited(size_t(w) * h, 0);
  struct Found {
    std::vector<int> pixels;
    bool touches_border;
  };
  std::vector<Found> found;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (visited[size_t(y) * w + x]) continue;
      auto px = flood(img, QPoint(x, y), tolerance, visited);
      if (int(px.size()) < minArea) continue;
      bool border = false;
      for (int i : px) {
        const int ix = i % w, iy = i / w;
        if (ix == 0 || iy == 0 || ix == w - 1 || iy == h - 1) {
          border = true;
          break;
        }
      }
      found.push_back({std::move(px), border});
    }
  // The largest region touching the border of an opaque picture is its background.
  if (!found.empty()) {
    auto bg = std::max_element(found.begin(), found.end(), [](const Found& a, const Found& b) {
      return (a.touches_border ? a.pixels.size() : 0) < (b.touches_border ? b.pixels.size() : 0);
    });
    if (bg->touches_border) found.erase(bg);
  }
  std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.pixels.size() > b.pixels.size(); });
  QList<QImage> out;
  for (size_t i = 0; i < found.size() && i < 24; ++i) out << to_mask(img.size(), found[i].pixels);
  return out;
}

QImage clean_mask(const QImage& mask) {
  const QImage in = mask.convertToFormat(QImage::Format_Grayscale8);
  const int w = in.width(), h = in.height();
  std::vector<uint8_t> inside(size_t(w) * h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) inside[size_t(y) * w + x] = in.constScanLine(y)[x] > 127;
  // Outside = background reachable from the border (4-connected); the rest of the background is a hole.
  std::vector<uint8_t> outside(inside.size(), 0);
  std::vector<int> stack;
  auto seed = [&](int x, int y) {
    const size_t i = size_t(y) * w + x;
    if (!inside[i] && !outside[i]) {
      outside[i] = 1;
      stack.push_back(int(i));
    }
  };
  for (int x = 0; x < w; ++x) seed(x, 0), seed(x, h - 1);
  for (int y = 0; y < h; ++y) seed(0, y), seed(w - 1, y);
  while (!stack.empty()) {
    const int i = stack.back();
    stack.pop_back();
    const int x = i % w, y = i / w;
    if (x > 0) seed(x - 1, y);
    if (x < w - 1) seed(x + 1, y);
    if (y > 0) seed(x, y - 1);
    if (y < h - 1) seed(x, y + 1);
  }
  for (size_t i = 0; i < inside.size(); ++i) inside[i] = inside[i] || !outside[i];
  // Islands (8-connected) smaller than 0.2 % of the largest one are specks.
  std::vector<int> label(inside.size(), 0), sizes{0};
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const size_t s0 = size_t(y) * w + x;
      if (!inside[s0] || label[s0]) continue;
      const int id = int(sizes.size());
      int n = 0;
      stack.assign(1, int(s0));
      label[s0] = id;
      while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        ++n;
        const int px = i % w, py = i / w;
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            const int nx = px + dx, ny = py + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            const size_t j = size_t(ny) * w + nx;
            if (inside[j] && !label[j]) {
              label[j] = id;
              stack.push_back(int(j));
            }
          }
      }
      sizes.push_back(n);
    }
  const int largest = sizes.size() > 1 ? *std::max_element(sizes.begin() + 1, sizes.end()) : 0;
  QImage out(w, h, QImage::Format_Grayscale8);
  for (int y = 0; y < h; ++y) {
    uchar* d = out.scanLine(y);
    for (int x = 0; x < w; ++x) {
      const int l = label[size_t(y) * w + x];
      d[x] = l && sizes[size_t(l)] * 500 >= largest ? 255 : 0;
    }
  }
  return out;
}

QImage apply_cutout(const QImage& original, const QImage& mask) {
  QImage out = original.convertToFormat(QImage::Format_ARGB32);
  const QImage m = mask.size() == out.size() ? mask : mask.scaled(out.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  const QImage g = m.convertToFormat(QImage::Format_Grayscale8);
  for (int y = 0; y < out.height(); ++y) {
    QRgb* d = reinterpret_cast<QRgb*>(out.scanLine(y));
    const uchar* a = g.constScanLine(y);
    for (int x = 0; x < out.width(); ++x) d[x] = qRgba(qRed(d[x]), qGreen(d[x]), qBlue(d[x]), qAlpha(d[x]) * a[x] / 255);
  }
  return out;
}

}  // namespace p3dseg
