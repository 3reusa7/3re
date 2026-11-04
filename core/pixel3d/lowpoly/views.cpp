#include "pixel3d/lowpoly/views.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "pixel3d/common/limits.hpp"

namespace pixel3d::lowpoly {
namespace {

SourceView* find(std::vector<SourceView>& views, ViewRole r) {
  for (auto& v : views)
    if (v.role == r) return &v;
  return nullptr;
}

// Pads the aligned image of `v` to w × h with transparent pixels (left/top by `left`, `top`).
void pad(SourceView& v, int w, int h, int left, int top) {
  ImageRGBA out(w, h);
  for (int y = 0; y < v.aligned->height(); ++y)
    for (int x = 0; x < v.aligned->width(); ++x) out.at(x + left, y + top) = v.aligned->at(x, y);
  v.pad_x += left;
  v.pad_y += top;
  v.aligned = std::make_shared<const ImageRGBA>(std::move(out));
}

}  // namespace

Expected<AlignResult> align_views(std::vector<SourceView>& views, const AlignOptions& opt, double front_only_depth) {
  SourceView* front = find(views, ViewRole::Front);
  SourceView* side = find(views, ViewRole::Side);
  SourceView* top = find(views, ViewRole::Top);
  if (!front) return make_error("lowpoly.no_front", "low-poly studio needs a Front drawing");
  if (side && top) return auto_align(views, opt);
  for (const auto& v : views)
    if (v.role != ViewRole::Front && v.role != ViewRole::Side && v.role != ViewRole::Top)
      return make_error("lowpoly.view_unsupported",
                        "low-poly studio takes Front with optional Side and Top; got " + std::string(to_string(v.role)),
                        {{"view", to_string(v.role)}});

  AlignResult res;
  // Each view alone: scale and crop exactly as single view mode does.
  for (SourceView* v : {front, side, top}) {
    if (!v) continue;
    std::vector<SourceView> one{*v};
    auto r = align_single_view(one, opt, [](int, int) { return 1; });
    if (!r) return Unexpected{r.error()};
    *v = std::move(one[0]);
    res.flip_confidence[v->role] = 0;  // not detected: the drawing's own orientation is used
    res.profile_conflicts[v->role] = 0;
    for (const Message& m : r->messages)
      if (m.code != "single_view") res.messages.push_back(m);
  }
  const int W = front->aligned->width(), H = front->aligned->height();
  // Front only without a depth: a box as deep as the drawing is wide (every part's own
  // depth is inferred inside it from its type).
  int D = front_only_depth > 0 ? std::max(1, static_cast<int>(std::lround(front_only_depth))) : W;
  auto reconcile = [&](SourceView& v, int have, int want, bool vertical) -> std::optional<Message> {
    if (have == want) return std::nullopt;
    if (std::abs(have - want) > opt.extent_tolerance || have > want)
      return make_error("extent_mismatch",
                        std::string(to_string(v.role)) + " is " + std::to_string(have) + " px where Front is " +
                            std::to_string(want) + " px",
                        {{"view", to_string(v.role)}, {"have", have}, {"want", want}})
          .error;
    // Shorter by ≤ tolerance: pad on the top (feet stay on the ground) or the right.
    if (vertical) pad(v, v.aligned->width(), want, 0, want - have);
    else pad(v, want, v.aligned->height(), 0, 0);
    res.messages.push_back(Message{"extent_padded", std::string(to_string(v.role)) + " padded by " +
                                                        std::to_string(want - have) + " px",
                                   Severity::Info, {{"view", to_string(v.role)}, {"px", want - have}}});
    return std::nullopt;
  };
  if (side) {
    if (auto e = reconcile(*side, side->aligned->height(), H, true)) return Unexpected{*e};
    D = side->aligned->width();
  }
  if (top) {
    if (auto e = reconcile(*top, top->aligned->width(), W, false)) return Unexpected{*e};
    D = top->aligned->height();
  }
  res.dims = Dims{W, H, D};
  if (W > limits::kMaxModelSide || H > limits::kMaxModelSide || D > limits::kMaxModelSide)
    return make_error("view.too_large", "the model would exceed the size limit", {{"dims", {W, H, D}}});
  if (!side && !top)
    res.messages.push_back(Message{"single_view", "front only: depth and hidden surfaces are inferred", Severity::Info,
                                   {{"view", "front"}, {"depth", D}}});
  return res;
}

}  // namespace pixel3d::lowpoly
