#pragma once
// Layered source files (1.2.0): Aseprite (.aseprite / .ase) and Photoshop (.psd).
// - Aseprite: the first frame, flattened exactly as Aseprite exports it (visible layers,
//   layer and cel opacity, normal blending), is the view's image.
// - PSD: the composite image stored in the file is the view's image.
// Visible raster layers are returned as well: the Session offers them as SUGGESTED part
// masks named after the layer (the user accepts or rejects them). Anything that cannot be
// read as a raster layer (text, smart object, adjustment or fill layer, tilemap, other
// blend modes) is reported in `notes`, never as a failed import. Files are only read.
// Every size is checked against limits:: before anything is allocated.
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/image/image.hpp"
#include "pixel3d/image/mask.hpp"

namespace pixel3d {

struct SourceLayer {
  std::string name;     // as stored (UTF-8)
  std::string path;     // with its groups: "group/sub/name"
  bool visible = true;  // the layer and every group above it are visible
  Mask2D::Box rect;     // canvas pixels covered by `pixels` (may be empty)
  ImageRGBA pixels;     // rect-sized, straight alpha, layer and cel opacity applied
};

struct LayeredImage {
  ImageFormat format = ImageFormat::Unknown;
  ImageRGBA composite;               // canvas-sized
  std::vector<SourceLayer> layers;   // raster layers, bottom to top
  std::vector<Message> notes;        // what was not read, and how the composite was made
};

Expected<LayeredImage> decode_aseprite(std::span<const uint8_t> bytes);
Expected<LayeredImage> decode_psd(std::span<const uint8_t> bytes);
// By signature: Aseprite or PSD; anything else is image.not_layered.
Expected<LayeredImage> decode_layered(std::span<const uint8_t> bytes);

}  // namespace pixel3d
