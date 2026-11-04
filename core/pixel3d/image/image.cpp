#include "pixel3d/image/image.hpp"

#include <algorithm>
#include <climits>
#include <cstring>

#include "pixel3d/common/limits.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/layered.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#include <webp/decode.h>
#include <webp/demux.h>

namespace pixel3d {

ImageRGBA::ImageRGBA(int w, int h, Rgba8 fill)
    : w_(w), h_(h), px_(static_cast<size_t>(w) * static_cast<size_t>(h), fill) {}

namespace {

bool starts_with(std::span<const uint8_t> b, std::initializer_list<uint8_t> sig) {
  if (b.size() < sig.size()) return false;
  return std::equal(sig.begin(), sig.end(), b.begin());
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
  if (s.size() < suffix.size()) return false;
  for (size_t i = 0; i < suffix.size(); ++i) {
    char c = s[s.size() - suffix.size() + i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != suffix[i]) return false;
  }
  return true;
}

// Decodes with stb_image after the format is known (the signature was checked).
Expected<ImageRGBA> decode_with_stb(std::span<const uint8_t> bytes, ImageFormat format) {
  const std::string what = std::string(to_string(format));
  if (bytes.size() > limits::kMaxImageFileBytes || bytes.size() > static_cast<size_t>(INT_MAX))
    return make_error("image.too_large", what + " data is too large", {{"bytes", bytes.size()}});
  const int len = static_cast<int>(bytes.size());
  int w = 0, h = 0, n = 0;
  // Header first: refuse oversized images before any pixel buffer is allocated.
  if (!stbi_info_from_memory(bytes.data(), len, &w, &h, &n)) {
    const std::string why = stbi_failure_reason();
    if (why == "too large") return make_error("image.too_large", "image dimensions are too large");
    return make_error("image.decode_failed", "not a readable " + what + " image: " + why, {{"format", what}});
  }
  if (w <= 0 || h <= 0 || w > limits::kMaxImageSide || h > limits::kMaxImageSide ||
      static_cast<int64_t>(w) * h > limits::kMaxImagePixels)
    return make_error("image.too_large",
                      "image is " + std::to_string(w) + "x" + std::to_string(h) + " px; the limit is " +
                          std::to_string(limits::kMaxImageSide) + " px per side and " +
                          std::to_string(limits::kMaxImagePixels) + " pixels",
                      {{"width", w}, {"height", h}});
  int dw = 0, dh = 0;
  // GIF: stb_image returns the first frame.
  stbi_uc* data = stbi_load_from_memory(bytes.data(), len, &dw, &dh, &n, 4);
  if (!data)
    return make_error("image.decode_failed", "not a readable " + what + " image: " + stbi_failure_reason(),
                      {{"format", what}});
  if (dw != w || dh != h) {
    stbi_image_free(data);
    return make_error("image.decode_failed", what + " header and data disagree on the image size", {{"format", what}});
  }
  ImageRGBA img(w, h);
  const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
  for (size_t i = 0; i < count; ++i)
    img.at(static_cast<int>(i % w), static_cast<int>(i / w)) =
        Rgba8{data[i * 4 + 0], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]};
  stbi_image_free(data);
  return img;
}

// libwebp (decode only): the header is read first, then the pixels go straight into
// our buffer; an animated file gives its first frame.
Expected<ImageRGBA> decode_webp(std::span<const uint8_t> bytes) {
  if (bytes.size() > limits::kMaxImageFileBytes)
    return make_error("image.too_large", "WebP data is too large", {{"bytes", bytes.size()}});
  WebPBitstreamFeatures f;
  if (WebPGetFeatures(bytes.data(), bytes.size(), &f) != VP8_STATUS_OK)
    return make_error("image.decode_failed", "not a readable WebP image: the header is damaged", {{"format", "WebP"}});
  if (f.width <= 0 || f.height <= 0 || f.width > limits::kMaxImageSide || f.height > limits::kMaxImageSide ||
      static_cast<int64_t>(f.width) * f.height > limits::kMaxImagePixels)
    return make_error("image.too_large",
                      "image is " + std::to_string(f.width) + "x" + std::to_string(f.height) + " px; the limit is " +
                          std::to_string(limits::kMaxImageSide) + " px per side and " +
                          std::to_string(limits::kMaxImagePixels) + " pixels",
                      {{"width", f.width}, {"height", f.height}});
  std::vector<uint8_t> buf(static_cast<size_t>(f.width) * static_cast<size_t>(f.height) * 4);
  const int stride = f.width * 4;
  if (!f.has_animation) {
    if (!WebPDecodeRGBAInto(bytes.data(), bytes.size(), buf.data(), buf.size(), stride))
      return make_error("image.decode_failed", "not a readable WebP image: the pixel data is damaged", {{"format", "WebP"}});
  } else {
    WebPAnimDecoderOptions opt;
    if (!WebPAnimDecoderOptionsInit(&opt)) return make_error("image.decode_failed", "WebP animation decoder unavailable");
    opt.color_mode = MODE_RGBA;
    opt.use_threads = 0;
    WebPData data{bytes.data(), bytes.size()};
    WebPAnimDecoder* dec = WebPAnimDecoderNew(&data, &opt);
    if (!dec) return make_error("image.decode_failed", "not a readable animated WebP image", {{"format", "WebP"}});
    WebPAnimInfo info;
    uint8_t* frame = nullptr;
    int ts = 0;
    const bool ok = WebPAnimDecoderGetInfo(dec, &info) && info.canvas_width == static_cast<uint32_t>(f.width) &&
                    info.canvas_height == static_cast<uint32_t>(f.height) && WebPAnimDecoderGetNext(dec, &frame, &ts) &&
                    frame;
    if (ok) std::memcpy(buf.data(), frame, buf.size());
    WebPAnimDecoderDelete(dec);
    if (!ok) return make_error("image.decode_failed", "not a readable animated WebP image", {{"format", "WebP"}});
  }
  ImageRGBA img(f.width, f.height);
  for (size_t i = 0; i < static_cast<size_t>(f.width) * static_cast<size_t>(f.height); ++i)
    img.at(static_cast<int>(i % static_cast<size_t>(f.width)), static_cast<int>(i / static_cast<size_t>(f.width))) =
        Rgba8{buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]};
  return img;
}

}  // namespace

std::string_view to_string(ImageFormat f) {
  switch (f) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::Bmp: return "BMP";
    case ImageFormat::Gif: return "GIF";
    case ImageFormat::Tga: return "TGA";
    case ImageFormat::Webp: return "WebP";
    case ImageFormat::Aseprite: return "Aseprite";
    case ImageFormat::Psd: return "PSD";
    case ImageFormat::Unknown: break;
  }
  return "unknown";
}

std::string_view file_extension(ImageFormat f) {
  switch (f) {
    case ImageFormat::Png: return "png";
    case ImageFormat::Jpeg: return "jpg";
    case ImageFormat::Bmp: return "bmp";
    case ImageFormat::Gif: return "gif";
    case ImageFormat::Tga: return "tga";
    case ImageFormat::Webp: return "webp";
    case ImageFormat::Aseprite: return "aseprite";
    case ImageFormat::Psd: return "psd";
    case ImageFormat::Unknown: break;
  }
  return "img";
}

ImageFormat sniff_image_format(std::span<const uint8_t> b, std::string_view name_hint) {
  if (starts_with(b, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return ImageFormat::Png;
  if (starts_with(b, {0xFF, 0xD8, 0xFF})) return ImageFormat::Jpeg;
  if (starts_with(b, {'G', 'I', 'F', '8'}) && b.size() >= 6 && (b[4] == '7' || b[4] == '9') && b[5] == 'a')
    return ImageFormat::Gif;
  if (starts_with(b, {'B', 'M'}) && b.size() >= 26) return ImageFormat::Bmp;
  if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WEBP", 4) == 0)
    return ImageFormat::Webp;
  if (b.size() >= 6 && b[4] == 0xE0 && b[5] == 0xA5) return ImageFormat::Aseprite;  // WORD 0xA5E0 at offset 4
  if (starts_with(b, {'8', 'B', 'P', 'S'})) return ImageFormat::Psd;
  // TGA has no signature: only a file that says it is one is read as one.
  if (ends_with_ci(name_hint, ".tga")) return ImageFormat::Tga;
  return ImageFormat::Unknown;
}

Expected<ImageRGBA> decode_png(std::span<const uint8_t> bytes) {
  if (sniff_image_format(bytes) != ImageFormat::Png)
    return make_error("image.decode_failed", "not a PNG image (the PNG signature is missing)", {{"format", "PNG"}});
  return decode_with_stb(bytes, ImageFormat::Png);
}

Expected<ImageRGBA> decode_image(std::span<const uint8_t> bytes, std::string_view name_hint) {
  const ImageFormat f = sniff_image_format(bytes, name_hint);
  switch (f) {
    case ImageFormat::Png:
    case ImageFormat::Jpeg:
    case ImageFormat::Bmp:
    case ImageFormat::Gif:
    case ImageFormat::Tga: return decode_with_stb(bytes, f);
    case ImageFormat::Webp: return decode_webp(bytes);
    case ImageFormat::Aseprite:
    case ImageFormat::Psd: {
      auto l = decode_layered(bytes);
      if (!l) return Unexpected{l.error()};
      return std::move(l->composite);
    }
    case ImageFormat::Unknown: break;
  }
  // The name says which format the file should be, but its content is not that format.
  for (const auto& [ext, named] : {std::pair{".png", ImageFormat::Png}, std::pair{".jpg", ImageFormat::Jpeg},
                                   std::pair{".jpeg", ImageFormat::Jpeg}, std::pair{".bmp", ImageFormat::Bmp},
                                   std::pair{".gif", ImageFormat::Gif}, std::pair{".webp", ImageFormat::Webp},
                                   std::pair{".aseprite", ImageFormat::Aseprite}, std::pair{".ase", ImageFormat::Aseprite},
                                   std::pair{".psd", ImageFormat::Psd}})
    if (ends_with_ci(name_hint, ext))
      return make_error("image.decode_failed",
                        "not a readable " + std::string(to_string(named)) + " image: the file does not start with a " +
                            std::string(to_string(named)) + " signature",
                        {{"format", std::string(to_string(named))}, {"name", std::string(name_hint)}});
  return make_error("image.unsupported_format",
                    "unsupported or unrecognised image format; supported formats are PNG, JPEG, BMP, GIF (first "
                    "frame), TGA (by its .tga name), WebP, Aseprite (.aseprite, .ase) and PSD",
                    {{"name", std::string(name_hint)}});
}

Expected<ImageRGBA> load_png(const std::filesystem::path& path) {
  auto bytes = read_file(path, limits::kMaxImageFileBytes);
  if (!bytes) return Unexpected{bytes.error()};
  auto img = decode_png(*bytes);
  if (!img) {
    Message m = img.error();
    m.where = {{"path", to_utf8(path)}};
    return Unexpected{m};
  }
  return img;
}

std::vector<uint8_t> encode_png(const ImageRGBA& img) {
  std::vector<uint8_t> out;
  auto sink = [](void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* p = static_cast<const uint8_t*>(data);
    v->insert(v->end(), p, p + size);
  };
  stbi_write_png_to_func(sink, &out, img.width(), img.height(), 4, img.pixels().data(), img.width() * 4);
  return out;
}

Message save_png(const ImageRGBA& img, const std::filesystem::path& path) {
  const auto bytes = encode_png(img);
  if (bytes.empty())
    return Message{"io.write_failed", "cannot encode PNG for " + to_utf8(path), Severity::Error, {{"path", to_utf8(path)}}};
  return write_file_atomic(path, bytes);
}

ImageRGBA flip_x(const ImageRGBA& img) {
  ImageRGBA out(img.width(), img.height());
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x) out.at(img.width() - 1 - x, y) = img.at(x, y);
  return out;
}

}  // namespace pixel3d
