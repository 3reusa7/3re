#include "pixel3d/image/layered.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>

#include <miniz.h>

#include "pixel3d/common/limits.hpp"

namespace pixel3d {
namespace {

// Bounds-checked little/big endian reader over the file bytes.
class Reader {
 public:
  Reader(std::span<const uint8_t> b, bool big_endian) : b_(b), be_(big_endian) {}
  size_t pos() const { return p_; }
  size_t size() const { return b_.size(); }
  bool ok() const { return ok_; }
  bool can(size_t n) const { return ok_ && n <= b_.size() - std::min(p_, b_.size()); }
  void seek(size_t at) {
    if (at > b_.size()) ok_ = false;
    else p_ = at;
  }
  void skip(size_t n) {
    if (!can(n)) ok_ = false;
    else p_ += n;
  }
  uint64_t uint(int n) {
    if (!can(static_cast<size_t>(n))) {
      ok_ = false;
      return 0;
    }
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) {
      const uint64_t byte = b_[p_ + static_cast<size_t>(i)];
      v |= be_ ? byte << (8 * (n - 1 - i)) : byte << (8 * i);
    }
    p_ += static_cast<size_t>(n);
    return v;
  }
  uint8_t u8() { return static_cast<uint8_t>(uint(1)); }
  uint16_t u16() { return static_cast<uint16_t>(uint(2)); }
  uint32_t u32() { return static_cast<uint32_t>(uint(4)); }
  int16_t s16() { return static_cast<int16_t>(u16()); }
  int32_t s32() { return static_cast<int32_t>(u32()); }
  std::span<const uint8_t> bytes(size_t n) {
    if (!can(n)) {
      ok_ = false;
      return {};
    }
    auto s = b_.subspan(p_, n);
    p_ += n;
    return s;
  }

 private:
  std::span<const uint8_t> b_;
  bool be_;
  size_t p_ = 0;
  bool ok_ = true;
};

Unexpected corrupt(std::string_view fmt, const std::string& why) {
  return make_error("image.decode_failed", "not a readable " + std::string(fmt) + " file: " + why, {{"format", fmt}});
}

std::optional<Unexpected> check_canvas(std::string_view fmt, int64_t w, int64_t h) {
  if (w <= 0 || h <= 0) return corrupt(fmt, "the canvas has no size");
  if (w > limits::kMaxImageSide || h > limits::kMaxImageSide || w * h > limits::kMaxImagePixels)
    return make_error("image.too_large",
                      std::string(fmt) + " canvas is " + std::to_string(w) + "x" + std::to_string(h) +
                          " px; the limit is " + std::to_string(limits::kMaxImageSide) + " px per side and " +
                          std::to_string(limits::kMaxImagePixels) + " pixels",
                      {{"width", w}, {"height", h}, {"format", fmt}});
  return std::nullopt;
}

// zlib stream → exactly `expected` bytes (more or less is corrupt). The output buffer is
// allocated at the expected size, so a stream that inflates further is refused.
std::optional<std::vector<uint8_t>> inflate_exact(std::span<const uint8_t> in, size_t expected, bool zlib_header) {
  std::vector<uint8_t> out(expected);
  const size_t n = tinfl_decompress_mem_to_mem(out.data(), out.size(), in.data(), in.size(),
                                               zlib_header ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
  if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || n != expected) return std::nullopt;
  return out;
}

// Aseprite's normal blend (rgba_blender_normal) and its 8-bit multiply with rounding.
int mul_un8(int a, int b) {
  const int t = a * b + 0x80;
  return ((t >> 8) + t) >> 8;
}
Rgba8 blend_normal(Rgba8 back, Rgba8 src, int opacity) {
  if (back.a == 0) return Rgba8{src.r, src.g, src.b, static_cast<uint8_t>(mul_un8(src.a, opacity))};
  if (src.a == 0) return back;
  const int sa = mul_un8(src.a, opacity);
  const int ra = sa + back.a - mul_un8(back.a, sa);
  if (ra == 0) return Rgba8{0, 0, 0, 0};
  auto ch = [&](int b, int s) { return static_cast<uint8_t>(b + (s - b) * sa / ra); };
  return Rgba8{ch(back.r, src.r), ch(back.g, src.g), ch(back.b, src.b), static_cast<uint8_t>(ra)};
}

// ---- Aseprite ----------------------------------------------------------------------
// https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md

constexpr std::string_view kAse = "Aseprite";

struct AseLayer {
  uint16_t flags = 0, type = 0, level = 0, blend = 0;
  uint8_t opacity = 255;
  std::string name;
  int parent = -1;
};

struct AseCel {
  int layer = 0, x = 0, y = 0, opacity = 255, z = 0;
  ImageRGBA img;
};

std::string_view ase_blend_name(int b) {
  static const char* names[] = {"normal",     "multiply",   "screen", "overlay",    "darken",     "lighten",
                                "color dodge", "color burn", "hard light", "soft light", "difference", "exclusion",
                                "hue",        "saturation", "color",  "luminosity", "addition",   "subtract",
                                "divide"};
  return b >= 0 && b < 19 ? names[b] : "unknown";
}

}  // namespace

Expected<LayeredImage> decode_aseprite(std::span<const uint8_t> bytes) {
  if (bytes.size() > limits::kMaxImageFileBytes)
    return make_error("image.too_large", "Aseprite file is too large", {{"bytes", bytes.size()}});
  Reader r(bytes, false);
  if (bytes.size() < 128) return corrupt(kAse, "the header is truncated");
  const uint32_t declared = r.u32();
  if (r.u16() != 0xA5E0) return corrupt(kAse, "the Aseprite signature is missing");
  // Aseprite writes its size first: a shorter file lost frames, layers or cels.
  if (declared > bytes.size()) return corrupt(kAse, "the file is truncated");
  const int frames = r.u16(), w = r.u16(), h = r.u16(), depth = r.u16();
  const uint32_t hflags = r.u32();
  r.skip(2 + 4 + 4);
  const uint8_t transparent_index = r.u8();
  r.skip(3);
  r.u16();  // number of colours
  r.seek(128);
  if (frames < 1) return corrupt(kAse, "the file has no frames");
  if (auto e = check_canvas(kAse, w, h)) return *e;
  if (depth != 32 && depth != 16 && depth != 8) return corrupt(kAse, "unknown colour depth " + std::to_string(depth));
  const int bpp = depth / 8;

  LayeredImage out;
  out.format = ImageFormat::Aseprite;
  std::vector<AseLayer> layers;
  std::vector<AseCel> cels;
  std::vector<Rgba8> palette(256, Rgba8{0, 0, 0, 255});
  int64_t cel_pixels = 0;

  // Only the first frame is read.
  const size_t frame_start = r.pos();
  const uint32_t frame_bytes = r.u32();
  if (r.u16() != 0xF1FA) return corrupt(kAse, "the first frame header is damaged");
  const uint32_t old_chunks = r.u16();
  r.skip(2 + 2);
  const uint32_t new_chunks = r.u32();
  if (!r.ok() || frame_bytes < 16 || frame_bytes > bytes.size() - frame_start)
    return corrupt(kAse, "the first frame is truncated");
  const size_t frame_end = frame_start + frame_bytes;
  const uint32_t chunks = new_chunks ? new_chunks : old_chunks;
  if (chunks > 1u << 20) return corrupt(kAse, "too many chunks");
  for (uint32_t c = 0; c < chunks; ++c) {
    const size_t at = r.pos();
    const uint32_t size = r.u32();
    const uint16_t type = r.u16();
    if (!r.ok() || size < 6 || size > frame_end - at) return corrupt(kAse, "a chunk runs past the end of the frame");
    const size_t end = at + size;
    Reader ch(bytes.subspan(0, end), false);
    ch.seek(r.pos());
    switch (type) {
      case 0x2004: {  // layer
        if (layers.size() >= static_cast<size_t>(limits::kMaxLayers))
          return make_error("image.too_large", "the Aseprite file has more than " + std::to_string(limits::kMaxLayers) + " layers");
        AseLayer L;
        L.flags = ch.u16();
        L.type = ch.u16();
        L.level = ch.u16();
        ch.skip(4);
        L.blend = ch.u16();
        L.opacity = ch.u8();
        if (!(hflags & 1)) L.opacity = 255;  // the opacity field is valid only with header flag 1
        ch.skip(3);
        const uint16_t len = ch.u16();
        const auto name = ch.bytes(len);
        if (!ch.ok()) return corrupt(kAse, "a layer chunk is truncated");
        L.name.assign(name.begin(), name.end());
        for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i)
          if (layers[static_cast<size_t>(i)].level + 1 == L.level) {
            L.parent = i;
            break;
          }
        layers.push_back(std::move(L));
        break;
      }
      case 0x2005: {  // cel
        AseCel cel;
        cel.layer = ch.u16();
        cel.x = ch.s16();
        cel.y = ch.s16();
        cel.opacity = ch.u8();
        const uint16_t cel_type = ch.u16();
        cel.z = ch.s16();
        ch.skip(5);
        if (!ch.ok()) return corrupt(kAse, "a cel chunk is truncated");
        if (cel_type == 1) {
          out.notes.push_back(Message{"layered.linked_cel", "a linked cel in the first frame was skipped", Severity::Warning,
                                      {{"layer", cel.layer}}});
          break;
        }
        if (cel_type == 3) {
          const std::string lname = cel.layer < static_cast<int>(layers.size()) ? layers[static_cast<size_t>(cel.layer)].name : "";
          out.notes.push_back(Message{"layered.tilemap_skipped",
                                      "tilemap layer '" + lname + "' is not read: it is missing from the image and from the suggestions",
                                      Severity::Warning, {{"layer", lname}}});
          break;
        }
        if (cel_type != 0 && cel_type != 2) return corrupt(kAse, "unknown cel type " + std::to_string(cel_type));
        const int cw = ch.u16(), chh = ch.u16();
        if (!ch.ok()) return corrupt(kAse, "a cel chunk is truncated");
        if (cw == 0 || chh == 0) break;
        if (cw > limits::kMaxImageSide || chh > limits::kMaxImageSide ||
            static_cast<int64_t>(cw) * chh > limits::kMaxImagePixels)
          return make_error("image.too_large", "an Aseprite cel is " + std::to_string(cw) + "x" + std::to_string(chh) + " px");
        cel_pixels += static_cast<int64_t>(cw) * chh;
        if (cel_pixels > limits::kMaxLayerPixelsTotal)
          return make_error("image.too_large", "the Aseprite layers hold too many pixels");
        const size_t raw_size = static_cast<size_t>(cw) * chh * static_cast<size_t>(bpp);
        std::vector<uint8_t> raw;
        if (cel_type == 0) {
          const auto data = ch.bytes(raw_size);
          if (!ch.ok()) return corrupt(kAse, "a cel's pixels are truncated");
          raw.assign(data.begin(), data.end());
        } else {
          auto inflated = inflate_exact(bytes.subspan(ch.pos(), end - ch.pos()), raw_size, true);
          if (!inflated) return corrupt(kAse, "a compressed cel does not hold exactly its pixels");
          raw = std::move(*inflated);
        }
        cel.img = ImageRGBA(cw, chh);
        const bool background = cel.layer < static_cast<int>(layers.size()) &&
                                (layers[static_cast<size_t>(cel.layer)].flags & 8);
        for (int i = 0; i < cw * chh; ++i) {
          const uint8_t* p = raw.data() + static_cast<size_t>(i) * static_cast<size_t>(bpp);
          Rgba8 px;
          if (bpp == 4) px = Rgba8{p[0], p[1], p[2], p[3]};
          else if (bpp == 2) px = Rgba8{p[0], p[0], p[0], p[1]};
          else px = (!background && p[0] == transparent_index) ? Rgba8{0, 0, 0, 0} : palette[p[0]];
          cel.img.at(i % cw, i / cw) = px;
        }
        cels.push_back(std::move(cel));
        break;
      }
      case 0x2019: {  // palette
        const uint32_t n = ch.u32(), first = ch.u32(), last = ch.u32();
        ch.skip(8);
        if (!ch.ok() || first > last || last >= n || n > 65536) return corrupt(kAse, "a palette chunk is damaged");
        for (uint32_t i = first; i <= last; ++i) {
          const uint16_t f = ch.u16();
          const uint8_t cr = ch.u8(), cg = ch.u8(), cb = ch.u8(), ca = ch.u8();
          if (f & 1) ch.skip(ch.u16());
          if (!ch.ok()) return corrupt(kAse, "a palette chunk is truncated");
          if (i < 256) palette[i] = Rgba8{cr, cg, cb, ca};
        }
        break;
      }
      case 0x0004:
      case 0x0011: {  // old palettes (0x0011: 6-bit values); a new palette chunk overrides them
        const uint16_t packets = ch.u16();
        int index = 0;
        for (int pk = 0; pk < packets && ch.ok(); ++pk) {
          index += ch.u8();
          int count = ch.u8();
          if (count == 0) count = 256;
          for (int i = 0; i < count && ch.ok(); ++i, ++index) {
            int cr = ch.u8(), cg = ch.u8(), cb = ch.u8();
            if (type == 0x0011) cr = cr * 255 / 63, cg = cg * 255 / 63, cb = cb * 255 / 63;
            if (index < 256)
              palette[static_cast<size_t>(index)] = Rgba8{static_cast<uint8_t>(cr), static_cast<uint8_t>(cg),
                                                          static_cast<uint8_t>(cb), 255};
          }
        }
        if (!ch.ok()) return corrupt(kAse, "an old palette chunk is truncated");
        break;
      }
      default: break;  // tags, slices, user data, colour profile, ... do not change the pixels
    }
    r.seek(end);
  }
  if (!r.ok()) return corrupt(kAse, "the first frame is damaged");
  if (frames > 1)
    out.notes.push_back(Message{"layered.first_frame", "the first of " + std::to_string(frames) + " frames is used",
                                Severity::Info, {{"frames", frames}}});

  // Effective visibility and opacity, the path through the groups.
  auto visible = [&](int i) {
    for (; i >= 0; i = layers[static_cast<size_t>(i)].parent)
      if (!(layers[static_cast<size_t>(i)].flags & 1)) return false;
    return true;
  };
  auto path_of = [&](int i) {
    std::string p = layers[static_cast<size_t>(i)].name;
    for (int q = layers[static_cast<size_t>(i)].parent; q >= 0; q = layers[static_cast<size_t>(q)].parent)
      p = layers[static_cast<size_t>(q)].name + "/" + p;
    return p;
  };
  for (size_t i = 0; i < layers.size(); ++i) {
    const AseLayer& L = layers[i];
    if (L.type == 1 && (L.opacity != 255 || L.blend != 0) && visible(static_cast<int>(i)))
      out.notes.push_back(Message{"layered.group_blend", "group '" + L.name + "' has its own opacity or blend mode: its "
                                                         "children are composed directly (approximation)",
                                  Severity::Warning, {{"layer", L.name}}});
    if (L.type == 0 && L.blend != 0 && visible(static_cast<int>(i)))
      out.notes.push_back(Message{"layered.blend_mode", "layer '" + L.name + "' uses the " + std::string(ase_blend_name(L.blend)) +
                                                        " blend mode: composed as normal",
                                  Severity::Warning, {{"layer", L.name}, {"blend", ase_blend_name(L.blend)}}});
  }

  // Draw order: layer order, moved by the cel's z-index (Aseprite: order = layer + z, ties by z).
  std::vector<const AseCel*> order;
  for (const AseCel& c : cels)
    if (c.layer >= 0 && c.layer < static_cast<int>(layers.size())) order.push_back(&c);
    else return corrupt(kAse, "a cel refers to a missing layer");
  std::stable_sort(order.begin(), order.end(), [](const AseCel* a, const AseCel* b) {
    const int oa = a->layer + a->z, ob = b->layer + b->z;
    return oa != ob ? oa < ob : a->z < b->z;
  });

  out.composite = ImageRGBA(w, h);
  for (const AseCel* c : order) {
    const AseLayer& L = layers[static_cast<size_t>(c->layer)];
    if (L.type != 0) continue;
    const bool reference = L.flags & 64;
    const bool vis = visible(c->layer) && !reference;
    const int opacity = mul_un8(c->opacity, L.opacity);
    SourceLayer sl;
    sl.name = L.name;
    sl.path = path_of(c->layer);
    sl.visible = vis;
    Mask2D::Box clip{std::max(0, c->x), std::max(0, c->y), std::min(w, c->x + c->img.width()),
                     std::min(h, c->y + c->img.height())};
    if (!clip.empty()) {
      sl.rect = clip;
      sl.pixels = ImageRGBA(clip.width(), clip.height());
      for (int y = clip.y0; y < clip.y1; ++y)
        for (int x = clip.x0; x < clip.x1; ++x) {
          const Rgba8 s = c->img.at(x - c->x, y - c->y);
          sl.pixels.at(x - clip.x0, y - clip.y0) = Rgba8{s.r, s.g, s.b, static_cast<uint8_t>(mul_un8(s.a, opacity))};
          if (vis) out.composite.at(x, y) = blend_normal(out.composite.at(x, y), s, opacity);
        }
    }
    if (reference) continue;  // reference layers are never part of the drawing
    out.layers.push_back(std::move(sl));
  }
  return out;
}

namespace {

// ---- Photoshop -----------------------------------------------------------------------
// Adobe Photoshop File Formats Specification (PSD, version 1).

constexpr std::string_view kPsd = "PSD";

// PackBits rows into `out` (exactly out.size() bytes).
bool unpack_bits(std::span<const uint8_t> in, std::span<uint8_t> out) {
  size_t i = 0, o = 0;
  while (o < out.size()) {
    if (i >= in.size()) return false;
    const int n = static_cast<int8_t>(in[i++]);
    if (n >= 0) {
      const size_t len = static_cast<size_t>(n) + 1;
      if (i + len > in.size() || o + len > out.size()) return false;
      std::memcpy(out.data() + o, in.data() + i, len);
      i += len, o += len;
    } else if (n != -128) {
      const size_t len = static_cast<size_t>(1 - n);
      if (i >= in.size() || o + len > out.size()) return false;
      std::memset(out.data() + o, in[i++], len);
      o += len;
    }
  }
  return true;
}

// One channel of `w` × `h` samples at `depth` bits, compressed as `comp`, from `data`
// (exactly the channel's bytes after its compression word, or for RLE starting at its
// row counts). Returns 8-bit samples.
std::optional<std::vector<uint8_t>> read_channel(std::span<const uint8_t> data, int comp, int w, int h, int depth) {
  const size_t bps = static_cast<size_t>(depth / 8);
  const size_t row = static_cast<size_t>(w) * bps, total = row * static_cast<size_t>(h);
  std::vector<uint8_t> raw;
  switch (comp) {
    case 0:
      if (data.size() < total) return std::nullopt;
      raw.assign(data.begin(), data.begin() + static_cast<ptrdiff_t>(total));
      break;
    case 1: {
      if (data.size() < static_cast<size_t>(h) * 2) return std::nullopt;
      raw.resize(total);
      size_t at = static_cast<size_t>(h) * 2;
      for (int y = 0; y < h; ++y) {
        const size_t n = static_cast<size_t>(data[static_cast<size_t>(y) * 2]) << 8 | data[static_cast<size_t>(y) * 2 + 1];
        if (at + n > data.size()) return std::nullopt;
        if (!unpack_bits(data.subspan(at, n), std::span<uint8_t>(raw.data() + static_cast<size_t>(y) * row, row)))
          return std::nullopt;
        at += n;
      }
      break;
    }
    case 2:
    case 3: {
      auto z = inflate_exact(data, total, true);
      if (!z) return std::nullopt;
      raw = std::move(*z);
      if (comp == 3) {  // horizontal delta per row (8 and 16 bit)
        for (int y = 0; y < h; ++y) {
          uint8_t* p = raw.data() + static_cast<size_t>(y) * row;
          if (bps == 1) {
            for (int x = 1; x < w; ++x) p[x] = static_cast<uint8_t>(p[x] + p[x - 1]);
          } else {
            for (int x = 1; x < w; ++x) {
              const uint16_t prev = static_cast<uint16_t>(p[2 * (x - 1)] << 8 | p[2 * (x - 1) + 1]);
              const uint16_t cur = static_cast<uint16_t>((p[2 * x] << 8 | p[2 * x + 1]) + prev);
              p[2 * x] = static_cast<uint8_t>(cur >> 8), p[2 * x + 1] = static_cast<uint8_t>(cur);
            }
          }
        }
      }
      break;
    }
    default: return std::nullopt;
  }
  if (bps == 1) return raw;
  std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (size_t i = 0; i < out.size(); ++i) out[i] = raw[i * 2];  // 16 bit: the high byte
  return out;
}

const std::map<std::string, std::string>& psd_non_raster_keys() {
  static const std::map<std::string, std::string> k = {
      {"TySh", "text"}, {"tySh", "text"}, {"SoLd", "smart object"}, {"SoLE", "smart object"}, {"PlLd", "smart object"},
      {"SoCo", "fill"}, {"GdFl", "fill"}, {"PtFl", "fill"}, {"levl", "adjustment"}, {"curv", "adjustment"},
      {"brit", "adjustment"}, {"blnc", "adjustment"}, {"hue ", "adjustment"}, {"hue2", "adjustment"},
      {"selc", "adjustment"}, {"mixr", "adjustment"}, {"grdm", "adjustment"}, {"phfl", "adjustment"},
      {"expA", "adjustment"}, {"vibA", "adjustment"}, {"thrs", "adjustment"}, {"post", "adjustment"},
      {"nvrt", "adjustment"}, {"blwh", "adjustment"}, {"clrL", "adjustment"}, {"CgEd", "adjustment"}};
  return k;
}

struct PsdChannel {
  int id = 0;
  uint32_t length = 0;
};

struct PsdLayer {
  int top = 0, left = 0, bottom = 0, right = 0;
  std::vector<PsdChannel> channels;
  std::string blend;
  uint8_t opacity = 255, clipping = 0, flags = 0;
  std::string name;
  int section = 0;          // lsct: 1/2 group (open/closed) record, 3 group end marker
  std::string non_raster;   // text, smart object, fill, adjustment
};

}  // namespace

Expected<LayeredImage> decode_psd(std::span<const uint8_t> bytes) {
  if (bytes.size() > limits::kMaxImageFileBytes)
    return make_error("image.too_large", "PSD file is too large", {{"bytes", bytes.size()}});
  Reader r(bytes, true);
  if (bytes.size() < 26 || std::memcmp(bytes.data(), "8BPS", 4) != 0) return corrupt(kPsd, "the PSD signature is missing");
  r.skip(4);
  const uint16_t version = r.u16();
  if (version == 2)
    return make_error("image.unsupported_format", "large-document PSB files are not supported; save as PSD",
                      {{"format", "PSB"}});
  if (version != 1) return corrupt(kPsd, "unknown version " + std::to_string(version));
  r.skip(6);
  const int nch = r.u16();
  const int64_t h = r.u32(), w = r.u32();
  const int depth = r.u16(), mode = r.u16();
  if (auto e = check_canvas(kPsd, w, h)) return *e;
  if (nch < 1 || nch > 56) return corrupt(kPsd, "invalid channel count");
  if (depth != 8 && depth != 16)
    return make_error("image.unsupported_format",
                      std::to_string(depth) + "-bit PSD files are not supported; save with 8 or 16 bits per channel",
                      {{"format", "PSD"}, {"depth", depth}});
  if (mode != 1 && mode != 2 && mode != 3)
    return make_error("image.unsupported_format",
                      "this PSD colour mode is not supported (only RGB, grayscale and indexed); convert the image to RGB",
                      {{"format", "PSD"}, {"mode", mode}});
  // Colour mode data (the palette of indexed images).
  const uint32_t cm_len = r.u32();
  const auto cm = r.bytes(cm_len);
  if (!r.ok()) return corrupt(kPsd, "the colour mode data is truncated");
  if (mode == 2 && cm.size() < 768) return corrupt(kPsd, "the indexed palette is missing");
  // Image resources: skipped.
  const uint32_t res_len = r.u32();
  r.skip(res_len);
  // Layer and mask information.
  const uint32_t lm_len = r.u32();
  const size_t lm_start = r.pos();
  if (!r.ok() || lm_len > bytes.size() - lm_start) return corrupt(kPsd, "the layer section is truncated");
  const size_t lm_end = lm_start + lm_len;

  LayeredImage out;
  out.format = ImageFormat::Psd;
  int layer_count = 0;
  std::vector<PsdLayer> layers;
  if (lm_len >= 4) {
    const uint32_t li_len = r.u32();
    const size_t li_start = r.pos();
    if (li_len > lm_end - li_start) return corrupt(kPsd, "the layer info is truncated");
    if (li_len >= 2) {
      layer_count = r.s16();
      const int n = std::abs(layer_count);
      if (n > limits::kMaxLayers)
        return make_error("image.too_large", "the PSD file has more than " + std::to_string(limits::kMaxLayers) + " layers");
      for (int i = 0; i < n; ++i) {
        PsdLayer L;
        L.top = r.s32(), L.left = r.s32(), L.bottom = r.s32(), L.right = r.s32();
        const int ch = r.u16();
        if (!r.ok() || ch > 56) return corrupt(kPsd, "a layer record is damaged");
        for (int c = 0; c < ch; ++c) {
          PsdChannel pc;
          pc.id = r.s16();
          pc.length = r.u32();
          L.channels.push_back(pc);
        }
        const auto sig = r.bytes(4);
        const auto key = r.bytes(4);
        if (!r.ok() || std::memcmp(sig.data(), "8BIM", 4) != 0) return corrupt(kPsd, "a layer blend signature is missing");
        L.blend.assign(key.begin(), key.end());
        L.opacity = r.u8(), L.clipping = r.u8(), L.flags = r.u8();
        r.skip(1);
        const uint32_t extra = r.u32();
        const size_t extra_start = r.pos();
        if (!r.ok() || extra > li_start + li_len - extra_start) return corrupt(kPsd, "a layer record is truncated");
        const size_t extra_end = extra_start + extra;
        r.skip(r.u32());  // layer mask data
        r.skip(r.u32());  // blending ranges
        const uint8_t nlen = r.u8();
        const auto nm = r.bytes(nlen);
        L.name.assign(nm.begin(), nm.end());
        r.skip((4 - (1 + nlen) % 4) % 4);  // Pascal string padded to 4
        // Additional layer information.
        while (r.ok() && r.pos() + 12 <= extra_end) {
          const auto s = r.bytes(4);
          const auto k = r.bytes(4);
          const uint32_t len = r.u32();
          if (!r.ok() || (std::memcmp(s.data(), "8BIM", 4) != 0 && std::memcmp(s.data(), "8B64", 4) != 0) ||
              len > extra_end - r.pos())
            break;
          const std::string kk(k.begin(), k.end());
          const size_t block = r.pos();
          if (kk == "luni" && len >= 4) {  // Unicode name (UTF-16 BE) → UTF-8
            const uint32_t chars = r.u32();
            if (chars <= (len - 4) / 2) {
              std::string u8;
              for (uint32_t q = 0; q < chars; ++q) {
                uint32_t cp = r.u16();
                if (cp >= 0xD800 && cp < 0xDC00 && q + 1 < chars) {
                  const uint32_t lo = r.u16();
                  ++q;
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if (cp == 0) break;
                if (cp < 0x80) u8 += static_cast<char>(cp);
                else if (cp < 0x800) u8 += static_cast<char>(0xC0 | cp >> 6), u8 += static_cast<char>(0x80 | (cp & 63));
                else if (cp < 0x10000)
                  u8 += static_cast<char>(0xE0 | cp >> 12), u8 += static_cast<char>(0x80 | (cp >> 6 & 63)),
                      u8 += static_cast<char>(0x80 | (cp & 63));
                else
                  u8 += static_cast<char>(0xF0 | cp >> 18), u8 += static_cast<char>(0x80 | (cp >> 12 & 63)),
                      u8 += static_cast<char>(0x80 | (cp >> 6 & 63)), u8 += static_cast<char>(0x80 | (cp & 63));
              }
              if (!u8.empty()) L.name = u8;
            }
          } else if ((kk == "lsct" || kk == "lsdk") && len >= 4) {
            L.section = static_cast<int>(r.u32());
          } else if (auto it = psd_non_raster_keys().find(kk); it != psd_non_raster_keys().end()) {
            L.non_raster = it->second;
          }
          r.seek(block + len);
        }
        r.seek(extra_end);
        if (!r.ok()) return corrupt(kPsd, "a layer record is damaged");
        layers.push_back(std::move(L));
      }
      // Channel image data, in record order.
      std::vector<SourceLayer> raster;  // bottom to top, as stored
      int64_t layer_pixels = 0;
      // Groups: records are bottom to top; a group's end marker (3) comes first, its record (1/2) last.
      // Resolve group names and visibility top-down: walk from the top record.
      std::vector<std::vector<std::pair<std::string, bool>>> stack_at(layers.size());
      {
        std::vector<std::pair<std::string, bool>> st;
        for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i) {
          const PsdLayer& L = layers[static_cast<size_t>(i)];
          if (L.section == 1 || L.section == 2) {
            st.push_back({L.name, !(L.flags & 2)});
            continue;
          }
          if (L.section == 3) {
            if (!st.empty()) st.pop_back();
            continue;
          }
          stack_at[static_cast<size_t>(i)] = st;
        }
      }
      for (size_t i = 0; i < layers.size(); ++i) {
        const PsdLayer& L = layers[i];
        const int64_t lw = static_cast<int64_t>(L.right) - L.left, lh = static_cast<int64_t>(L.bottom) - L.top;
        if (lw < 0 || lh < 0 || lw > limits::kMaxImageSide || lh > limits::kMaxImageSide)
          return corrupt(kPsd, "a layer has an invalid size");
        layer_pixels += lw * lh;
        if (layer_pixels > limits::kMaxLayerPixelsTotal) return make_error("image.too_large", "the PSD layers hold too many pixels");
        std::map<int, std::vector<uint8_t>> planes;
        for (const PsdChannel& c : L.channels) {
          const size_t at = r.pos();
          if (c.length > bytes.size() - at) return corrupt(kPsd, "a layer channel is truncated");
          if (c.length >= 2 && lw > 0 && lh > 0 && (c.id >= -1 && c.id <= 2)) {
            const int comp = (bytes[at] << 8) | bytes[at + 1];
            auto plane = read_channel(bytes.subspan(at + 2, c.length - 2), comp, static_cast<int>(lw), static_cast<int>(lh), depth);
            if (!plane) return corrupt(kPsd, "layer '" + L.name + "' has damaged pixel data");
            planes[c.id] = std::move(*plane);
          }
          r.seek(at + c.length);
        }
        if (L.section != 0) continue;
        std::string path;
        bool vis = !(L.flags & 2);
        for (const auto& [g, gv] : stack_at[i]) {
          path += g + "/";
          vis = vis && gv;
        }
        path += L.name;
        if (!L.non_raster.empty()) {
          out.notes.push_back(Message{"layered.not_raster",
                                      L.non_raster + " layer '" + L.name + "' is in the composite image but is not a raster "
                                                     "layer: no part mask is suggested for it",
                                      Severity::Info, {{"layer", path}, {"kind", L.non_raster}}});
          continue;
        }
        if (L.clipping)
          out.notes.push_back(Message{"layered.clipping", "layer '" + L.name + "' is clipped to the layer below; its "
                                                          "suggested mask is its own pixels",
                                      Severity::Info, {{"layer", path}}});
        SourceLayer sl;
        sl.name = L.name;
        sl.path = path;
        sl.visible = vis;
        const Mask2D::Box clip{std::max(0, L.left), std::max(0, L.top), std::min<int>(static_cast<int>(w), L.right),
                               std::min<int>(static_cast<int>(h), L.bottom)};
        if (!clip.empty() && lw > 0 && lh > 0) {
          sl.rect = clip;
          sl.pixels = ImageRGBA(clip.width(), clip.height());
          for (int y = clip.y0; y < clip.y1; ++y)
            for (int x = clip.x0; x < clip.x1; ++x) {
              const size_t k = static_cast<size_t>(y - L.top) * static_cast<size_t>(lw) + static_cast<size_t>(x - L.left);
              auto sample = [&](int id, uint8_t dflt) { auto it = planes.find(id); return it == planes.end() ? dflt : it->second[k]; };
              uint8_t cr, cg, cb;
              if (mode == 3) cr = sample(0, 0), cg = sample(1, 0), cb = sample(2, 0);
              else if (mode == 1) cr = cg = cb = sample(0, 0);
              else {
                const uint8_t idx = sample(0, 0);
                cr = cm[idx], cg = cm[256 + idx], cb = cm[512 + idx];
              }
              const uint8_t a = static_cast<uint8_t>(mul_un8(sample(-1, 255), L.opacity));
              sl.pixels.at(x - clip.x0, y - clip.y0) = Rgba8{cr, cg, cb, a};
            }
        }
        if (L.blend != "norm" && L.blend != "pass" && vis)
          out.notes.push_back(Message{"layered.blend_mode", "layer '" + L.name + "' uses blend mode '" + L.blend +
                                                            "': its suggested mask is its own pixels",
                                      Severity::Info, {{"layer", path}, {"blend", L.blend}}});
        raster.push_back(std::move(sl));
      }
      out.layers = std::move(raster);
    }
  }
  r.seek(lm_end);
  // The composite image stored in the file.
  const int comp = r.u16();
  if (!r.ok()) return corrupt(kPsd, "the composite image is missing");
  const size_t plane = static_cast<size_t>(w) * static_cast<size_t>(h);
  const size_t row = static_cast<size_t>(w) * static_cast<size_t>(depth / 8);
  const int colour_channels = mode == 3 ? 3 : 1;
  const bool alpha = layer_count < 0 && nch > colour_channels;  // transparency of the merged result
  if (layer_count >= 0 && nch > colour_channels)
    out.notes.push_back(Message{"layered.extra_channels", "extra channels of the composite (saved selections) are ignored",
                                Severity::Info, {{"channels", nch}}});
  const int read = colour_channels + (alpha ? 1 : 0);
  std::vector<std::vector<uint8_t>> planes;
  const auto rest = bytes.subspan(r.pos());
  if (comp == 1) {
    // Row counts of every row of every channel first, then the rows.
    const size_t counts = static_cast<size_t>(nch) * static_cast<size_t>(h) * 2;
    if (rest.size() < counts) return corrupt(kPsd, "the composite image is truncated");
    size_t at = counts;
    for (int c = 0; c < read; ++c) {
      std::vector<uint8_t> raw(row * static_cast<size_t>(h));
      for (int y = 0; y < h; ++y) {
        const size_t q = (static_cast<size_t>(c) * static_cast<size_t>(h) + static_cast<size_t>(y)) * 2;
        const size_t n = static_cast<size_t>(rest[q]) << 8 | rest[q + 1];
        if (at + n > rest.size() ||
            !unpack_bits(rest.subspan(at, n), std::span<uint8_t>(raw.data() + static_cast<size_t>(y) * row, row)))
          return corrupt(kPsd, "the composite image is damaged");
        at += n;
      }
      if (depth == 16) {
        std::vector<uint8_t> hi(plane);
        for (size_t i = 0; i < plane; ++i) hi[i] = raw[i * 2];
        raw = std::move(hi);
      }
      planes.push_back(std::move(raw));
    }
  } else if (comp == 0) {
    const size_t per = row * static_cast<size_t>(h);
    if (rest.size() < per * static_cast<size_t>(read)) return corrupt(kPsd, "the composite image is truncated");
    for (int c = 0; c < read; ++c) {
      auto p = read_channel(rest.subspan(per * static_cast<size_t>(c), per), 0, static_cast<int>(w), static_cast<int>(h), depth);
      if (!p) return corrupt(kPsd, "the composite image is damaged");
      planes.push_back(std::move(*p));
    }
  } else {
    // ZIP composites are not written by Photoshop for 8/16-bit images; refuse rather than guess.
    return make_error("image.unsupported_format", "this PSD composite compression is not supported",
                      {{"format", "PSD"}, {"compression", comp}});
  }
  out.composite = ImageRGBA(static_cast<int>(w), static_cast<int>(h));
  for (size_t i = 0; i < plane; ++i) {
    uint8_t cr, cg, cb;
    if (mode == 3) cr = planes[0][i], cg = planes[1][i], cb = planes[2][i];
    else if (mode == 1) cr = cg = cb = planes[0][i];
    else cr = cm[planes[0][i]], cg = cm[256 + planes[0][i]], cb = cm[512 + planes[0][i]];
    out.composite.at(static_cast<int>(i % static_cast<size_t>(w)), static_cast<int>(i / static_cast<size_t>(w))) =
        Rgba8{cr, cg, cb, alpha ? planes[static_cast<size_t>(colour_channels)][i] : uint8_t{255}};
  }
  return out;
}

Expected<LayeredImage> decode_layered(std::span<const uint8_t> bytes) {
  switch (sniff_image_format(bytes)) {
    case ImageFormat::Aseprite: return decode_aseprite(bytes);
    case ImageFormat::Psd: return decode_psd(bytes);
    default: return make_error("image.not_layered", "not an Aseprite or PSD file");
  }
}

}  // namespace pixel3d
