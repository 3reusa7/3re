#include <doctest/doctest.h>

#include <algorithm>
#include <map>

#include "lowpoly_helpers.hpp"
#include "pixel3d/lowpoly/build.hpp"
#include "pixel3d/lowpoly/suggest.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;
using namespace p3dtest;

namespace {

MaterialFamilies families_of(const Input& in) {
  std::vector<const ImageRGBA*> imgs;
  for (const auto& v : in.views) imgs.push_back(v.aligned.get());
  return analyze_palette(imgs);
}

const SourceView& front(const Input& in) {
  for (const auto& v : in.views)
    if (v.role == ViewRole::Front) return v;
  throw std::runtime_error("no front");
}

// Best IoU of `gt` with any suggested part, and that part's type.
std::pair<double, PartType> best_match(const Suggestion& s, const Mask2D& gt) {
  double best = 0;
  PartType type = PartType::Accessory;
  for (const auto& p : s.parts) {
    const double iou = mask_iou(p.masks.at(ViewRole::Front), gt);
    if (iou > best) best = iou, type = p.type;
  }
  return {best, type};
}

// The drawing reduced to one flat colour: no ink lines, no colour regions — shape only.
ImageRGBA flat_silhouette(const ImageRGBA& img) {
  ImageRGBA out(img.width(), img.height());
  for (int y = 0; y < img.height(); ++y)
    for (int x = 0; x < img.width(); ++x)
      if (img.at(x, y).a >= 128) out.at(x, y) = {90, 110, 160, 255};
  return out;
}

}  // namespace

TEST_CASE("lowpoly suggest: the cloaked drawing splits into head, cloak, two legs and two horns") {
  const Input in = load_lowpoly_fixture("cloaked", false, 0);
  const Suggestion s = suggest_parts(front(in), families_of(in), 128);
  CHECK(s.parts.size() == 6);
  CHECK(s.templ == Template::CloakedCharacter);
  for (const auto& gt : in.parts) {
    CAPTURE(gt.name);
    const auto [iou, type] = best_match(s, gt.masks.at(ViewRole::Front));
    CHECK(iou >= 0.75);
    CHECK(type == gt.type);
  }
  // Masks cover every opaque pixel, once.
  const Mask2D opaque = opaque_mask(*front(in).aligned);
  int covered = 0, twice = 0;
  for (int y = 0; y < opaque.height(); ++y)
    for (int x = 0; x < opaque.width(); ++x) {
      int n = 0;
      for (const auto& p : s.parts) n += p.masks.at(ViewRole::Front).get(x, y);
      covered += n > 0;
      twice += n > 1;
    }
  CHECK(covered == opaque.count());
  CHECK(twice == 0);
}

TEST_CASE("lowpoly suggest: the humanoid drawing gives a head, a torso and limb chains that fit the humanoid template") {
  const Input in = load_lowpoly_fixture("humanoid", false, 0);
  const Suggestion s = suggest_parts(front(in), families_of(in), 128);
  int heads = 0, bodies = 0, limbs = 0;
  for (const auto& p : s.parts) {
    heads += p.type == PartType::Head;
    bodies += p.type == PartType::Torso || p.type == PartType::Cloth;
    limbs += p.type == PartType::Limb;
  }
  CHECK(heads == 1);
  CHECK(bodies == 1);
  CHECK(limbs >= 4);
  CHECK(s.templ == Template::Humanoid);
  for (const auto& gt : in.parts)
    if (gt.type != PartType::Limb) {
      CAPTURE(gt.name);
      CHECK(best_match(s, gt.masks.at(ViewRole::Front)).first >= 0.75);
    }
}

TEST_CASE("lowpoly suggest: a flat silhouette without ink or colours still splits by shape") {
  Input in = load_lowpoly_fixture("cloaked", false, 0);
  SourceView v = front(in);
  v.aligned = std::make_shared<const ImageRGBA>(flat_silhouette(*v.aligned));
  const std::vector<const ImageRGBA*> imgs{v.aligned.get()};
  const Suggestion s = suggest_parts(v, analyze_palette(imgs), 128);
  std::map<PartType, int> n;
  for (const auto& p : s.parts) ++n[p.type];
  CHECK(n[PartType::Head] == 1);
  CHECK(n[PartType::Cloth] + n[PartType::Torso] == 1);
  CHECK(n[PartType::Limb] == 2);       // legs
  CHECK(n[PartType::Accessory] == 2);  // horns
  for (const auto& gt : in.parts) {
    CAPTURE(gt.name);
    CHECK(best_match(s, gt.masks.at(ViewRole::Front)).first >= 0.6);
  }
}

TEST_CASE("lowpoly suggest: suggested parts build a low-poly character without any painted mask") {
  Input in = load_lowpoly_fixture("cloaked", false, 0);
  const Suggestion s = suggest_parts(front(in), families_of(in), 128);
  in.parts = s.parts;
  in.settings.tri_budget = 1000;
  const auto r = build_lowpoly(in);
  REQUIRE(r);
  CHECK(r->layout.templ == Template::CloakedCharacter);
  CHECK(r->triangles <= 1000);
  CHECK(std::any_of(s.messages.begin(), s.messages.end(), [](const Message& m) { return m.code == "lowpoly.suggestion"; }));
}
