#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "fixturegen/lowpoly_character.hpp"
#include "lowpoly_helpers.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/lowpoly/build.hpp"
#include "pixel3d/lowpoly/gate.hpp"
#include "pixel3d/lowpoly/layout.hpp"
#include "pixel3d/lowpoly/suggest.hpp"
#include "pixel3d/lowpoly/views.hpp"
#include "pixel3d/image/palette.hpp"

// 1.2.0 P2: fixture F (input B, the hooded 3-view reference) and the defects found in the
// baseline, each reproduced here. A test marked should_fail() reproduces a defect that is
// still open: it passes while the defect exists. P3 fixes the defect and removes the marker
// (should_fail turns a fixed defect into a failing test, so the marker cannot be forgotten).
// Findings: docs/reports/1.2.0/p2/README.md.

using namespace pixel3d;
using namespace p3dtest;

namespace {

const char* kF = "F_hooded_reference";

const lowpoly::TubeSpec* tube_with(const lowpoly::Layout& l, int part) {
  for (const auto& t : l.tubes)
    if (std::find(t.parts.begin(), t.parts.end(), part) != t.parts.end()) return &t;
  return nullptr;
}

// Part id of the front-most surface at a Front pixel centre (-1 = none): the Front camera
// looks along -Z, so the largest z wins.
int front_owner(const lowpoly::SkinnedMesh& m, Dims d, int u, int v) {
  const double x = u + 0.5, y = d.H - (v + 0.5);
  double best = -std::numeric_limits<double>::infinity();
  int owner = -1;
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const vec3d& a = m.positions[m.indices[t]];
    const vec3d& b = m.positions[m.indices[t + 1]];
    const vec3d& c = m.positions[m.indices[t + 2]];
    const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (std::abs(area) < 1e-12) continue;
    const double w1 = ((x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y)) / area;
    const double w2 = ((b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y)) / area;
    const double w0 = 1 - w1 - w2;
    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
    const double z = w0 * a.z + w1 * b.z + w2 * c.z;
    if (z > best) best = z, owner = m.vertex_part[m.indices[t]];
  }
  return owner;
}

// The texel the front-most surface shows at a Front pixel centre (alpha 0 = none).
Rgba8 front_texel(const lowpoly::SkinnedMesh& m, const ImageRGBA& tex, Dims d, int u, int v) {
  const double x = u + 0.5, y = d.H - (v + 0.5);
  double best = -std::numeric_limits<double>::infinity();
  Rgba8 out{0, 0, 0, 0};
  for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
    const uint32_t i0 = m.indices[t], i1 = m.indices[t + 1], i2 = m.indices[t + 2];
    const vec3d& a = m.positions[i0];
    const vec3d& b = m.positions[i1];
    const vec3d& c = m.positions[i2];
    const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    if (std::abs(area) < 1e-12) continue;
    const double w1 = ((x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y)) / area;
    const double w2 = ((b.x - a.x) * (y - a.y) - (x - a.x) * (b.y - a.y)) / area;
    const double w0 = 1 - w1 - w2;
    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
    const double z = w0 * a.z + w1 * b.z + w2 * c.z;
    if (z <= best) continue;
    best = z;
    const vec2d uv = m.uvs[i0] * w0 + m.uvs[i1] * w1 + m.uvs[i2] * w2;
    const int tx = std::clamp(static_cast<int>(uv.x * tex.width()), 0, tex.width() - 1);
    const int ty = std::clamp(static_cast<int>(uv.y * tex.height()), 0, tex.height() - 1);
    out = tex.at(tx, ty);
  }
  return out;
}

}  // namespace

TEST_CASE("fixture F: committed drawings equal the generator output") {
  const p3dfix::CharFixture fx = p3dfix::make_hooded_reference();
  const std::string dir = lowpoly_fixture_dir(kF);
  CHECK(*load_png(dir + "/front.png") == fx.front.art);
  CHECK(*load_png(dir + "/side.png") == fx.side.art);
  CHECK(*load_png(dir + "/top.png") == fx.top.art);
  CHECK(*load_png(dir + "/front_parts.png") == fx.front.parts);
  CHECK(*load_png(dir + "/side_parts.png") == fx.side.parts);
  CHECK(*load_png(dir + "/top_parts.png") == fx.top.parts);
  REQUIRE(fx.extents.size() == fx.parts.size());
  for (const auto& e : fx.extents) CHECK(e.volume > 0);
}

TEST_CASE("fixture F: front + side align to the tight ground-truth box") {
  const lowpoly::Input in = load_lowpoly_fixture(kF);
  const auto meta = lowpoly_fixture_meta(kF);
  CHECK(in.dims == Dims{meta["dims"][0].get<int>(), meta["dims"][1].get<int>(), meta["dims"][2].get<int>()});
}

// P2 finding LP-1 (ENGINE_BUG, layout.cpp limb chains): a chain follows any limb whose Front
// mask touches the current one, so two legs drawn side by side (only an ink line between
// them) become ONE loft: left leg -> right leg -> boots.
TEST_CASE("P2 LP-1: legs drawn side by side stay two chains") {
  const lowpoly::Input in = load_lowpoly_fixture(kF);
  const auto l = lowpoly::build_layout(in);
  REQUIRE(l);
  const lowpoly::TubeSpec* left = tube_with(*l, 1011);   // leg_l
  const lowpoly::TubeSpec* right = tube_with(*l, 1012);  // leg_r
  REQUIRE(left);
  REQUIRE(right);
  CHECK(left != right);
  CHECK(tube_with(*l, 1013) == left);   // boot_l continues the left leg
  CHECK(tube_with(*l, 1014) == right);  // boot_r continues the right leg
}

namespace {

// The corrected part masks of samples/hooded_character (P2 masks row), Front only, 20×50.
lowpoly::Input hooded_masks() {
  const char* rows[] = {
      ".......000000.......", ".....0000000000.....", "....000000000000....", "...00000000000000...",
      "..0000000000000000..", ".000000000000000000.", ".000000000000000000.", ".000008888888800000.",
      ".000008888888800000.", "00000086666668000000", ".000006666666600000.", ".000066666666660000.",
      ".000006666666600000.", ".000006666666600000.", ".000000666666000000.", ".000000066660000000.",
      ".000777777777777000.", ".000777777777777000.", ".000777777777777000.", ".aaa777777777777999.",
      ".aaaa71111117779999.", ".aaaaa1111117779999.", ".aaaaa1111117779999.", ".aaaaa1111117779999.",
      ".aaaaa1111117779999.", ".aaaaa1111117779999.", ".aaaaa1111117779999.", ".aaaaa1111111179999.",
      ".aaaaa1111111119999.", ".aaaabbbbbbbbbb9999.", ".aaaabbbbbbbbbb9999.", ".aaaa11111111119999.",
      ".aaaa11111111119999.", ".aaaa11111111119999.", "..aaa3333112222999..", "....333333222222....",
      "....333333222222....", "....333333222222....", "....333333222222....", "....333333222222....",
      "....333333222222....", "....333333222222....", "....333333222222....", "...33333332222222...",
      "...33333332222222...", "...33333332222222...", "...33333332222222...", "...33333332222222...",
      "...33333332222222...", "....33333..22222....",
  };
  const int W = 20, H = 50;
  struct P { char c; int id; const char* name; lowpoly::PartType type; };
  const P parts[] = {{'0', 1000, "hood", lowpoly::PartType::Cloth},     {'1', 1001, "torso", lowpoly::PartType::Torso},
                     {'2', 1002, "leg_l", lowpoly::PartType::Limb},     {'3', 1003, "leg_r", lowpoly::PartType::Limb},
                     {'6', 1006, "head", lowpoly::PartType::Head},      {'7', 1007, "scarf", lowpoly::PartType::Cloth},
                     {'8', 1008, "hair", lowpoly::PartType::Accessory}, {'9', 1009, "arm_l", lowpoly::PartType::Limb},
                     {'a', 1010, "arm_r", lowpoly::PartType::Limb},     {'b', 1011, "belt", lowpoly::PartType::Accessory}};
  ImageRGBA img(W, H);
  for (int v = 0; v < H; ++v)
    for (int u = 0; u < W; ++u)
      if (rows[v][u] != '.') img.at(u, v) = {120, 80, 50, 255};
  SourceView s;
  s.role = ViewRole::Front;
  s.original = std::make_shared<const ImageRGBA>(img);
  std::vector<SourceView> views{s};
  auto al = lowpoly::align_views(views, AlignOptions{}, 18);
  REQUIRE(al);
  lowpoly::Input in;
  in.views = views;
  in.dims = al->dims;
  for (const P& pp : parts) {
    lowpoly::Part p;
    p.id = pp.id, p.name = pp.name, p.type = pp.type;
    Mask2D m(W, H);
    for (int v = 0; v < H; ++v)
      for (int u = 0; u < W; ++u) m.set(u, v, rows[v][u] == pp.c);
    p.masks.emplace(ViewRole::Front, std::move(m));
    in.parts.push_back(std::move(p));
  }
  return in;
}

}  // namespace

// LP-1 on the corrected masks of samples/hooded_character (P2 masks row, Front, 20×50): arms
// hang against the body and touch the legs at their tips (row 34), the legs touch each other
// along their whole length. P2 built one loft arm_l+leg_l+leg_r+arm_r.
TEST_CASE("P2 LP-1: arms against the body and side-by-side legs are four separate chains") {
  const lowpoly::Input in = hooded_masks();
  REQUIRE(in.views[0].aligned->width() == 20);
  const auto l = lowpoly::build_layout(in);
  REQUIRE(l);
  for (int id : {1002, 1003, 1009, 1010}) {
    const lowpoly::TubeSpec* t = tube_with(*l, id);
    REQUIRE(t);
    CAPTURE(t->name);
    CHECK(t->parts.size() == 1);  // no other limb in its loft
  }
  // The legs hang from the torso (what they touch most), not from an arm.
  for (int id : {1002, 1003}) {
    const lowpoly::TubeSpec* t = tube_with(*l, id);
    REQUIRE(t->parent_tube >= 0);
    const auto& pt = l->tubes[static_cast<size_t>(t->parent_tube)].parts;
    CHECK(std::find(pt.begin(), pt.end(), 1001) != pt.end());
  }
}

// P2 finding U-5 (cause found in P3): the corrected torso is drawn in two pieces (the belt
// crosses it) and its tube followed one piece only, so 23 drawn torso pixels below the belt had
// no surface. A part drawn in pieces is one tube over the gaps between them.
TEST_CASE("P2 U-5: a torso crossed by a belt keeps a surface over all its drawn pixels") {
  lowpoly::Input in = hooded_masks();
  in.settings.accept_inferred_depth = true;
  const auto r = lowpoly::build_lowpoly(in);
  REQUIRE(r);
  const auto it = std::find_if(in.parts.begin(), in.parts.end(), [](const lowpoly::Part& p) { return p.name == "torso"; });
  REQUIRE(it != in.parts.end());
  const auto bt = std::find_if(in.parts.begin(), in.parts.end(), [](const lowpoly::Part& p) { return p.name == "belt"; });
  const Mask2D& m = it->masks.at(ViewRole::Front);
  const Mask2D& bm = bt->masks.at(ViewRole::Front);
  // P2: 23 of 91 torso pixels, a hole below the belt. Interior pixels (torso or belt on all four
  // sides) must all have a surface; on the outline a low-poly ring may miss single pixels.
  int bare_inside = 0, bare_edge = 0;
  for (int v = 0; v < m.height(); ++v)
    for (int u = 0; u < m.width(); ++u) {
      if (!m.get(u, v) || front_owner(r->assembly.mesh, in.dims, u, v) >= 0) continue;
      bool inside = true;
      for (const auto& [du, dv] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
        inside &= m.get_or_false(u + du, v + dv) || bm.get_or_false(u + du, v + dv);
      (inside ? bare_inside : bare_edge)++;
    }
  CHECK(bare_inside == 0);
  CHECK(bare_edge <= 3);
  // ... and the torso bridged over the belt's rows stays behind the belt: the belt shows on
  // its own drawn pixels (1.2.1 comparison: 12 of 20).
  int belt_px = 0, belt_shown = 0;
  for (int v = 0; v < bm.height(); ++v)
    for (int u = 0; u < bm.width(); ++u)
      if (bm.get(u, v)) ++belt_px, belt_shown += front_owner(r->assembly.mesh, in.dims, u, v) == bt->id;
  CAPTURE(belt_px);
  CHECK(belt_shown >= 0.9 * belt_px);
}

// P2 finding LP-2 (ENGINE_BUG, Session contract): a user part cannot be deleted in a Low-Poly
// Studio project; delete_part is a visual-hull edit and answers state.wrong_mode.
TEST_CASE("P2 LP-2: a suggested part can be deleted in a Low-Poly Studio project") {
  Session s;
  ProjectSettings ps;
  ps.name = "p2";
  ps.lowpoly.enabled = true;
  REQUIRE(s.new_project(ps).success);
  const std::string dir = lowpoly_fixture_dir(kF);
  REQUIRE(s.import_views({{ViewRole::Front, dir + "/front.png", {}}, {ViewRole::Side, dir + "/side.png", {}}}).success);
  const OpResult sg = s.lowpoly_suggest_parts({.apply = true});
  REQUIRE(sg.success);
  const int victim = sg.data["part_ids"].back().get<int>();
  const OpResult del = s.delete_part(victim);
  CHECK_MESSAGE(del.success, to_json(del).dump());
  CHECK_FALSE(s.document().masks.parts.contains(victim));
  CHECK_FALSE(s.document().part_types.contains(victim));
  CHECK_FALSE(s.document().part_names.contains(victim));
  CHECK(s.lowpoly_build({}).success);  // the rest still builds
  // Undo brings it back; deleting an unknown part is refused.
  for (int i = 0; i < 3 && !s.document().masks.parts.contains(victim); ++i) REQUIRE(s.undo().success);
  CHECK(s.document().masks.parts.contains(victim));
  CHECK_FALSE(s.delete_part(999999).success);
}

// P2 finding LP-3 (ENGINE_BUG, front-only depth): with Front only, every part is inflated
// about z = D/2 by its own type's roundness, so a part drawn ON another part (a face inside a
// hood, a belt on a tunic) can end up inside the bigger shell: its drawn pixels are covered
// by the other part and its texture never shows (texture_bake only takes a Front pixel for
// the part whose mask holds it, so the covering shell gets edge-extended colour there).
TEST_CASE("P2 LP-3: front only, every drawn part is the front-most surface over its pixels") {
  lowpoly::Input in = load_lowpoly_fixture(kF, false);
  in.settings.accept_inferred_depth = true;
  const auto r = lowpoly::build_lowpoly(in);
  REQUIRE(r);
  const lowpoly::SkinnedMesh& m = r->assembly.mesh;
  for (const lowpoly::Part& p : in.parts) {
    const auto it = p.masks.find(ViewRole::Front);
    if (it == p.masks.end() || it->second.count() < 20) continue;
    int own = 0, total = 0;
    for (int v = 0; v < it->second.height(); ++v)
      for (int u = 0; u < it->second.width(); ++u)
        if (it->second.get(u, v)) ++total, own += front_owner(m, in.dims, u, v) == p.id;
    CAPTURE(p.name);
    CHECK(own >= total / 2);
  }
  // ... and shows its own drawing there: the face is not the scarf's colour. Interior pixels
  // (not on the part's outline, where the contour ink is replaced) must show their drawn colour.
  REQUIRE(r->bake.texture);
  const ImageRGBA& front = *in.views[0].aligned;
  for (const char* name : {"head", "hair"}) {
    const auto it = std::find_if(in.parts.begin(), in.parts.end(), [&](const lowpoly::Part& p) { return p.name == name; });
    REQUIRE(it != in.parts.end());
    const Mask2D& pm = it->masks.at(ViewRole::Front);
    int inner = 0, same = 0;
    for (int v = 1; v + 1 < pm.height(); ++v)
      for (int u = 1; u + 1 < pm.width(); ++u) {
        if (!(pm.get(u, v) && pm.get(u - 1, v) && pm.get(u + 1, v) && pm.get(u, v - 1) && pm.get(u, v + 1))) continue;
        ++inner;
        same += front_texel(m, *r->bake.texture, in.dims, u, v) == front.at(u, v);
      }
    CAPTURE(std::string(name));
    CAPTURE(inner);
    CHECK(same >= 0.8 * inner);
  }
}

// ---- LP-5 / LP-6: part suggestion --------------------------------------------------------
namespace {

// A Front-only drawing from a character map (one char per pixel, '.' transparent), aligned.
lowpoly::Input drawn(const std::vector<std::string>& rows, const std::map<char, Rgba8>& pal) {
  const int H = static_cast<int>(rows.size()), W = static_cast<int>(rows[0].size());
  ImageRGBA img(W, H);
  for (int v = 0; v < H; ++v)
    for (int u = 0; u < W; ++u)
      if (rows[v][u] != '.') img.at(u, v) = pal.at(rows[v][u]);
  SourceView s;
  s.role = ViewRole::Front;
  s.original = std::make_shared<const ImageRGBA>(img);
  std::vector<SourceView> views{s};
  auto al = lowpoly::align_views(views, AlignOptions{}, 16);
  REQUIRE(al);
  lowpoly::Input in;
  in.views = views;
  in.dims = al->dims;
  return in;
}

lowpoly::Suggestion suggest(const lowpoly::Input& in) {
  std::vector<const ImageRGBA*> imgs{in.views[0].aligned.get()};
  return lowpoly::suggest_parts(in.views[0], analyze_palette(imgs), 128);
}

// The suggested part holding most pixels of colour `c` on one side of the image (-1 left,
// +1 right, 0 both); nullptr when none.
const lowpoly::Part* part_of(const lowpoly::Suggestion& s, const ImageRGBA& img, Rgba8 c, int side) {
  std::map<const lowpoly::Part*, int> n;
  for (int v = 0; v < img.height(); ++v)
    for (int u = 0; u < img.width(); ++u) {
      if (!(img.at(u, v) == c) || (side < 0 && 2 * u + 1 > img.width()) || (side > 0 && 2 * u + 1 < img.width())) continue;
      for (const auto& p : s.parts)
        if (p.masks.at(ViewRole::Front).get_or_false(u, v)) ++n[&p];
    }
  const lowpoly::Part* best = nullptr;
  for (const auto& [p, k] : n)
    if (!best || k > n[best]) best = p;
  return best;
}

// samples/hooded_character Front at its native 1 px (26×50): sleeves h (66,42,28) and tunic
// j/k are shades of one brown ramp; an ink line separates the left sleeve from the tunic, the
// scarf tail separates the right one.
const std::vector<std::string> kHooded = {
    "..........aaaaaa..........", "........aabbbbbbaa........", ".......abbccccccbba.......",
    "......abccccccccccba......", ".....abccccccccccccba.....", "....abbccccccccccccbba....",
    "....abbbbbbbbbbbbbbbba....", "....abbbbddddddddbbbba....", "....abbbbddddddddbbbba....",
    "...abbbbbdeeeeeedbbbbba...", "....abbbbeeeeeeeebbbba....", "....abbbeefeeeefeebbba....",
    "....abbbbefeeeefebbbba....", "....abbbbeeeeeeeebbbba....", "....abbbbbeeeeeebbbbba....",
    "....abbbbbbeeeebbbbbba....", "....abbggggggggggggbba....", "....abbggggggggggggbba....",
    "....abbggggggggggggbba....", "....ahhiiiiiiiiiiiihha....", "....ahhhajjjjjjggghhha....",
    "....ahhhajjjkkjggghhha....", "....ahhhajjjkkjggghhha....", "....ahhhajjjkkjggghhha....",
    "....ahhhajjjkkjggghhha....", "....ahhhajjjkkjggghhha....", "....ahhhajjjkkjiiihhha....",
    "....ahhhajjjkkjjjahhha....", "....ahhhajjjkkjjjahhha....", "....ahhhllllmmllllhhha....",
    "....ahhhllllmmllllhhha....", "....ahhhkkkkkkkkkkhhha....", "....aeeekkkkkkkkkkeeea....",
    "....aeeekkkkkkkkkkeeea....", ".....aaannnnaannnnaaa.....", ".......annnnaannnna.......",
    ".......annnnaannnna.......", ".......annnnaannnna.......", ".......annnnaannnna.......",
    ".......annnnaannnna.......", ".......annnnaannnna.......", ".......annnnaannnna.......",
    ".......annnnaannnna.......", "......aoooooaaoooooa......", "......apppppaapppppa......",
    "......apppppaapppppa......", "......apppppaapppppa......", "......apppppaapppppa......",
    "......apppppaapppppa......", ".......aaaaa..aaaaa.......",
};
const std::map<char, Rgba8> kHoodedPal = {
    {'a', {30, 26, 36, 255}},    {'b', {74, 66, 80, 255}},  {'c', {98, 88, 112, 255}}, {'d', {122, 74, 42, 255}},
    {'e', {242, 201, 160, 255}}, {'f', {42, 42, 58, 255}},  {'g', {47, 95, 208, 255}}, {'h', {66, 42, 28, 255}},
    {'i', {31, 63, 153, 255}},   {'j', {138, 90, 52, 255}}, {'k', {106, 66, 36, 255}}, {'l', {58, 42, 26, 255}},
    {'m', {216, 176, 64, 255}},  {'n', {90, 90, 102, 255}}, {'o', {106, 68, 40, 255}}, {'p', {74, 46, 26, 255}},
};

}  // namespace

TEST_CASE("P2 LP-5: arms drawn against the body (one colour ramp, ink between) are suggested as limbs") {
  const lowpoly::Input in = drawn(kHooded, kHoodedPal);
  const lowpoly::Suggestion s = suggest(in);
  const ImageRGBA& img = *in.views[0].aligned;
  const lowpoly::Part* torso = part_of(s, img, kHoodedPal.at('j'), 0);
  const lowpoly::Part* arm_r = part_of(s, img, kHoodedPal.at('h'), -1);  // image left = the character's right
  const lowpoly::Part* arm_l = part_of(s, img, kHoodedPal.at('h'), +1);
  REQUIRE(torso);
  REQUIRE(arm_r);
  REQUIRE(arm_l);
  CHECK(arm_r != torso);
  CHECK(arm_l != torso);
  CHECK(arm_r != arm_l);
  CHECK(arm_r->type == lowpoly::PartType::Limb);
  CHECK(arm_l->type == lowpoly::PartType::Limb);
  CHECK(torso->type != lowpoly::PartType::Limb);
}

TEST_CASE("P2 LP-5: a belt and a pouch drawn on the body are never suggested as limbs") {
  const lowpoly::Input in = load_lowpoly_fixture(kF, false);
  std::vector<const ImageRGBA*> imgs{in.views[0].aligned.get()};
  const lowpoly::Suggestion s = lowpoly::suggest_parts(in.views[0], analyze_palette(imgs), 128);
  for (const lowpoly::Part& gt : in.parts) {
    if (gt.name != "belt" && gt.name != "pouch") continue;
    const Mask2D& g = gt.masks.at(ViewRole::Front);
    for (const auto& p : s.parts) {
      const Mask2D& m = p.masks.at(ViewRole::Front);
      int inside = 0;
      for (int v = 0; v < m.height(); ++v)
        for (int u = 0; u < m.width(); ++u) inside += m.get(u, v) && g.get_or_false(u, v);
      CAPTURE(gt.name);
      CAPTURE(p.name);
      if (inside > 0.5 * m.count()) CHECK(p.type != lowpoly::PartType::Limb);  // a part made mostly of belt/pouch
    }
  }
}

TEST_CASE("P2 LP-6: a barrel painted in bands is one part; a crate is a body, not a head") {
  const std::vector<std::string> barrel = {
      "...aaaaaaaaaa...", "..abbbcbbcbbba..", ".abbbcbbbcbbba..", ".adddddddddddda.", "aeeeeeeeeeeeeea.",
      "abbbbcbbbcbbbba.", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba",
      "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbcbbbba.",
      "addddddddddddda.", ".aeeeeeeeeeeeea.", ".abbbcbbbcbbba..", "..abbbcbbcbbba..", "...aaaaaaaaaa...",
  };
  const std::map<char, Rgba8> bpal = {{'a', {30, 26, 36, 255}},    {'b', {150, 104, 60, 255}}, {'c', {110, 72, 38, 255}},
                                      {'d', {100, 100, 112, 255}}, {'e', {140, 140, 150, 255}}};
  const lowpoly::Suggestion sb = suggest(drawn(barrel, bpal));
  REQUIRE(sb.parts.size() == 1);
  CHECK(sb.parts[0].type != lowpoly::PartType::Limb);
  CHECK(sb.parts[0].type != lowpoly::PartType::Head);

  const std::vector<std::string> crate = {
      ".aaaaaaaaaaaaaaaa.", "abbbbbbbbbbbbbbbba", "abbbbbbbbbbbbbbbba", "abbccddddddddddbba", "abbdccdddddddddbba",
      "abbeecceeeeeeeebba", "abbdddccdddddddbba", "abbddddccddddddbba", "abbdddddccdddddbba", "abbeeeeeecceeeebba",
      "abbdddddddccdddbba", "abbddddddddccddbba", "abbdddddddddccdbba", "abbeeeeeeeeeeccbba", "abbdddddddddddccba",
      "abbbbbbbbbbbbbbbba", "abbbbbbbbbbbbbbbba", ".aaaaaaaaaaaaaaaa.",
  };
  const std::map<char, Rgba8> cpal = {{'a', {30, 26, 36, 255}},    {'b', {80, 52, 28, 255}}, {'c', {168, 118, 70, 255}},
                                      {'d', {150, 104, 60, 255}}, {'e', {110, 72, 38, 255}}};
  const lowpoly::Suggestion sc = suggest(drawn(crate, cpal));
  REQUIRE(sc.parts.size() == 1);
  CHECK(sc.parts[0].type != lowpoly::PartType::Head);
}

// P3 finding N-1 (ENGINE_BUG): once the face is suggested as its own part, its branch got the
// "extension into the parent" along its own (horizontal) spine and left the hood sideways: a box
// beside the head. Built from the suggestion, nothing may stand outside the Front drawing.
TEST_CASE("P3 N-1: a part drawn inside another one does not stick out of the drawing") {
  lowpoly::Input in = drawn(kHooded, kHoodedPal);
  in.parts = suggest(in).parts;
  in.settings.accept_inferred_depth = true;
  const auto r = lowpoly::build_lowpoly(in);
  REQUIRE(r);
  const ImageRGBA& img = *in.views[0].aligned;
  int outside = 0, inside = 0;
  for (int v = -8; v < img.height() + 8; ++v)  // beyond the image too: the head is the widest part
    for (int u = -8; u < img.width() + 8; ++u) {
      if (img.in_bounds(u, v) && img.at(u, v).a >= 128) {
        ++inside;
        continue;
      }
      outside += front_owner(r->assembly.mesh, in.dims, u, v) >= 0;
    }
  CAPTURE(inside);
  CHECK(outside <= inside / 100);
}

// P3 finding N-3 (ENGINE_BUG): a suggested part's Side mask was taken from its dominant colour
// family only, so a one-part prop (a barrel: wood + metal hoops + ink outline) lost its hoops and
// outline in Side and failed the side silhouette check. One part = the whole Side drawing.
TEST_CASE("P3 N-3: a one-part prop's Side mask is the whole Side drawing") {
  const std::vector<std::string> barrel = {
      "...aaaaaaaaaa...", "..abbbcbbcbbba..", ".abbbcbbbcbbba..", ".adddddddddddda.", "aeeeeeeeeeeeeea.",
      "abbbbcbbbcbbbba.", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba",
      "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbbcbbbba", "abbbbcbbbcbbbba.",
      "addddddddddddda.", ".aeeeeeeeeeeeea.", ".abbbcbbbcbbba..", "..abbbcbbcbbba..", "...aaaaaaaaaaa...",
  };
  const std::map<char, Rgba8> pal = {{'a', {30, 26, 36, 255}},    {'b', {150, 104, 60, 255}}, {'c', {110, 72, 38, 255}},
                                     {'d', {100, 100, 112, 255}}, {'e', {140, 140, 150, 255}}};
  const int H = static_cast<int>(barrel.size()), W = 16;
  ImageRGBA img(W, H);
  for (int v = 0; v < H; ++v)
    for (int u = 0; u < W; ++u)
      if (barrel[v][u] != '.') img.at(u, v) = pal.at(barrel[v][u]);
  std::vector<SourceView> views(2);
  views[0].role = ViewRole::Front, views[1].role = ViewRole::Side;
  views[0].original = views[1].original = std::make_shared<const ImageRGBA>(img);
  REQUIRE(lowpoly::align_views(views, AlignOptions{}, 0));
  std::vector<const ImageRGBA*> imgs{views[0].aligned.get(), views[1].aligned.get()};
  const lowpoly::Suggestion s = lowpoly::suggest_parts(views[0], analyze_palette(imgs), 128, nullptr, &views[1]);
  REQUIRE(s.parts.size() == 1);
  REQUIRE(s.parts[0].masks.contains(ViewRole::Side));
  const Mask2D side_subject = opaque_mask(*views[1].aligned, 128);
  CHECK(mask_iou(s.parts[0].masks.at(ViewRole::Side), side_subject) >= 0.99);
}

// P3 finding N-4 (ENGINE_BUG, from the 1.2.1 comparison): after U-5 a torso crossed by a belt is
// one tube over the belt's rows; with a Side drawing its depth there is inferred and it came out
// in front of the belt (belt visible on 12 of 20 px). A part bridged over another part's pixels
// stays behind that part.
TEST_CASE("P3 N-4: with Front + Side, every part drawn on the torso stays visible") {
  const lowpoly::Input in = load_lowpoly_fixture(kF);
  const auto r = lowpoly::build_lowpoly(in);
  REQUIRE(r);
  for (const lowpoly::Part& p : in.parts) {
    if (p.name != "belt" && p.name != "pouch") continue;
    const Mask2D& m = p.masks.at(ViewRole::Front);
    int px = 0, shown = 0;
    for (int v = 0; v < m.height(); ++v)
      for (int u = 0; u < m.width(); ++u)
        if (m.get(u, v)) ++px, shown += front_owner(r->assembly.mesh, in.dims, u, v) == p.id;
    CAPTURE(p.name);
    CAPTURE(px);
    // The report's declared per-part rule (coverage >= 0.75). Before this fix the belt showed on
    // 50 of 72 px; the round belt band's top and bottom rows against the torso's own rows stay
    // hidden as in 1.2.0 (57 of 72).
    CHECK(shown >= 0.75 * px);
  }
  // The torso keeps its back: the Side silhouette is unchanged (1.2.2 comparison: side IoU
  // 0.966 -> 0.882 when the bridged torso was thinned on both sides).
  const auto iou = lowpoly::silhouette_iou(in, *r);
  REQUIRE(iou.contains(ViewRole::Side));
  CHECK(iou.at(ViewRole::Side) >= lowpoly::kSilhouetteIouMin);
}
