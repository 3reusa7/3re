#include "pixel3d/views/align.hpp"

#include <algorithm>
#include <cmath>
#include <array>
#include <numeric>
#include <set>

#include "pixel3d/common/limits.hpp"
#include "pixel3d/image/normalize.hpp"

namespace pixel3d {
namespace {

using Profile = std::vector<uint8_t>;
enum Axis { AX = 0, AY = 1, AZ = 2 };

// Which world axis each image axis represents, and whether it runs backwards.
struct AxisMap {
  Axis u;
  bool u_rev;
  Axis v;
  bool v_rev;
};

AxisMap axis_map(ViewRole r) {
  switch (r) {
    case ViewRole::Front: return {AX, false, AY, true};
    case ViewRole::Back: return {AX, true, AY, true};
    case ViewRole::Side: return {AZ, false, AY, true};
    case ViewRole::Right: return {AZ, true, AY, true};
    case ViewRole::Top: return {AX, false, AZ, false};
    case ViewRole::Bottom: return {AX, false, AZ, true};
  }
  return {AX, false, AY, true};
}

Profile col_any(const Mask2D& m) {
  Profile p(m.width(), 0);
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) p[x] |= m.get(x, y) ? 1 : 0;
  return p;
}

Profile row_any(const Mask2D& m) {
  Profile p(m.height(), 0);
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x) p[y] |= m.get(x, y) ? 1 : 0;
  return p;
}

Profile reversed(Profile p) {
  std::reverse(p.begin(), p.end());
  return p;
}

// Profile of `m` along world axis `a`, indexed by world coordinate.
Profile world_profile(const Mask2D& m, ViewRole r, Axis a) {
  const AxisMap am = axis_map(r);
  if (am.u == a) return am.u_rev ? reversed(col_any(m)) : col_any(m);
  return am.v_rev ? reversed(row_any(m)) : row_any(m);
}

// Opaque pixel count per world coordinate along axis `a` (row/column structure).
std::vector<double> world_counts(const Mask2D& m, ViewRole r, Axis a) {
  const AxisMap am = axis_map(r);
  const bool along_u = am.u == a;
  std::vector<double> c(along_u ? m.width() : m.height(), 0);
  for (int y = 0; y < m.height(); ++y)
    for (int x = 0; x < m.width(); ++x)
      if (m.get(x, y)) c[along_u ? x : y] += 1;
  if (along_u ? am.u_rev : am.v_rev) std::reverse(c.begin(), c.end());
  return c;
}

// Pearson correlation of `mine` placed at offset o against `ref` (0 if flat).
double placed_correlation(const std::vector<double>& mine, int o, const std::vector<double>& ref) {
  const size_t n = ref.size();
  std::vector<double> a(n, 0);
  for (size_t i = 0; i < mine.size() && i + o < n; ++i) a[i + o] = mine[i];
  double ma = 0, mb = 0;
  for (size_t i = 0; i < n; ++i) ma += a[i], mb += ref[i];
  ma /= n, mb /= n;
  double ab = 0, aa = 0, bb = 0;
  for (size_t i = 0; i < n; ++i) ab += (a[i] - ma) * (ref[i] - mb), aa += (a[i] - ma) * (a[i] - ma), bb += (ref[i] - mb) * (ref[i] - mb);
  return (aa <= 0 || bb <= 0) ? 0 : ab / std::sqrt(aa * bb);
}

bool has_axis(ViewRole r, Axis a) {
  const AxisMap am = axis_map(r);
  return am.u == a || am.v == a;
}

// Fewest disagreeing cells over the placements of the shorter profile inside
// the longer one (lengths may differ by the extent mismatch).
int best_mismatch(const Profile& a, const Profile& b) {
  const int la = static_cast<int>(a.size()), lb = static_cast<int>(b.size());
  const int len = std::max(la, lb);
  int best = len + 1;
  for (int s = std::min(0, lb - la); s <= std::max(0, lb - la); ++s) {  // a placed at offset s within b's frame
    int bad = 0;
    for (int i = std::min(0, s); i < std::max(lb, la + s); ++i) {
      const int ia = i - s;
      const uint8_t va = (ia >= 0 && ia < la) ? a[ia] : 0;
      const uint8_t vb = (i >= 0 && i < lb) ? b[i] : 0;
      bad += va != vb ? 1 : 0;
    }
    best = std::min(best, bad);
  }
  return best;
}

struct Work {
  SourceView* sv;
  ImageRGBA img;  // downscaled, cropped, flipped
  Mask2D mask;
};

// The image alignment starts from: the normalised working image when there is one.
const ImageRGBA& source_of(const SourceView& sv) { return sv.working ? *sv.working : *sv.original; }

bool too_large(const Dims& d) {
  return d.W > limits::kMaxModelSide || d.H > limits::kMaxModelSide || d.D > limits::kMaxModelSide ||
         static_cast<int64_t>(d.W) * d.H * d.D > limits::kMaxModelCells;
}

// view.too_large with what each image looks like and the likely cause.
Message too_large_error(const Dims& d, const std::vector<SourceView>& views, uint8_t alpha_threshold) {
  nlohmann::json per_view = nlohmann::json::object();
  std::set<int> scales;
  std::set<std::string> causes;
  for (const SourceView& v : views) {
    nlohmann::json j = view_diagnostics(*v.original, alpha_threshold);
    if (v.working) j["normalized_size"] = {v.working->width(), v.working->height()};
    scales.insert(j["detected_scale"].get<int>());
    causes.insert(j["likely_cause"].get<std::string>());
    per_view[std::string(to_string(v.role))] = std::move(j);
  }
  // Clean views at different integer scales share no common factor.
  if (scales.size() > 1 && causes == std::set<std::string>{"drawing_too_large"})
    for (auto& [role, j] : per_view.items()) j["likely_cause"] = "different_scales";
  std::string why;
  for (const auto& [role, j] : per_view.items())
    why += (why.empty() ? "" : "; ") + role + ": " + j["likely_cause"].get<std::string>();
  return Message{"view.too_large",
                 "the model would be " + std::to_string(d.W) + "x" + std::to_string(d.H) + "x" + std::to_string(d.D) +
                     " px; the limit is " + std::to_string(limits::kMaxModelSide) + " px per axis and " +
                     std::to_string(limits::kMaxModelCells) + " cells (likely cause per view - " + why + ")",
                 Severity::Error,
                 {{"dims", {d.W, d.H, d.D}}, {"views", per_view}}};
}

int extent_of(const Work& w, Axis a) {
  const AxisMap am = axis_map(w.sv->role);
  return am.u == a ? w.img.width() : w.img.height();
}

}  // namespace

Expected<AlignResult> auto_align(std::vector<SourceView>& views, const AlignOptions& opt) {
  // Roles: required present, no duplicates.
  std::map<ViewRole, size_t> by_role;
  for (size_t i = 0; i < views.size(); ++i) {
    if (!by_role.emplace(views[i].role, i).second)
      return make_error("view.duplicate", "two views have role " + std::string(to_string(views[i].role)),
                        {{"view", to_string(views[i].role)}});
    if (!views[i].original)
      return make_error("view.no_image", "view has no image", {{"view", to_string(views[i].role)}});
  }
  for (ViewRole r : kRequiredViewRoles)
    if (!by_role.contains(r))
      return make_error("view.missing_required", "required view missing: " + std::string(to_string(r)),
                        {{"view", to_string(r)}});

  AlignResult res;
  // Normalised views (image/normalize.hpp) are already at native scale.
  const bool normalised = std::any_of(views.begin(), views.end(), [](const SourceView& v) { return v.working != nullptr; });
  // One block size for all views: the gcd of every view's colour-change spacing.
  // Per-view detection is fragile on simple shapes (a solid 2-px-wide view looks 2×).
  int g = 0;
  if (!normalised) {
    for (const auto& sv : views) g = std::gcd(g, change_spacing_gcd(*sv.original));
    // A genuine k× export also has opaque bounding boxes that are multiples of k;
    // small simple sprites can fake a change-spacing gcd without that.
    for (const auto& sv : views) {
      const Mask2D::Box b = opaque_mask(*sv.original, opt.alpha_threshold).bbox();
      if (!b.empty()) g = std::gcd(g, std::gcd(b.width(), b.height()));
    }
  }
  const int k = normalised ? 1 : scale_from_gcd(g);

  std::map<ViewRole, Work> work;
  for (auto& sv : views) {
    sv.scale = scale_with_k(source_of(sv), k);
    ImageRGBA img = downscale_nearest(source_of(sv), sv.scale);
    const Mask2D full = opaque_mask(img, opt.alpha_threshold);
    if (full.count() == 0)
      return make_error("view.empty", "view has no opaque pixels: " + std::string(to_string(sv.role)),
                        {{"view", to_string(sv.role)}});
    sv.crop = full.bbox();
    img = crop(img, sv.crop);
    work.emplace(sv.role, Work{&sv, img, opaque_mask(img, opt.alpha_threshold)});
    if (sv.scale.k > 1)
      res.messages.push_back(Message{"view_downscaled", "artwork downscaled by " + std::to_string(sv.scale.k),
                                     Severity::Info, {{"view", to_string(sv.role)}, {"k", sv.scale.k}}});
  }

  // Mirror detection against reference axes: X from Front, Z from Top's rows.
  const Profile ref_x = col_any(work.at(ViewRole::Front).mask);
  auto decide_flip = [&](ViewRole r, const Profile& ref_world) {
    auto it = work.find(r);
    if (it == work.end()) return;
    Work& w = it->second;
    const AxisMap am = axis_map(r);
    const Profile expected = am.u_rev ? reversed(ref_world) : ref_world;
    const Profile cols = col_any(w.mask);
    const int normal = best_mismatch(cols, expected), flipped = best_mismatch(reversed(cols), expected);
    // 1.0 when one orientation is perfect and the other is not; 0 when equal.
    const int worst = std::max(normal, flipped);
    res.flip_confidence[r] = worst == 0 ? 0.0 : static_cast<double>(std::abs(normal - flipped)) / worst;
    bool flip = flipped < normal && res.flip_confidence[r] >= 0.5;
    if (auto f = opt.force_flip.find(r); f != opt.force_flip.end()) {
      flip = f->second;
      w.sv->flip_user_override = true;
    }
    w.sv->flipped_x = flip;
    if (flip) {
      w.img = flip_x(w.img);
      w.mask = flip_x(w.mask);
      res.messages.push_back(Message{"view_flipped", "view was mirrored horizontally to match the others",
                                     Severity::Info, {{"view", to_string(r)}, {"confidence", res.flip_confidence[r]}}});
    } else if (res.flip_confidence[r] < 0.05 && !w.sv->flip_user_override) {
      res.messages.push_back(Message{"flip_undetermined", "orientation cannot be checked (symmetric profile)",
                                     Severity::Info, {{"view", to_string(r)}}});
    }
  };
  decide_flip(ViewRole::Top, ref_x);
  const Profile ref_z = row_any(work.at(ViewRole::Top).mask);  // Top rows: v == z
  decide_flip(ViewRole::Side, ref_z);
  decide_flip(ViewRole::Back, ref_x);
  decide_flip(ViewRole::Right, ref_z);
  decide_flip(ViewRole::Bottom, ref_x);

  // Extents: every axis takes the largest contributor; others are padded where
  // their occupancy profile agrees best (ties: start of the axis, i.e. bottom for Y).
  std::array<int, 3> target{0, 0, 0};
  for (auto& [role, w] : work)
    for (Axis a : {AX, AY, AZ})
      if (has_axis(role, a)) target[a] = std::max(target[a], extent_of(w, a));
  res.dims = Dims{target[AX], target[AY], target[AZ]};
  if (too_large(res.dims)) return Unexpected{too_large_error(res.dims, views, opt.alpha_threshold)};

  std::array<Profile, 3> reference;
  std::array<std::vector<double>, 3> reference_counts;
  for (Axis a : {AX, AY, AZ})
    for (auto& [role, w] : work)
      if (has_axis(role, a) && extent_of(w, a) == target[a]) {
        reference[a] = world_profile(w.mask, role, a);
        reference_counts[a] = world_counts(w.mask, role, a);
        break;
      }

  for (auto& [role, w] : work) {
    const AxisMap am = axis_map(role);
    int pad[2] = {0, 0};  // image u, v
    for (int k = 0; k < 2; ++k) {
      const Axis a = k == 0 ? am.u : am.v;
      const bool rev = k == 0 ? am.u_rev : am.v_rev;
      const int size = extent_of(w, a), diff = target[a] - size;
      if (diff <= 0) continue;
      const Profile mine = world_profile(w.mask, role, a);
      // Occupancy agreement first; when it ties (e.g. every row occupied in both
      // views) the row/column structure decides, never the axis start.
      const std::vector<double> counts = world_counts(w.mask, role, a);
      int best_o = 0, best_agree = -1;
      double best_corr = -2;
      for (int o = 0; o <= diff; ++o) {
        int agree = 0;
        for (int i = 0; i < target[a]; ++i) {
          const uint8_t v = (i >= o && i < o + size) ? mine[i - o] : 0;
          agree += v == reference[a][i] ? 1 : 0;
        }
        const double corr = placed_correlation(counts, o, reference_counts[a]);
        if (agree > best_agree || (agree == best_agree && corr > best_corr + 1e-9))
          best_agree = agree, best_corr = corr, best_o = o;
      }
      pad[k] = rev ? diff - best_o : best_o;
      const bool within = diff <= opt.extent_tolerance;
      res.messages.push_back(Message{within ? "extent_padded" : "extent_mismatch",
                                     std::string(to_string(role)) + " is " + std::to_string(diff) +
                                         " px smaller than the other views on axis " + "XYZ"[a],
                                     within ? Severity::Warning : Severity::Error,
                                     {{"view", to_string(role)}, {"axis", std::string(1, "XYZ"[a])}, {"diff", diff}}});
    }
    const ImageSize sz = view_image_size(role, res.dims);
    ImageRGBA aligned(sz.w, sz.h);
    for (int y = 0; y < w.img.height(); ++y)
      for (int x = 0; x < w.img.width(); ++x) aligned.at(x + pad[0], y + pad[1]) = w.img.at(x, y);
    w.sv->pad_x = pad[0];
    w.sv->pad_y = pad[1];
    w.img = std::move(aligned);
    w.mask = opaque_mask(w.img, opt.alpha_threshold);
    w.sv->aligned = std::make_shared<const ImageRGBA>(w.img);
  }

  // Pixels no volume can explain: some other view sharing an axis is empty there.
  std::map<ViewRole, std::array<Profile, 3>> any;
  for (auto& [role, w] : work)
    for (Axis a : {AX, AY, AZ})
      if (has_axis(role, a)) any[role][a] = world_profile(w.mask, role, a);
  for (auto& [role, w] : work) {
    const AxisMap am = axis_map(role);
    int bad = 0;
    for (int v = 0; v < w.mask.height(); ++v)
      for (int u = 0; u < w.mask.width(); ++u) {
        if (!w.mask.get(u, v)) continue;
        const int cu = am.u_rev ? w.mask.width() - 1 - u : u;
        const int cv = am.v_rev ? w.mask.height() - 1 - v : v;
        bool ok = true;
        for (auto& [other, prof] : any) {
          if (other == role) continue;
          if (has_axis(other, am.u) && !prof[am.u][cu]) ok = false;
          if (has_axis(other, am.v) && !prof[am.v][cv]) ok = false;
        }
        bad += ok ? 0 : 1;
      }
    res.profile_conflicts[role] = bad;
  }
  return res;
}

Expected<AlignResult> align_single_view(std::vector<SourceView>& views, const AlignOptions& opt,
                                        const std::function<int(int, int)>& depth_for) {
  if (views.size() != 1 || !views[0].original)
    return make_error("view.single_view", "single view mode takes exactly one drawing");
  SourceView& sv = views[0];
  int g = change_spacing_gcd(source_of(sv));
  const Mask2D::Box b0 = opaque_mask(source_of(sv), opt.alpha_threshold).bbox();
  if (!b0.empty()) g = std::gcd(g, std::gcd(b0.width(), b0.height()));
  sv.scale = scale_with_k(source_of(sv), sv.working ? 1 : scale_from_gcd(g));
  ImageRGBA img = downscale_nearest(source_of(sv), sv.scale);
  const Mask2D full = opaque_mask(img, opt.alpha_threshold);
  if (full.count() == 0)
    return make_error("view.empty", "view has no opaque pixels: " + std::string(to_string(sv.role)),
                      {{"view", to_string(sv.role)}});
  sv.crop = full.bbox();
  img = crop(img, sv.crop);
  sv.flipped_x = false;
  if (const auto f = opt.force_flip.find(sv.role); f != opt.force_flip.end() && f->second) {
    img = flip_x(img);
    sv.flipped_x = sv.flip_user_override = true;
  }
  sv.pad_x = sv.pad_y = 0;
  const int w = img.width(), h = img.height(), depth = std::max(1, depth_for(w, h));
  AlignResult res;
  switch (sv.role) {
    case ViewRole::Front:
    case ViewRole::Back: res.dims = Dims{w, h, depth}; break;
    case ViewRole::Side:
    case ViewRole::Right: res.dims = Dims{depth, h, w}; break;
    case ViewRole::Top:
    case ViewRole::Bottom: res.dims = Dims{w, depth, h}; break;
  }
  if (too_large(res.dims)) return Unexpected{too_large_error(res.dims, views, opt.alpha_threshold)};
  sv.aligned = std::make_shared<const ImageRGBA>(std::move(img));
  res.flip_confidence[sv.role] = 1.0;
  res.profile_conflicts[sv.role] = 0;
  if (sv.scale.k > 1)
    res.messages.push_back(Message{"view_downscaled", "artwork downscaled by " + std::to_string(sv.scale.k),
                                   Severity::Info, {{"view", to_string(sv.role)}, {"k", sv.scale.k}}});
  res.messages.push_back(Message{"single_view", "single view mode: depth and hidden surfaces are inferred",
                                 Severity::Info, {{"view", to_string(sv.role)}, {"depth", depth}}});
  return res;
}

}  // namespace pixel3d
