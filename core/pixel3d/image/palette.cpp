#include "pixel3d/image/palette.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <set>

namespace pixel3d {
namespace {

constexpr double kNeutralChroma = 0.04;      // below: grey-ish
constexpr double kOutlineMaxL = 0.32;        // outline candidates are dark...
constexpr double kOutlineMaxChroma = 0.08;   // ...and desaturated
constexpr double kOutlineThinRatio = 0.7;    // fraction of pixels with ≤ 2 same-colour 4-neighbours
constexpr double kHueGapDeg = 25.0;          // single-linkage gap between ramp members
constexpr double kNeutralLGap = 0.2;

uint32_t pack(Rgba8 c) { return (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b; }

double srgb_to_linear(uint8_t v) {
  const double c = v / 255.0;
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// Single-linkage clustering of sorted 1D values; returns a cluster id per input
// (inputs must be sorted ascending). `circular` treats values as degrees.
std::vector<int> link_sorted(const std::vector<double>& v, double gap, bool circular) {
  std::vector<int> id(v.size(), 0);
  if (v.empty()) return id;
  int cur = 0;
  for (size_t i = 1; i < v.size(); ++i) {
    if (v[i] - v[i - 1] > gap) ++cur;
    id[i] = cur;
  }
  if (circular && cur > 0 && (v.front() + 360.0 - v.back()) <= gap)
    for (auto& x : id)
      if (x == cur) x = 0;
  return id;
}

}  // namespace

double relative_luminance(Rgba8 c) {
  return 0.2126 * srgb_to_linear(c.r) + 0.7152 * srgb_to_linear(c.g) + 0.0722 * srgb_to_linear(c.b);
}

OkLch to_oklch(Rgba8 c) {
  const double r = srgb_to_linear(c.r), g = srgb_to_linear(c.g), b = srgb_to_linear(c.b);
  const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
  const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
  const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
  const double L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
  const double A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
  const double B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
  double h = std::atan2(B, A) * 180.0 / std::numbers::pi;
  if (h < 0) h += 360.0;
  return {L, std::hypot(A, B), h};
}

int MaterialFamilies::entry_of(Rgba8 c) const {
  const auto it = index.find(pack(c));
  return it == index.end() ? -1 : it->second;
}

int MaterialFamilies::family_of(Rgba8 c, uint8_t alpha_threshold) const {
  if (c.a < alpha_threshold) return -1;
  const int e = entry_of(c);
  return e < 0 ? -1 : entries[e].family;
}

MaterialFamilies analyze_palette(std::span<const ImageRGBA* const> views, uint8_t alpha_threshold) {
  MaterialFamilies f;
  // 1. Collect colours with counts and "thin" pixel counts.
  std::vector<int> thin;
  for (const ImageRGBA* img : views)
    for (int y = 0; y < img->height(); ++y)
      for (int x = 0; x < img->width(); ++x) {
        const Rgba8 c = img->at(x, y);
        if (c.a < alpha_threshold) continue;
        auto [it, inserted] = f.index.try_emplace(pack(c), static_cast<int>(f.entries.size()));
        if (inserted) {
          f.entries.push_back(PaletteEntry{Rgba8{c.r, c.g, c.b, 255}, 0, -1});
          thin.push_back(0);
        }
        const int e = it->second;
        ++f.entries[e].count;
        int same = 0;
        const int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
          const int nx = x + dx[k], ny = y + dy[k];
          if (img->in_bounds(nx, ny) && img->at(nx, ny).a >= alpha_threshold && pack(img->at(nx, ny)) == pack(c)) ++same;
        }
        if (same <= 2) ++thin[e];
      }

  // 2. Outline colours: dark, desaturated, drawn as thin lines.
  std::vector<OkLch> lch(f.entries.size());
  std::vector<int> chromatic, neutral;
  bool any_outline = false;
  for (size_t i = 0; i < f.entries.size(); ++i) {
    lch[i] = to_oklch(f.entries[i].color);
    const bool outline = lch[i].L < kOutlineMaxL && lch[i].C < kOutlineMaxChroma &&
                         thin[i] >= kOutlineThinRatio * f.entries[i].count;
    if (outline) {
      f.entries[i].family = -2;  // placeholder, assigned below
      any_outline = true;
    } else if (lch[i].C < kNeutralChroma) {
      neutral.push_back(static_cast<int>(i));
    } else {
      chromatic.push_back(static_cast<int>(i));
    }
  }

  // 3. Chromatic colours: single-linkage on hue.
  std::sort(chromatic.begin(), chromatic.end(), [&](int a, int b) { return lch[a].h < lch[b].h; });
  std::vector<double> hues;
  for (int i : chromatic) hues.push_back(lch[i].h);
  const auto hue_ids = link_sorted(hues, kHueGapDeg, true);
  int next = 0;
  std::map<int, int> remap;
  for (size_t k = 0; k < chromatic.size(); ++k) {
    auto [it, ins] = remap.try_emplace(hue_ids[k], next);
    if (ins) ++next;
    f.entries[chromatic[k]].family = it->second;
  }

  // 4. Neutral colours: single-linkage on lightness.
  std::sort(neutral.begin(), neutral.end(), [&](int a, int b) { return lch[a].L < lch[b].L; });
  std::vector<double> ls;
  for (int i : neutral) ls.push_back(lch[i].L);
  const auto l_ids = link_sorted(ls, kNeutralLGap, false);
  remap.clear();
  for (size_t k = 0; k < neutral.size(); ++k) {
    auto [it, ins] = remap.try_emplace(l_ids[k], next);
    if (ins) ++next;
    f.entries[neutral[k]].family = it->second;
  }

  if (any_outline) {
    f.outline_family = next++;
    for (auto& e : f.entries)
      if (e.family == -2) e.family = f.outline_family;
  }
  f.family_count = next;
  return f;
}

}  // namespace pixel3d
