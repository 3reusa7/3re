#pragma once
// RGBA8 images and PNG I/O. Pixels are never resampled with filtering anywhere
// in Pixel3D: source artwork is copied or indexed, never blurred.
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/common/file_io.hpp"

namespace pixel3d {

struct Rgba8 {
  uint8_t r = 0, g = 0, b = 0, a = 0;
  bool operator==(const Rgba8&) const = default;
};

class ImageRGBA {
 public:
  ImageRGBA() = default;
  ImageRGBA(int w, int h, Rgba8 fill = {0, 0, 0, 0});

  int width() const { return w_; }
  int height() const { return h_; }
  bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
  Rgba8& at(int x, int y) { return px_[static_cast<size_t>(y) * w_ + x]; }
  const Rgba8& at(int x, int y) const { return px_[static_cast<size_t>(y) * w_ + x]; }
  std::span<const Rgba8> pixels() const { return px_; }
  bool operator==(const ImageRGBA&) const = default;

 private:
  int w_ = 0, h_ = 0;
  std::vector<Rgba8> px_;
};

// Source image formats. The format is taken from the file's signature; TGA has
// none, so it is only accepted when the file name ends in .tga. Aseprite and PSD
// are layered sources (image/layered.hpp): their image is the flattened first
// frame / the stored composite.
enum class ImageFormat { Unknown, Png, Jpeg, Bmp, Gif, Tga, Webp, Aseprite, Psd };
std::string_view to_string(ImageFormat f);      // "PNG", "JPEG", ...
std::string_view file_extension(ImageFormat f);  // "png", "jpg", ... ("img" for Unknown)
ImageFormat sniff_image_format(std::span<const uint8_t> bytes, std::string_view name_hint = {});

Expected<ImageRGBA> load_png(const std::filesystem::path& path);
// Refuses images beyond limits::kMaxImageSide / kMaxImagePixels (image.too_large)
// before allocating anything. PNG only (used to verify exported textures).
Expected<ImageRGBA> decode_png(std::span<const uint8_t> bytes);
// Any supported source format (PNG, JPEG, BMP, GIF first frame, TGA by name,
// WebP (first frame), Aseprite (first frame, flattened), PSD (composite)), with
// the same limits. Errors: image.unsupported_format, image.decode_failed,
// image.too_large.
Expected<ImageRGBA> decode_image(std::span<const uint8_t> bytes, std::string_view name_hint = {});
std::vector<uint8_t> encode_png(const ImageRGBA& img);
// Atomic, checked write (write_file_atomic). Returns a Message with code "ok"
// (Severity::Info) on success, io.write_failed otherwise.
Message save_png(const ImageRGBA& img, const std::filesystem::path& path);

ImageRGBA flip_x(const ImageRGBA& img);

}  // namespace pixel3d
