#pragma once
// Input normalisation: turns real-world images (opaque or white backgrounds,
// anti-aliasing, JPEG noise, AI "pixel style" art, views upscaled by different
// factors, oversize art) into native-resolution pixel art the aligner can use.
//
// Clean pixel art passes through untouched: when no view needs any change the
// views get no working image and alignment is exactly what it was before.
// Otherwise every view gets a working image derived only from its original and
// the settings (deterministic, never stored), and every change is reported.
// Pixels are never interpolated: backgrounds are removed by flood fill, colours
// are merged into the most frequent nearby source colour, and downscaling takes
// each block's dominant colour.
#include <optional>
#include <vector>

#include "pixel3d/common/diagnostics.hpp"
#include "pixel3d/image/image.hpp"
#include "pixel3d/image/mask.hpp"

namespace pixel3d {

struct SourceView;

// Project settings (saved in the project file).
struct NormalizeSettings {
  bool enabled = true;
  int target_size = 128;        // longest model side for views that needed normalisation (16..512)
  int bg_tolerance = 24;        // largest per-channel distance from the background colour (0..255)
  double axis_tolerance = 0.15; // largest relative disagreement of shared axes between views (0..1)
  bool operator==(const NormalizeSettings&) const = default;
};
std::optional<Message> check_normalize_settings(const NormalizeSettings& s);

struct NormalizeOptions {
  NormalizeSettings settings;
  uint8_t alpha_threshold = 128;
  int extent_tolerance = 1;
  bool single_view = false;
};

struct NormalizeResult {
  bool applied = false;  // at least one view got a working image
  std::vector<Message> messages;
};

// Sets SourceView::working and ::norm for every view when any view needs
// normalisation; clears them otherwise. Errors: input.no_subject,
// input.turnaround_sheet, input.views_inconsistent.
Expected<NormalizeResult> normalize_views(std::vector<SourceView>& views, const NormalizeOptions& opt);

// ---- building blocks (exposed for tests and diagnostics) --------------------

struct BackgroundRemoval {
  ImageRGBA image;
  bool removed = false;
  Rgba8 colour{};              // the (first) background colour
  bool checker = false;        // a two-colour "fake transparency" checkerboard
  int removed_px = 0;
  bool all_background = false; // the fill would remove every pixel (nothing is removed then)
  bool touches_edge = false;   // the subject's bounding box touches the image border
};
// Flood fill from the border through pixels within `tolerance` of the border's
// dominant colour (or of both checkerboard colours). Interior pixels of the same
// colour that are not connected to the border stay opaque. `removed` is false
// when the border has no dominant colour or the fill would not reach past the
// border ring.
BackgroundRemoval remove_background(const ImageRGBA& img, int tolerance, uint8_t alpha_threshold);

// Source pixels per art pixel estimated from the run lengths of uniform colour
// (tolerant to anti-aliasing and noise); 1 when no block grid is evident.
// `determined` (optional) is false when the runs cannot tell the size from its
// multiples: every matching run is the same number of blocks (e.g. a view that
// is one flat colour), so any divisor of that length would fit as well.
double estimate_block_size(const ImageRGBA& img, uint8_t alpha_threshold, bool* determined = nullptr);

// Region of `src` resampled to out_w × out_h: each output pixel takes the
// dominant opaque colour of the central part of its cell (transparent when most
// of that part is transparent). No colour is invented.
ImageRGBA resample_mode(const ImageRGBA& src, Mask2D::Box region, int out_w, int out_h, uint8_t alpha_threshold);

// Per-view facts for diagnostics (view.too_large, inspect): original size,
// opaque bbox, transparent share, detected scale, colour count, likely cause.
nlohmann::json view_diagnostics(const ImageRGBA& img, uint8_t alpha_threshold);

nlohmann::json to_json(const NormalizeSettings& s);
// What normalisation did to a view (SourceView::norm and its working image).
nlohmann::json normalization_json(const SourceView& v);

}  // namespace pixel3d
