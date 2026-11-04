#include "pixel3d/image/normalize.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>

#include "pixel3d/common/limits.hpp"
#include "pixel3d/image/pixel_scale.hpp"
#include "pixel3d/views/source_view.hpp"

namespace pixel3d {
namespace {

// Fixed parameters (the user-facing ones are in NormalizeSettings).
constexpr int kMergeTolerance = 12;           // colours this close (per channel) are one colour after clean-up
constexpr double kBorderDominance = 0.6;      // share of the border a background colour must cover
constexpr double kCheckerShare = 0.2;         // each checkerboard colour covers at least this share of the border
constexpr double kNoTransparency = 0.01;      // below this transparent share a view has "no transparency"
constexpr double kPartialAlphaNoise = 0.01;   // more partial-alpha pixels than this share: not clean pixel art
constexpr int kCleanMaxColours = 256;         // more colours than this: not clean pixel art
constexpr double kMinSubjectShare = 0.02;     // a clean opaque view keeps at least this much after removal
constexpr double kLeakShare = 0.6;            // removing more than this warns about a possible fill leak
constexpr double kFigureShare = 0.4;          // components this large (vs the largest) count as figures
constexpr int kMaxExactBlock = 128;           // largest exact block grid looked for while normalising
constexpr double kInner = 0.2;                // resampling ignores this share of each cell's edges
constexpr int kRunTolerance = 24;             // colour tolerance when tracing runs in irregular (AI) pixel art

uint32_t rgba_key(Rgba8 c) { return uint32_t(c.r) << 24 | uint32_t(c.g) << 16 | uint32_t(c.b) << 8 | c.a; }
Rgba8 from_key(uint32_t k) {
  return Rgba8{uint8_t(k >> 24), uint8_t(k >> 16), uint8_t(k >> 8), uint8_t(k)};
}
int channel_distance(Rgba8 a, Rgba8 b) {
  return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

std::string hex(Rgba8 c) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
  return buf;
}

std::string role_name(ViewRole r) { return std::string(to_string(r)); }
std::string Role(ViewRole r) {
  std::string s = role_name(r);
  if (!s.empty()) s[0] = static_cast<char>(s[0] - 'a' + 'A');
  return s;
}

double round3(double v) { return std::round(v * 1000) / 1000; }

// ---- facts about one image ------------------------------------------------------

struct Facts {
  int w = 0, h = 0;
  Mask2D::Box bbox;
  int opaque = 0, transparent = 0, partial = 0, visible = 0;
  int colours = 0;  // distinct RGB of opaque pixels
  double transparent_share() const { return w > 0 && h > 0 ? double(transparent) / (double(w) * h) : 0; }
  double partial_share() const { return visible ? double(partial) / visible : 0; }
  bool clean() const { return partial_share() <= kPartialAlphaNoise && colours <= kCleanMaxColours; }
};

Facts facts_of(const ImageRGBA& img, uint8_t thr) {
  Facts f;
  f.w = img.width(), f.h = img.height();
  std::vector<uint32_t> cols;
  cols.reserve(static_cast<size_t>(f.w) * f.h / 4);
  for (const Rgba8& p : img.pixels()) {
    if (p.a < thr) ++f.transparent;
    else {
      ++f.opaque;
      cols.push_back(uint32_t(p.r) << 16 | uint32_t(p.g) << 8 | p.b);
    }
    if (p.a > 0) ++f.visible;
    if (p.a > 0 && p.a < 255) ++f.partial;
  }
  std::sort(cols.begin(), cols.end());
  f.colours = static_cast<int>(std::unique(cols.begin(), cols.end()) - cols.begin());
  f.bbox = opaque_mask(img, thr).bbox();
  return f;
}

// Exact k× block grid (colour changes and the opaque bbox), as the aligner finds it.
int exact_block(const ImageRGBA& img, uint8_t thr, int max_k) {
  int g = change_spacing_gcd(img);
  const Mask2D::Box b = opaque_mask(img, thr).bbox();
  if (!b.empty()) g = std::gcd(g, std::gcd(b.width(), b.height()));
  return scale_from_gcd(g, max_k);
}

bool neutral_or_key(Rgba8 c) {
  const int mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
  if (mx - mn <= 16) return true;  // white, black, greys
  const bool magenta = c.r >= 230 && c.g <= 30 && c.b >= 230, green = c.g >= 200 && c.r <= 60 && c.b <= 60;
  return magenta || green;
}

// Whether a removal found by remove_background is accepted for this image:
// clean pixel art keeps its own border colour unless it clearly is a background.
bool background_accepted(const Facts& f, const BackgroundRemoval& b) {
  if (!b.removed) return false;
  if (!f.clean()) return true;
  const double subject = double(f.opaque - b.removed_px) / std::max(1, f.w * f.h);
  return (!b.touches_edge || neutral_or_key(b.colour)) && subject >= kMinSubjectShare;
}

// ---- clean-up --------------------------------------------------------------------

// Partial alpha is snapped to the opacity the engine sees; transparent pixels lose their RGB.
int snap_alpha(ImageRGBA& img, uint8_t thr) {
  int n = 0;
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x) {
      Rgba8& p = img.at(x, y);
      if (p.a > 0 && p.a < 255) ++n;
      if (p.a < thr) p = Rgba8{};
      else p.a = 255;
    }
  return n;
}

// Every colour joins the most frequent colour within kMergeTolerance (a colour of
// the image, never a blend); returns the number of colours after merging.
int merge_colours(ImageRGBA& img) {
  std::map<uint32_t, int> hist;
  for (const Rgba8& p : img.pixels())
    if (p.a) ++hist[rgba_key(p)];
  std::vector<std::pair<int, uint32_t>> order;
  order.reserve(hist.size());
  for (const auto& [k, n] : hist) order.push_back({-n, k});
  std::sort(order.begin(), order.end());  // most frequent first, ties by colour
  std::vector<Rgba8> reps;
  std::map<uint32_t, Rgba8> to;
  for (const auto& [neg, k] : order) {
    const Rgba8 c = from_key(k);
    int best = -1, best_d = kMergeTolerance + 1;
    for (size_t i = 0; i < reps.size(); ++i) {
      const int d = channel_distance(c, reps[i]);
      if (d < best_d) best_d = d, best = static_cast<int>(i);
    }
    if (best < 0) {
      reps.push_back(c);
      to[k] = c;
    } else {
      to[k] = reps[static_cast<size_t>(best)];
    }
  }
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x)
      if (img.at(x, y).a) img.at(x, y) = to.at(rgba_key(img.at(x, y)));
  return static_cast<int>(reps.size());
}

// ---- figures (turnaround sheets) ---------------------------------------------------

struct Component {
  int area = 0;
  int x0 = 0, x1 = 0;
};

std::vector<Component> components(const Mask2D& m) {
  std::vector<Component> out;
  std::vector<uint8_t> seen(static_cast<size_t>(m.width()) * m.height(), 0);
  std::vector<std::pair<int, int>> stack;
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) {
      if (!m.get(x, y) || seen[static_cast<size_t>(y) * m.width() + x]) continue;
      Component c{0, x, x + 1};
      stack.push_back({x, y});
      seen[static_cast<size_t>(y) * m.width() + x] = 1;
      while (!stack.empty()) {
        const auto [cx, cy] = stack.back();
        stack.pop_back();
        ++c.area;
        c.x0 = std::min(c.x0, cx), c.x1 = std::max(c.x1, cx + 1);
        const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : nb) {
          const int nx = cx + d[0], ny = cy + d[1];
          if (!m.get_or_false(nx, ny) || seen[static_cast<size_t>(ny) * m.width() + nx]) continue;
          seen[static_cast<size_t>(ny) * m.width() + nx] = 1;
          stack.push_back({nx, ny});
        }
      }
      out.push_back(c);
    }
  return out;
}

// Large figures standing side by side (no horizontal overlap): a turnaround sheet.
int side_by_side_figures(const Mask2D& m) {
  std::vector<Component> cs = components(m);
  if (cs.size() < 2) return static_cast<int>(cs.size());
  const int largest = std::max_element(cs.begin(), cs.end(), [](auto& a, auto& b) { return a.area < b.area; })->area;
  std::vector<Component> big;
  for (const Component& c : cs)
    if (c.area >= kFigureShare * largest) big.push_back(c);
  std::sort(big.begin(), big.end(), [](auto& a, auto& b) { return a.x0 < b.x0; });
  for (size_t i = 1; i < big.size(); ++i)
    if (big[i].x0 < big[i - 1].x1) return 1;  // overlapping columns: parts of one figure
  return static_cast<int>(big.size());
}

// ---- shared axes ---------------------------------------------------------------------

enum Axis { AX = 0, AY = 1, AZ = 2 };
const char* axis_name(int a) { return a == AX ? "X" : a == AY ? "Y" : "Z"; }

// Image width and height of a view as world axes.
std::array<int, 2> view_axes(ViewRole r) {
  switch (r) {
    case ViewRole::Front:
    case ViewRole::Back: return {AX, AY};
    case ViewRole::Side:
    case ViewRole::Right: return {AZ, AY};
    case ViewRole::Top:
    case ViewRole::Bottom: return {AX, AZ};
  }
  return {AX, AY};
}

struct Work {
  SourceView* sv = nullptr;
  Facts facts;          // of the original
  ImageRGBA img;        // after background removal and clean-up (full resolution)
  bool modified = false;
  bool clean = true;
  double block = 1;
  bool exact = true;
  bool determined = true;  // the block size is known (not just "some divisor of the run length")
  double rescale = 1;   // per-view correction so shared axes agree
  Mask2D::Box bbox;     // opaque bbox of img
  double extent(int axis) const {  // native pixels along a world axis
    const auto ax = view_axes(sv->role);
    const int px = ax[0] == axis ? bbox.width() : bbox.height();
    return px / (block * rescale);
  }
  bool has(int axis) const {
    const auto ax = view_axes(sv->role);
    return ax[0] == axis || ax[1] == axis;
  }
};

}  // namespace

// ---- settings ---------------------------------------------------------------------------

std::optional<Message> check_normalize_settings(const NormalizeSettings& s) {
  auto bad = [](const std::string& field, const std::string& text) {
    return Message{"input.invalid", field + " " + text, Severity::Error, {{"field", field}}};
  };
  if (s.target_size < 16 || s.target_size > limits::kMaxModelSide)
    return bad("target_size", "must be between 16 and " + std::to_string(limits::kMaxModelSide) + " px");
  if (s.bg_tolerance < 0 || s.bg_tolerance > 255) return bad("bg_tolerance", "must be between 0 and 255");
  if (!(s.axis_tolerance >= 0 && s.axis_tolerance <= 1)) return bad("axis_tolerance", "must be between 0 and 1");
  return std::nullopt;
}

// ---- background -------------------------------------------------------------------------

BackgroundRemoval remove_background(const ImageRGBA& img, int tolerance, uint8_t alpha_threshold) {
  BackgroundRemoval r{img};
  const int w = img.width(), h = img.height();
  if (w < 3 || h < 3) return r;
  // Opaque border pixels (each position once).
  std::vector<Rgba8> border;
  auto add = [&](int x, int y) {
    if (img.at(x, y).a >= alpha_threshold) border.push_back(img.at(x, y));
  };
  for (int x = 0; x < w; ++x) add(x, 0), add(x, h - 1);
  for (int y = 1; y < h - 1; ++y) add(0, y), add(w - 1, y);
  const int ring = 2 * w + 2 * (h - 2);
  if (border.size() < static_cast<size_t>(ring) / 2) return r;
  // Candidate colours: the most frequent exact border colours.
  std::map<uint32_t, int> hist;
  for (const Rgba8& c : border) ++hist[rgba_key(Rgba8{c.r, c.g, c.b, 255})];
  std::vector<std::pair<int, uint32_t>> top;
  for (const auto& [k, n] : hist) top.push_back({-n, k});
  std::sort(top.begin(), top.end());
  if (top.size() > 24) top.resize(24);
  auto coverage = [&](Rgba8 c) {
    int n = 0;
    for (const Rgba8& b : border) n += channel_distance(b, c) <= tolerance ? 1 : 0;
    return n;
  };
  std::vector<Rgba8> bg;
  int best = -1;
  Rgba8 best_c{};
  for (const auto& [neg, k] : top) {
    const int cov = coverage(from_key(k));
    if (cov > best) best = cov, best_c = from_key(k);
  }
  const double total = static_cast<double>(ring);
  if (best >= kBorderDominance * total) {
    bg.push_back(best_c);
  } else {
    // A fake-transparency checkerboard: two colours that together cover the border.
    for (size_t i = 0; i < top.size() && bg.empty(); ++i)
      for (size_t j = i + 1; j < top.size() && bg.empty(); ++j) {
        const Rgba8 a = from_key(top[i].second), b = from_key(top[j].second);
        if (channel_distance(a, b) <= tolerance) continue;
        int ca = 0, cb = 0, both = 0;
        for (const Rgba8& p : border) {
          const bool in_a = channel_distance(p, a) <= tolerance, in_b = channel_distance(p, b) <= tolerance;
          ca += in_a, cb += in_b, both += (in_a || in_b);
        }
        if (ca >= kCheckerShare * total && cb >= kCheckerShare * total && both >= 0.8 * total) {
          bg = {a, b};
          r.checker = true;
        }
      }
  }
  if (bg.empty()) return r;
  r.colour = bg.front();
  auto is_bg = [&](const Rgba8& p) {
    if (p.a < alpha_threshold) return true;
    for (const Rgba8& c : bg)
      if (channel_distance(p, c) <= tolerance) return true;
    return false;
  };
  // Flood fill (4-connected) from every border pixel.
  std::vector<uint8_t> fill(static_cast<size_t>(w) * h, 0);
  std::vector<std::pair<int, int>> stack;
  auto seed = [&](int x, int y) {
    const size_t i = static_cast<size_t>(y) * w + x;
    if (!fill[i] && is_bg(img.at(x, y))) fill[i] = 1, stack.push_back({x, y});
  };
  for (int x = 0; x < w; ++x) seed(x, 0), seed(x, h - 1);
  for (int y = 0; y < h; ++y) seed(0, y), seed(w - 1, y);
  while (!stack.empty()) {
    const auto [x, y] = stack.back();
    stack.pop_back();
    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& d : nb) {
      const int nx = x + d[0], ny = y + d[1];
      if (nx >= 0 && ny >= 0 && nx < w && ny < h) seed(nx, ny);
    }
  }
  int removed = 0, remaining = 0;
  Mask2D::Box box{w, h, 0, 0};
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const Rgba8 p = img.at(x, y);
      if (p.a < alpha_threshold) continue;
      if (fill[static_cast<size_t>(y) * w + x]) {
        ++removed;
      } else {
        ++remaining;
        box.x0 = std::min(box.x0, x), box.y0 = std::min(box.y0, y);
        box.x1 = std::max(box.x1, x + 1), box.y1 = std::max(box.y1, y + 1);
      }
    }
  if (remaining == 0) {
    r.all_background = true;
    return r;
  }
  // A background reaches well past the border ring (an outline does not).
  if (removed < 3 * ring) return r;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (fill[static_cast<size_t>(y) * w + x]) r.image.at(x, y) = Rgba8{};
  r.removed = true;
  r.removed_px = removed;
  r.touches_edge = box.x0 == 0 || box.y0 == 0 || box.x1 == w || box.y1 == h;
  return r;
}

// ---- scale --------------------------------------------------------------------------

namespace {

// Lengths of uniform-colour runs inside the subject, along rows and columns, as
// counts per length (each run counts once: long runs must not outvote short
// ones). A run continues while colours stay within `tolerance` of its first
// pixel. Runs touching the image border are incomplete and left out; runs of
// transparent pixels are not art.
std::map<int, double> collect_runs(const ImageRGBA& img, uint8_t alpha_threshold, int tolerance) {
  std::map<int, double> runs;
  auto scan = [&](int n, int len, auto at) {
    for (int line = 0; line < n; ++line) {
      int start = 0;
      Rgba8 ref = at(line, 0);
      for (int i = 1; i <= len; ++i) {
        bool end = i == len;
        if (!end) {
          const Rgba8 c = at(line, i);
          end = (c.a >= alpha_threshold) != (ref.a >= alpha_threshold) ||
                (ref.a >= alpha_threshold && channel_distance(c, ref) > tolerance);
        }
        if (!end) continue;
        const int l = i - start;
        if (ref.a >= alpha_threshold && l >= 2 && start > 0 && i < len) runs[l] += 1;
        start = i;
        if (i < len) ref = at(line, i);
      }
    }
  };
  scan(img.height(), img.width(), [&](int y, int x) { return img.at(x, y); });
  scan(img.width(), img.height(), [&](int x, int y) { return img.at(x, y); });
  return runs;
}

// A strict k× grid (real-valued k): runs are whole numbers of blocks within 10 %.
double strict_grid(const ImageRGBA& img, uint8_t alpha_threshold, bool* determined) {
  const std::map<int, double> runs = collect_runs(img, alpha_threshold, 0);
  double total = 0;
  for (const auto& [l, n] : runs) total += n;
  if (total < 20) return 1;
  // Candidate block sizes on a real scale (art upscaled by 39.3 has runs of
  // 39 and 40 px that integer sizes drift away from). The score is the
  // matching share minus what random run lengths would match. Every divisor of
  // the true size matches too, so the largest size scoring close to the best wins.
  const double max_b = std::min({double(kMaxExactBlock), std::max(2.0, img.width() / 2.0), std::max(2.0, img.height() / 2.0)});
  auto excess_of = [&](double b) {
    const double tol = std::max(1.0, 0.1 * b);
    double hit = 0;
    for (const auto& [l, n] : runs) {
      const double k = std::max(1.0, std::round(l / b));
      if (std::abs(l - k * b) <= tol) hit += n;
    }
    return hit / total - std::min(1.0, (2 * tol + 1) / b);
  };
  std::vector<std::pair<double, double>> scored;  // (size, excess)
  double best_excess = 0;
  for (double b = 2; b <= max_b; b *= 1.01) {
    const double e = excess_of(b);
    scored.push_back({b, e});
    best_excess = std::max(best_excess, e);
  }
  if (best_excess < 0.3) return 1;
  double best_b = 1;
  for (const auto& [b, e] : scored)
    if (e >= 0.85 * best_excess) best_b = std::max(best_b, b);
  // Refine from the matching runs: total length / total block count.
  const double tol = std::max(1.0, 0.1 * best_b);
  double sum_l = 0, sum_n = 0;
  std::set<int> counts;  // distinct numbers of blocks per matching run
  for (const auto& [l, n] : runs) {
    const double k = std::max(1.0, std::round(l / best_b));
    if (std::abs(l - k * best_b) <= tol) sum_l += n * l, sum_n += n * k, counts.insert(static_cast<int>(k));
  }
  if (determined) *determined = counts.size() >= 2;
  return sum_n > 0 ? sum_l / sum_n : best_b;
}

// AI "pixel style" art has blocks of varying size (8 to 12 px around 10) and
// noise inside them: runs are traced with a colour tolerance and the block size
// is the dominant run length (the fundamental, not a multiple), accepted when
// enough runs are close to a whole number of blocks.
double irregular_grid(const ImageRGBA& img, uint8_t alpha_threshold, bool* determined) {
  std::map<int, double> hist;
  for (const auto& [l, n] : collect_runs(img, alpha_threshold, kRunTolerance))
    if (l >= 3) hist[l] += n;
  double total = 0;
  for (const auto& [l, n] : hist) total += n;
  if (total < 20) return 1;
  auto near = [&](double c) {
    double s = 0;
    for (const auto& [l, n] : hist)
      if (std::abs(l - c) <= std::max(1.0, 0.2 * c)) s += n;
    return s;
  };
  double best = 0, best_n = -1;
  for (const auto& [l, n] : hist) {
    const double v = near(l);
    if (v > best_n) best_n = v, best = l;  // ties: the shorter length (map order)
  }
  while (best / 2 >= 3 && near(best / 2) >= 0.5 * near(best)) best /= 2;
  const double share = near(best) / total;
  double regular = 0, multiples = 0;
  for (const auto& [l, n] : hist) {
    const double k = std::max(1.0, std::round(l / best));
    if (std::abs(l - k * best) <= std::max(1.0, 0.25 * best)) regular += n, multiples += k >= 2 ? n : 0;
  }
  if (share < 0.2 || regular / total < 0.4) return 1;
  double sum_l = 0, sum_n = 0;
  for (const auto& [l, n] : hist)
    if (std::abs(l - best) <= std::max(1.0, 0.2 * best)) sum_l += n * l, sum_n += n;
  if (determined) *determined = multiples / total >= 0.05;
  return sum_n > 0 ? sum_l / sum_n : best;
}

}  // namespace

double estimate_block_size(const ImageRGBA& img, uint8_t alpha_threshold, bool* determined) {
  if (determined) *determined = false;
  const double strict = strict_grid(img, alpha_threshold, determined);
  if (strict > 1) return strict;
  return irregular_grid(img, alpha_threshold, determined);
}

ImageRGBA resample_mode(const ImageRGBA& src, Mask2D::Box region, int out_w, int out_h, uint8_t alpha_threshold) {
  ImageRGBA out(std::max(1, out_w), std::max(1, out_h));
  const double fx = double(region.width()) / out.width(), fy = double(region.height()) / out.height();
  auto range = [](double a, double b, double f, int lo_clip, int hi_clip) {
    const double c = (a + b) / 2;
    int lo = static_cast<int>(std::floor(a + kInner * f)), hi = static_cast<int>(std::ceil(b - kInner * f));
    if (hi <= lo) lo = static_cast<int>(std::floor(c)), hi = lo + 1;
    lo = std::clamp(lo, lo_clip, hi_clip - 1), hi = std::clamp(hi, lo + 1, hi_clip);
    return std::pair{lo, hi};
  };
  std::vector<uint32_t> keys;
  for (int oy = 0; oy < out.height(); ++oy) {
    const auto [y0, y1] = range(region.y0 + oy * fy, region.y0 + (oy + 1) * fy, fy, 0, src.height());
    for (int ox = 0; ox < out.width(); ++ox) {
      const auto [x0, x1] = range(region.x0 + ox * fx, region.x0 + (ox + 1) * fx, fx, 0, src.width());
      keys.clear();
      int total = 0;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          ++total;
          const Rgba8 p = src.at(x, y);
          if (p.a >= alpha_threshold) keys.push_back(rgba_key(p));
        }
      if (keys.empty() || 2 * static_cast<int>(keys.size()) < total) continue;  // mostly transparent
      std::sort(keys.begin(), keys.end());
      uint32_t best = keys[0];
      size_t best_n = 0;
      for (size_t i = 0; i < keys.size();) {
        size_t j = i;
        while (j < keys.size() && keys[j] == keys[i]) ++j;
        if (j - i > best_n) best_n = j - i, best = keys[i];  // ties: the smaller colour (first in order)
        i = j;
      }
      out.at(ox, oy) = from_key(best);
    }
  }
  return out;
}

// ---- diagnostics ------------------------------------------------------------------------

nlohmann::json view_diagnostics(const ImageRGBA& img, uint8_t alpha_threshold) {
  const Facts f = facts_of(img, alpha_threshold);
  const int k = exact_block(img, alpha_threshold, 16);
  std::string cause = "drawing_too_large";
  if (f.transparent_share() < kNoTransparency && background_accepted(f, remove_background(img, 24, alpha_threshold)))
    cause = "opaque_background";
  else if (!f.clean()) cause = "not_clean_pixel_art";
  return {{"original_size", {f.w, f.h}},
          {"opaque_bbox", {f.bbox.x0, f.bbox.y0, f.bbox.width(), f.bbox.height()}},
          {"transparent_share", round3(f.transparent_share())},
          {"partial_alpha_share", round3(f.partial_share())},
          {"detected_scale", k},
          {"colours", f.colours},
          {"likely_cause", cause}};
}

nlohmann::json to_json(const NormalizeSettings& s) {
  return {{"enabled", s.enabled},
          {"target_size", s.target_size},
          {"bg_tolerance", s.bg_tolerance},
          {"axis_tolerance", s.axis_tolerance}};
}

nlohmann::json normalization_json(const SourceView& v) {
  const ViewNormalization& n = v.norm;
  nlohmann::json j{{"applied", n.applied}};
  if (!n.applied) return j;
  j["background_removed"] = n.background_removed;
  if (n.background_removed) {
    j["background"] = hex(n.background);
    j["background_share"] = round3(n.background_share);
  }
  j["alpha_snapped"] = n.alpha_snapped;
  if (n.colours_before) j["colours"] = {{"before", n.colours_before}, {"after", n.colours_after}};
  j["block"] = round3(n.block);
  j["block_exact"] = n.block_exact;
  j["factor"] = round3(n.factor);
  if (v.working) j["working_size"] = {v.working->width(), v.working->height()};
  return j;
}

// ---- the stage -----------------------------------------------------------------------------

Expected<NormalizeResult> normalize_views(std::vector<SourceView>& views, const NormalizeOptions& opt) {
  for (SourceView& v : views) {
    v.working.reset();
    v.norm = ViewNormalization{};
  }
  NormalizeResult res;
  if (!opt.settings.enabled || views.empty()) return res;
  for (const SourceView& v : views)
    if (!v.original) return res;  // the aligner reports missing images
  const uint8_t thr = opt.alpha_threshold;
  const NormalizeSettings& s = opt.settings;
  auto info = [&](std::string code, std::string text, nlohmann::json where, Severity sev = Severity::Info) {
    res.messages.push_back(Message{std::move(code), std::move(text), sev, std::move(where)});
  };

  // 1. Background and clean-up, per view.
  std::vector<Work> work;
  for (SourceView& v : views) {
    Work w;
    w.sv = &v;
    w.facts = facts_of(*v.original, thr);
    w.clean = w.facts.clean();
    w.img = *v.original;
    const std::string role = role_name(v.role);
    if (w.facts.transparent_share() < kNoTransparency) {
      const BackgroundRemoval b = remove_background(w.img, s.bg_tolerance, thr);
      if (b.all_background && (!w.clean || w.facts.colours > 1))
        return make_error("input.no_subject",
                          Role(v.role) + " is only background: every pixel is within " +
                              std::to_string(s.bg_tolerance) + " of its border colour " + hex(b.colour) +
                              ". Use an image that shows the object, on a plain background or with transparency.",
                          {{"view", role}, {"colour", hex(b.colour)}, {"tolerance", s.bg_tolerance}});
      if (background_accepted(w.facts, b)) {
        const double share = double(b.removed_px) / (double(w.facts.w) * w.facts.h);
        w.img = b.image;
        w.modified = true;
        v.norm.background_removed = true;
        v.norm.background = b.colour;
        v.norm.background_share = share;
        info("input.background_removed",
             Role(v.role) + ": removed the " + (b.checker ? "checkerboard " : "") + "background " + hex(b.colour) + " (" +
                 std::to_string(static_cast<int>(std::lround(share * 100))) + " % of the image)",
             {{"view", role}, {"colour", hex(b.colour)}, {"checker", b.checker}, {"removed_share", round3(share)},
              {"tolerance", s.bg_tolerance}});
        if (share > kLeakShare || b.touches_edge)
          info("input.background_leak_possible",
               Role(v.role) + ": the background fill removed " + std::to_string(static_cast<int>(std::lround(share * 100))) +
                   " % of the image" + (b.touches_edge ? " and the object touches the image edge" : "") +
                   "; check that no part of the object was removed (lower bg_tolerance if it was)",
               {{"view", role}, {"removed_share", round3(share)}, {"touches_edge", b.touches_edge},
                {"tolerance", s.bg_tolerance}},
               Severity::Warning);
      } else if (!w.clean) {
        info("input.background_not_found",
             Role(v.role) + ": no uniform background colour around the edge; the whole image is treated as the object",
             {{"view", role}}, Severity::Warning);
      }
    }
    if (!w.clean) {
      const int snapped = snap_alpha(w.img, thr);
      v.norm.alpha_snapped = snapped;
      if (snapped > 0)
        info("input.alpha_snapped",
             Role(v.role) + ": " + std::to_string(snapped) + " semi-transparent pixels set fully opaque or transparent",
             {{"view", role}, {"pixels", snapped}, {"alpha_threshold", thr}});
      const int before = facts_of(w.img, thr).colours;
      const int after = merge_colours(w.img);
      v.norm.colours_before = before;
      v.norm.colours_after = after;
      if (after < before)
        info("input.colours_merged",
             Role(v.role) + ": " + std::to_string(before) + " colours merged into " + std::to_string(after) +
                 " (anti-aliasing and compression noise)",
             {{"view", role}, {"before", before}, {"after", after}, {"tolerance", kMergeTolerance}});
      w.modified = true;
    }
    w.bbox = opaque_mask(w.img, thr).bbox();
    if (w.bbox.empty())
      return make_error("input.no_subject", Role(v.role) + " has no visible object after removing its background",
                        {{"view", role}});
    if (w.modified) {
      const int figures = side_by_side_figures(opaque_mask(w.img, thr));
      if (figures >= 2)
        return make_error("input.turnaround_sheet",
                          Role(v.role) + " shows " + std::to_string(figures) +
                              " separate figures side by side, like a turnaround sheet. Give each view as a separate "
                              "image showing one figure.",
                          {{"view", role}, {"figures", figures}});
    }
    work.push_back(std::move(w));
  }

  // 2. Scale. Clean views first try the aligner's own detection.
  const bool any_modified = std::any_of(work.begin(), work.end(), [](const Work& w) { return w.modified; });
  int g = 0;
  for (const Work& w : work) g = std::gcd(g, change_spacing_gcd(w.img));
  for (const Work& w : work) g = std::gcd(g, std::gcd(w.bbox.width(), w.bbox.height()));
  const int k_common = scale_from_gcd(g);
  std::array<double, 3> common_dims{0, 0, 0};
  for (Work& w : work) {
    w.block = k_common;
    for (int a : {AX, AY, AZ})
      if (w.has(a)) common_dims[a] = std::max(common_dims[a], std::ceil(w.extent(a)));
  }
  const bool fits = common_dims[AX] <= limits::kMaxModelSide && common_dims[AY] <= limits::kMaxModelSide &&
                    common_dims[AZ] <= limits::kMaxModelSide &&
                    common_dims[AX] * common_dims[AY] * common_dims[AZ] <= double(limits::kMaxModelCells);
  // Edge anti-aliasing (any semi-transparent pixel) in art too large to align as it is.
  const bool antialiased = std::any_of(work.begin(), work.end(), [](const Work& w) { return w.facts.partial > 0; });
  auto shared_axes_agree = [&](double tol_px) {
    for (int a : {AX, AY, AZ}) {
      double lo = 1e18, hi = -1;
      for (const Work& w : work)
        if (w.has(a)) lo = std::min(lo, w.extent(a)), hi = std::max(hi, w.extent(a));
      if (hi >= 0 && hi - lo > tol_px) return false;
    }
    return true;
  };
  bool per_view = false;
  if (!any_modified) {
    // Views exported at different integer factors (3×, 4×): their own grids line up, the shared one does not.
    std::vector<int> own;
    for (const Work& w : work) own.push_back(exact_block(w.img, thr, 16));
    const bool differ = std::any_of(own.begin(), own.end(), [&](int k) { return k != k_common; });
    if (differ && !opt.single_view && !shared_axes_agree(opt.extent_tolerance)) {
      for (size_t i = 0; i < work.size(); ++i) work[i].block = own[i];
      per_view = shared_axes_agree(opt.extent_tolerance);
      if (!per_view)
        for (Work& w : work) w.block = k_common;
    }
    // Clean pixel art that aligns today is never touched. Too large without any
    // anti-aliasing (a solid native-size drawing) it is not touched either: the
    // aligner then reports view.too_large with a diagnosis of every view.
    if (!per_view && (fits || !antialiased)) return res;
  }

  // 3. Per-view block size for views that are not settled yet. A view this stage
  // changed gets its own exact grid (up to 128×) or an estimate; a clean view
  // only an estimate, which needs many runs (an exact grid on one small simple
  // sprite is easily fake: a solid 4×4 square looks 4×).
  if (!per_view)
    for (Work& w : work) {
      const int k = w.modified ? exact_block(w.img, thr, kMaxExactBlock) : 1;
      if (k > 1) {
        w.block = k, w.exact = true;
      } else {
        bool determined = false;
        w.block = estimate_block_size(w.img, thr, &determined);
        w.exact = w.block == 1;
        w.determined = w.block == 1 || determined;
      }
    }
  // A view whose runs cannot tell its block size (one flat colour across) takes
  // it from a view it shares an axis with; without one it keeps every pixel.
  for (Work& w : work) {
    if (w.determined) continue;
    const Work* ref = nullptr;
    int axis = -1;
    for (const ViewRole r : {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Back, ViewRole::Right, ViewRole::Bottom})
      for (const Work& o : work)
        if (!ref && &o != &w && o.sv->role == r && o.determined)
          for (int a : {AX, AY, AZ})
            if (!ref && w.has(a) && o.has(a)) ref = &o, axis = a;
    const std::string role = role_name(w.sv->role);
    if (ref && !opt.single_view) {
      const auto ax = view_axes(w.sv->role);
      const int px = ax[0] == axis ? w.bbox.width() : w.bbox.height();
      w.block = px / std::max(1e-9, ref->extent(axis));
      w.exact = false;
      w.determined = true;
      info("input.scale_from_other_view",
           Role(w.sv->role) + " shows no pixel grid of its own (its runs fit any multiple); its scale was taken from " +
               Role(ref->sv->role) + " on the " + axis_name(axis) + " axis",
           {{"view", role}, {"from", role_name(ref->sv->role)}, {"axis", axis_name(axis)}, {"block", round3(w.block)}});
    } else {
      w.block = 1, w.exact = true, w.determined = true;
    }
  }
  {
    double lo = 1e18, hi = 0;
    for (const Work& w : work) lo = std::min(lo, w.block), hi = std::max(hi, w.block);
    if (work.size() > 1 && hi > lo * 1.02) {
      nlohmann::json factors = nlohmann::json::object();
      std::string list;
      for (const Work& w : work) {
        factors[role_name(w.sv->role)] = w.exact ? nlohmann::json(static_cast<int>(w.block)) : nlohmann::json(round3(w.block));
        list += (list.empty() ? "" : ", ") + Role(w.sv->role) + " " + std::to_string(w.block).substr(0, 5) + "x";
      }
      info("input.scale_per_view",
           "the views are drawn at different scales (" + list + "); each is reduced by its own factor",
           {{"factors", factors}}, Severity::Warning);
    }
  }

  // 4. Shared axes: small differences are evened out against Front, large ones refused.
  auto front_it = std::find_if(work.begin(), work.end(), [](const Work& w) { return w.sv->role == ViewRole::Front; });
  auto disagree = [&](int axis, const Work& a, const Work& b) -> std::optional<Message> {
    const double ea = a.extent(axis), eb = b.extent(axis);
    const double rel = std::max(ea, eb) / std::max(1e-9, std::min(ea, eb)) - 1;
    if (rel <= s.axis_tolerance) return std::nullopt;
    const std::string an = axis_name(axis);
    return Message{"input.views_inconsistent",
                   Role(b.sv->role) + " and " + Role(a.sv->role) + " disagree on the " + an + " axis: " +
                       Role(b.sv->role) + " is " + std::to_string(static_cast<int>(std::lround(eb))) + " px and " +
                       Role(a.sv->role) + " " + std::to_string(static_cast<int>(std::lround(ea))) +
                       " px at the detected pixel scale (" + std::to_string(static_cast<int>(std::lround(rel * 100))) +
                       " % apart, the tolerance is " + std::to_string(static_cast<int>(std::lround(s.axis_tolerance * 100))) +
                       " %). Each view must be a straight orthographic drawing of the same object at the same scale; "
                       "check that " + Role(b.sv->role) + " really is the " + role_name(b.sv->role) + " view.",
                   Severity::Error,
                   {{"axis", an},
                    {"views", {role_name(a.sv->role), role_name(b.sv->role)}},
                    {"extents", {{role_name(a.sv->role), round3(ea)}, {role_name(b.sv->role), round3(eb)}}},
                    {"factors", {{role_name(a.sv->role), round3(a.block * a.rescale)}, {role_name(b.sv->role), round3(b.block * b.rescale)}}},
                    {"tolerance", s.axis_tolerance}}};
  };
  if (!opt.single_view && front_it != work.end()) {
    const Work& front = *front_it;
    for (Work& w : work) {
      if (&w == &front) continue;
      const auto ax = view_axes(w.sv->role);
      const int axis = (ax[0] == AX || ax[1] == AX) ? AX : AY;  // the axis this view shares with Front
      if (auto e = disagree(axis, front, w)) return Unexpected{*e};
      const double diff = w.extent(axis) - front.extent(axis);
      if (std::abs(diff) > opt.extent_tolerance) {
        w.rescale = w.extent(axis) / front.extent(axis);
        info("input.view_rescaled",
             Role(w.sv->role) + " is " + std::to_string(static_cast<int>(std::lround(std::abs(w.rescale - 1) * 100))) +
                 " % " + (w.rescale > 1 ? "larger" : "smaller") + " than Front on the " + axis_name(axis) +
                 " axis; it was scaled to match",
             {{"view", role_name(w.sv->role)}, {"axis", axis_name(axis)}, {"factor", round3(1 / w.rescale)}},
             Severity::Warning);
      }
    }
    // Every other shared axis must agree too (Z between Side and Top, ...).
    for (size_t i = 0; i < work.size(); ++i)
      for (size_t j = 0; j < work.size(); ++j) {
        if (i == j) continue;
        for (int a : {AX, AY, AZ})
          if (work[i].has(a) && work[j].has(a) && work[j].sv->role != ViewRole::Front)
            if (auto e = disagree(a, work[i], work[j])) return Unexpected{*e};
      }
  }

  // 5. Fit: one common factor so the longest side meets the target and the limits hold.
  std::array<double, 3> dims{0, 0, 0};
  for (const Work& w : work)
    for (int a : {AX, AY, AZ})
      if (w.has(a)) dims[a] = std::max(dims[a], w.extent(a));
  const double longest = std::max({dims[AX], dims[AY], dims[AZ]});
  double fit = 1;
  if (longest > s.target_size) fit = longest / s.target_size;
  const double cells = std::max(1.0, dims[AX]) * std::max(1.0, dims[AY]) * std::max(1.0, dims[AZ]);
  if (!opt.single_view && cells / (fit * fit * fit) > double(limits::kMaxModelCells))
    fit = std::cbrt(cells / double(limits::kMaxModelCells));
  if (fit > 1)
    info("input.fitted",
         [&] {
           std::string t;
           for (int a : {AX, AY, AZ})
             if (dims[a] > 0) t += (t.empty() ? "" : "x") + std::to_string(static_cast<int>(std::lround(dims[a])));
           return "the art would be " + t;
         }() + " px; every view was reduced by " +
             std::to_string(fit).substr(0, 5) + " so the longest side is " + std::to_string(s.target_size) +
             " px (target_size)",
         {{"target_size", s.target_size},
          {"factor", round3(fit)},
          {"from", {round3(dims[AX]), round3(dims[AY]), round3(dims[AZ])}},
          {"to", {round3(dims[AX] / fit), round3(dims[AY] / fit), round3(dims[AZ] / fit)}}});

  // 6. Working images: whole pixels, each the dominant colour of its cell.
  for (Work& w : work) {
    SourceView& v = *w.sv;
    const double factor = w.block * w.rescale * fit;
    ImageRGBA out;
    if (w.exact && w.rescale == 1 && fit == 1) {
      out = downscale_nearest(w.img, scale_with_k(w.img, static_cast<int>(w.block)));
    } else {
      const int ow = std::max(1, static_cast<int>(std::lround(w.bbox.width() / factor)));
      const int oh = std::max(1, static_cast<int>(std::lround(w.bbox.height() / factor)));
      out = resample_mode(w.img, w.bbox, ow, oh, thr);
    }
    if (opaque_mask(out, thr).count() == 0)
      return make_error("input.no_subject",
                        Role(v.role) + " has nothing left after reducing it by " + std::to_string(factor).substr(0, 5) +
                            ": the object is too thin; raise target_size",
                        {{"view", role_name(v.role)}, {"factor", round3(factor)}});
    v.norm.applied = true;
    v.norm.block = w.block;
    v.norm.block_exact = w.exact;
    v.norm.factor = factor;
    const Mask2D::Box ob = opaque_mask(out, thr).bbox();
    if (factor != 1)
      info("input.downscaled",
           Role(v.role) + ": " + std::to_string(w.bbox.width()) + "x" + std::to_string(w.bbox.height()) + " px of art became " +
               std::to_string(ob.width()) + "x" + std::to_string(ob.height()) + " px (each pixel is the main colour of a " +
               std::to_string(factor).substr(0, 5) + " px block" + (w.exact ? "" : ", block size estimated") + ")",
           {{"view", role_name(v.role)},
            {"from", {w.bbox.width(), w.bbox.height()}},
            {"to", {ob.width(), ob.height()}},
            {"factor", round3(factor)},
            {"block", round3(w.block)},
            {"block_exact", w.exact}});
    v.working = std::make_shared<const ImageRGBA>(std::move(out));
  }
  res.applied = true;
  return res;
}

}  // namespace pixel3d
