#pragma once
// Colour regions of one aligned view: 4-connected pixels of one material
// family. Outline pixels separate regions and belong to none; shading inside a
// family never splits a region.
#include <vector>

#include "pixel3d/image/mask.hpp"
#include "pixel3d/image/palette.hpp"

namespace pixel3d {

struct Region {
  int id = 0;
  int family = -1;
  std::vector<int> pixels;  // v * w + u
  Mask2D::Box box;
  // Reaches transparency (or the image border) directly or through one outline pixel.
  bool touches_silhouette = false;
  // Does not touch the silhouette and is surrounded by other regions (a rim).
  bool enclosed = false;
  double mean_luminance = 0;
  std::vector<int> neighbours;  // regions adjacent directly or across one outline pixel
};

struct RegionMap {
  int w = 0, h = 0;
  std::vector<int> label;  // region id per pixel; -1 = transparent or outline
  std::vector<Region> regions;
  int at(int u, int v) const { return label[static_cast<size_t>(v) * w + u]; }
};

RegionMap compute_regions(const ImageRGBA& aligned, const MaterialFamilies& fam, uint8_t alpha_threshold = 128);

}  // namespace pixel3d
