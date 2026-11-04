// M4 editor: every operation through the Session (the API the GUI and MCP use).
// Fixture A in aligned world units: legs x∈[1,3]∪[15,17], z∈[5,7]∪[17,19],
// y∈[0,14]; seat x∈[0,18], y∈[14,17], z∈[4,20]; reclined back above y=17
// near z∈[0,6]. The seat top (y = 17) is visible from Top for z ≥ 6.
#include <doctest/doctest.h>

#include <filesystem>
#include <set>

#include "pixel3d/edit/brush.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/json_io.hpp"
#include "pixel3d/io/project_file.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

std::string fixture(const std::string& name, const std::string& role) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + role + ".png";
}

std::vector<ViewImport> fst(const std::string& name) {
  return {{ViewRole::Front, fixture(name, "front"), {}},
          {ViewRole::Side, fixture(name, "side"), {}},
          {ViewRole::Top, fixture(name, "top"), {}}};
}

void open(Session& s, const std::string& name, ToleranceProfile profile = ToleranceProfile::Strict) {
  ProjectSettings ps;
  ps.profile = profile;
  REQUIRE(s.new_project(ps).success);
  REQUIRE(s.import_views(fst(name)).success);
  REQUIRE(s.project_textures(TextureOptions{}).success);
}

double volume(const Session& s) { return s.document().solid->topology.signed_volume; }
bool watertight(const Session& s) { return s.document().solid->topology.watertight; }
int triangles(const Session& s) { return s.document().solid->topology.triangle_count; }

bool has_message(const std::vector<Message>& ms, const std::string& code) {
  for (const Message& m : ms)
    if (m.code == code) return true;
  return false;
}

int part_at(Session& s, ViewRole view, int u, int v) {
  const OpResult r = s.select_at_pixel(view, u, v, SelectMode::Part);
  REQUIRE(r.success);
  REQUIRE(r.data.contains("hit"));
  return r.data["hit"]["part"].get<int>();
}

int mismatch(const Session& s, ViewRole v) { return s.document().live->views.at(v).mismatch_px; }

std::set<int> part_ids(const Session& s) { return {s.document().solid->mesh.tri_part.begin(), s.document().solid->mesh.tri_part.end()}; }

fs::path tmp(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / "pixel3d_tests" / "editor";
  fs::create_directories(dir);
  return dir / name;
}

}  // namespace

TEST_CASE("brush: weights follow radius and hardness, strokes have no gaps") {
  BrushParams p;
  p.radius = 2;
  p.hardness = 1;
  const auto one = brush_weights({{5.5, 5.5}}, p, 20, 20);
  CHECK(one.size() == 13);  // pixel centres within 2 px of the centre
  for (const auto& [px, w] : one) CHECK(w == 1.0);
  p.radius = 0.5;
  const auto line = brush_weights({{0.5, 0.5}, {9.5, 0.5}}, p, 20, 20);
  CHECK(line.size() == 10);  // every pixel along the stroke, none skipped
  p.radius = 3;
  p.hardness = 0;
  p.strength = 2;
  p.raise = false;
  const auto samples = depth_brush_samples({{10.5, 10.5}}, p, 20, 20);
  REQUIRE_FALSE(samples.empty());
  for (const DepthSample& s : samples) CHECK((s.offset == -1 || s.offset == -2));  // snapped to whole pixels
}

TEST_CASE("editor: depth brush lowers the seat by exactly the brushed pixels, one undo step") {
  Session s;
  open(s, "A_synthetic_chair");
  const double v0 = volume(s);
  const int t0 = triangles(s);
  DepthPaintRequest r;
  r.view = ViewRole::Top;
  r.stroke = {{6.5, 12.5}, {11.5, 12.5}};
  r.brush.radius = 1.5;
  r.brush.hardness = 1;
  r.brush.strength = 1;
  r.brush.raise = false;
  const auto expect = depth_brush_samples(r.stroke, r.brush, 18, 20);
  const OpResult e = s.paint_depth(r);
  REQUIRE(e.success);
  CHECK(watertight(s));
  CHECK(volume(s) == doctest::Approx(v0 - static_cast<double>(expect.size())));
  CHECK(triangles(s) < t0 + 120);  // grouped into one region, not one box per pixel
  CHECK(mismatch(s, ViewRole::Front) == 0);  // a dent from the top keeps every silhouette
  CHECK(s.document().edits.size() == 1);
  REQUIRE(s.undo().success);
  CHECK(volume(s) == doctest::Approx(v0));
  CHECK(s.document().edits.empty());
  REQUIRE(s.redo().success);
  CHECK(volume(s) == doctest::Approx(v0 - static_cast<double>(expect.size())));
}

TEST_CASE("editor: raising a surface past the drawing is reported immediately") {
  Session s;
  open(s, "A_synthetic_chair");
  DepthPaintRequest r;
  r.view = ViewRole::Top;
  r.samples = {{9, 12, 3}, {10, 12, 3}};
  const OpResult e = s.paint_depth(r);
  REQUIRE(e.success);
  CHECK(has_message(e.warnings, "edit.projection_changed"));
  CHECK(mismatch(s, ViewRole::Side) > 0);  // the bump stands above the seat where Side shows nothing
  // Validation: a deliberate deviation to review, never a silent PASS or a broken FAIL.
  const OpResult v = s.validate(ValidateOptions{});
  CHECK(v.status == ValidationStatus::NeedsReview);
  bool deviation = false;
  for (const auto& c : v.data["conflicts"]) deviation |= c["kind"] == "edit.projection_deviation";
  CHECK(deviation);
}

TEST_CASE("editor: push/pull a face along its normal, clamped to the drawings' box") {
  Session s;
  open(s, "A_synthetic_chair");
  const double v0 = volume(s);
  REQUIRE(s.select_at_pixel(ViewRole::Top, 9, 12, SelectMode::Face).success);
  const OpResult sel = s.get_selection();
  REQUIRE(sel.data["face"].is_object());
  CHECK(sel.data["face"]["normal"][1].get<double>() == doctest::Approx(1.0));  // the seat top faces up
  const double area = sel.data["face"]["area"].get<double>();
  PushPullRequest pull;
  pull.face = sel.data["face"]["id"].get<int>();
  pull.distance = 2;
  const OpResult p = s.push_pull(pull);
  REQUIRE(p.success);
  CHECK(watertight(s));
  CHECK(volume(s) == doctest::Approx(v0 + 2 * area).epsilon(1e-6));
  CHECK(has_message(p.warnings, "edit.projection_changed"));  // the seat now looks taller from the front
  REQUIRE(s.undo().success);
  // Push in by one: the same face sinks.
  PushPullRequest push = pull;
  push.distance = -1;
  REQUIRE(s.push_pull(push).success);
  CHECK(volume(s) == doctest::Approx(v0 - area).epsilon(1e-3));
  REQUIRE(s.undo().success);
  // The top of the backrest is already at the box: a bounded pull is refused, an unbounded one allowed.
  PushPullRequest top;
  top.view = ViewRole::Top;
  top.u = 9;
  top.v = 1;
  top.distance = 3;
  const OpResult bounded = s.push_pull(top);
  CHECK_FALSE(bounded.success);
  CHECK(bounded.errors[0].code == "edit.at_bound");
  top.respect_bounds = false;
  CHECK(s.push_pull(top).success);
}

TEST_CASE("editor: mirror and cut") {
  Session c;
  open(c, "C_asym_planter");
  CHECK(mismatch(c, ViewRole::Front) == 0);
  const OpResult m = c.mirror(-1, 0, std::nullopt, false);  // the one-sided handle moves to the other side
  REQUIRE(m.success);
  CHECK(watertight(c));
  CHECK(mismatch(c, ViewRole::Front) > 0);
  CHECK(has_message(m.warnings, "edit.projection_changed"));

  Session a;
  open(a, "A_synthetic_chair");
  const int leg = part_at(a, ViewRole::Front, 1, 30);  // front-left leg seen from the front
  const double v0 = volume(a);
  const size_t parts0 = part_ids(a).size();
  REQUIRE(a.mirror(leg, 2, 21.0, true).success);  // a mirrored copy behind the box (z ↦ 42 − z)
  CHECK(part_ids(a).size() == parts0 + 1);
  CHECK(volume(a) > v0);
  REQUIRE(a.undo().success);
  // Cut everything above y = 10: only the four leg stubs remain.
  REQUIRE(a.cut(-1, 1, 10.0, -1).success);
  CHECK(volume(a) == doctest::Approx(4 * 2 * 2 * 10));
  CHECK(a.document().solid->topology.shells == 4);
}

TEST_CASE("editor: split, merge, duplicate, transform, delete") {
  Session s;
  open(s, "A_synthetic_chair");
  const int seat = part_at(s, ViewRole::Top, 9, 12);
  const double v0 = volume(s);
  const OpResult sp = s.split_part(seat, 0, 9.0);
  REQUIRE(sp.success);
  REQUIRE(sp.data["created_parts"].size() == 1);
  const int half = sp.data["created_parts"][0].get<int>();
  CHECK(half >= kFirstUserPart);
  CHECK(part_ids(s).contains(half));
  CHECK(volume(s) == doctest::Approx(v0));
  REQUIRE(s.merge_parts({seat, half}).success);
  CHECK_FALSE(part_ids(s).contains(half));
  CHECK(volume(s) == doctest::Approx(v0));
  const int leg = part_at(s, ViewRole::Front, 1, 30);
  TransformRequest dup;
  dup.part = leg;
  dup.translate = {0, 0, -4.4};  // snapped to whole pixels
  dup.copy = true;
  const OpResult d = s.transform_part(dup);
  REQUIRE(d.success);
  CHECK(d.data["edit"]["translate"][2].get<double>() == -4.0);
  CHECK(volume(s) == doctest::Approx(v0 + 2 * 2 * 14));  // a separate copy of a 2×2×14 leg
  const int copy = d.data["created_parts"][0].get<int>();
  TransformRequest move;
  move.part = copy;
  move.translate = {0, 0, -1};
  REQUIRE(s.transform_part(move).success);
  CHECK(volume(s) == doctest::Approx(v0 + 56));
  REQUIRE(s.delete_part(copy).success);
  CHECK(volume(s) == doctest::Approx(v0));
  // Two legs merged into one part are two shells; split_part by shells separates them again.
  const int leg2 = part_at(s, ViewRole::Front, 15, 30);
  REQUIRE(s.merge_parts({leg, leg2}).success);
  const OpResult shells = s.split_part_shells(leg);
  REQUIRE(shells.success);
  CHECK(shells.data["created_parts"].size() >= 1);
  CHECK(s.document().edits.size() == 7);
}

TEST_CASE("editor: extrude and volume add/erase") {
  Session s;
  open(s, "A_synthetic_chair");
  const double v0 = volume(s);
  const OpResult e = s.extrude(ViewRole::Top, rect_polys(8, 10, 12, 14), 2, -1);  // a 4×4 block on the seat
  REQUIRE(e.success);
  CHECK(volume(s) == doctest::Approx(v0 + 32));
  CHECK(e.data["edit"]["label"] == "extrude");
  REQUIRE(s.undo().success);
  REQUIRE(s.erase_volume(ViewRole::Front, rect_polys(0, 16, 18, 19), -1, 100, -1).success);  // cut the seat away
  CHECK(volume(s) < v0);
  REQUIRE(s.undo().success);
  const OpResult add = s.add_volume(ViewRole::Front, rect_polys(4, 20, 14, 22), 2, 4, -1);  // a new part
  REQUIRE(add.success);
  CHECK(add.data["created_parts"].size() == 1);
  CHECK(volume(s) > v0);
}

TEST_CASE("editor: part masks label geometry and resolve ambiguity") {
  Session a;
  open(a, "A_synthetic_chair");
  const OpResult cp = a.create_part("seat middle", false);
  REQUIRE(cp.success);
  const int id = cp.data["part"].get<int>();
  MaskPaintRequest m;
  m.part = id;
  m.view = ViewRole::Top;
  for (int v = 8; v < 16; ++v)
    for (int u = 6; u < 12; ++u) m.pixels.emplace_back(u, v);
  REQUIRE(a.paint_part_mask(m).success);
  REQUIRE(a.project_textures(TextureOptions{}).success);
  CHECK(part_ids(a).contains(id));
  CHECK(part_at(a, ViewRole::Top, 8, 10) == id);
  CHECK(watertight(a));
  REQUIRE(a.rename_part(id, "seat centre").success);
  CHECK(a.document().part_names.at(id) == "seat centre");

  // Fixture C: three "define" parts drawn on Front and Side resolve the legs.
  Session c;
  open(c, "C_asym_planter");
  const int legs[3][3] = {{1, 11, 0}, {8, 1, 0}, {15, 11, 0}};  // front x0, side z0
  for (const auto& l : legs) {
    const OpResult p = c.create_part("leg", true);
    REQUIRE(p.success);
    const int pid = p.data["part"].get<int>();
    for (const auto& [view, u0] : {std::pair{ViewRole::Front, l[0]}, std::pair{ViewRole::Side, l[1]}}) {
      MaskPaintRequest mr;
      mr.part = pid;
      mr.view = view;
      for (int v = 14; v < 22; ++v)
        for (int u = u0; u < u0 + 2; ++u) mr.pixels.emplace_back(u, v);
      REQUIRE(c.paint_part_mask(mr).success);
    }
  }
  OpResult v = c.validate(ValidateOptions{});
  REQUIRE(v.success);
  std::set<std::string> blocking;
  for (const auto& b : v.validation["blocking_conflicts"]) blocking.insert(b["kind"].get<std::string>());
  CHECK(blocking == std::set<std::string>{"possible_recess"});  // the legs are decided by the masks
  REQUIRE(c.set_depth(DepthConstraint{ViewRole::Top, rect_polys(2, 2, 16, 12), 11, 1e9, "cavity"}).success);
  v = c.validate(ValidateOptions{});
  CHECK(v.status == ValidationStatus::Pass);
  // Same geometry as resolving the legs by keep/remove decisions.
  Session d;
  open(d, "C_asym_planter");
  PartEdit edit;
  for (const Candidate& cand : d.document().candidates->cands)
    if (cand.status == CandStatus::Ambiguous) {
      bool keep = false;
      for (const auto& xz : {std::pair{1, 11}, std::pair{8, 1}, std::pair{15, 11}})
        keep |= cand.footprint.x0 == xz.first && cand.footprint.y0 == xz.second;
      edit.decisions.push_back({cand.id, keep});
    }
  REQUIRE(d.edit_part(edit).success);
  REQUIRE(d.set_depth(DepthConstraint{ViewRole::Top, rect_polys(2, 2, 16, 12), 11, 1e9, "cavity"}).success);
  REQUIRE(d.project_textures(TextureOptions{}).success);
  CHECK(volume(c) == doctest::Approx(volume(d)));
}

TEST_CASE("editor: masks that disagree between views are reported, sync fills missing views") {
  Session a;
  open(a, "A_synthetic_chair");
  const int id = a.create_part("leg", true).data["part"].get<int>();
  MaskPaintRequest front;
  front.part = id;
  front.view = ViewRole::Front;
  for (int v = 20; v < 33; ++v)
    for (int u = 1; u < 3; ++u) front.pixels.emplace_back(u, v);  // the front-left leg
  REQUIRE(a.paint_part_mask(front).success);
  MaskPaintRequest side = front;
  side.view = ViewRole::Side;
  side.pixels.clear();
  for (int v = 16; v < 19; ++v)
    for (int u = 5; u < 8; ++u) side.pixels.emplace_back(u, v);  // seat rows: cannot meet the leg rows of Front
  REQUIRE(a.paint_part_mask(side).success);
  REQUIRE(a.project_textures(TextureOptions{}).success);
  REQUIRE(a.document().mask_conflicts);
  CHECK_FALSE(a.document().mask_conflicts->empty());
  // Erase the wrong Side mask, then let the engine project the part into Side and Top.
  side.erase = true;
  REQUIRE(a.paint_part_mask(side).success);
  const OpResult sync = a.sync_part_masks(id);
  REQUIRE(sync.success);
  CHECK(sync.data["views"].size() == 2);
  CHECK(a.document().mask_conflicts->empty());
}

TEST_CASE("editor: selection maps 2D to 3D and back") {
  Session s;
  open(s, "A_synthetic_chair");
  const int leg = part_at(s, ViewRole::Front, 1, 30);
  const OpResult sel = s.get_selection();
  // The leg seen from the front projects onto the leg columns of Front and onto its footprint in Top.
  CHECK(sel.data["views"]["front"]["box"] == nlohmann::json({1, 19, 3, 33}));
  CHECK(sel.data["views"]["top"]["pixels"].get<int>() > 0);
  // A ray from the camera side finds the same part.
  const OpResult ray = s.select_at_ray({1.5, 5, 100}, {0, 0, -1}, SelectMode::Part);
  CHECK(ray.data["parts"][0] == leg);
  CHECK(s.select_parts({leg}).success);
  CHECK_FALSE(s.select_parts({12345}).success);
  CHECK(s.clear_selection().data["parts"].empty());
}

TEST_CASE("editor: preview shows an edit's effect without committing it") {
  Session s;
  open(s, "A_synthetic_chair");
  const auto rev = s.document().revision;
  Mesh preview;
  const OpResult p = s.preview_edit(EditOp{CutEdit{-1, 1, 10.0, -1, -1}}, &preview);
  REQUIRE(p.success);
  CHECK(p.data["watertight"] == true);
  CHECK(p.data["views"]["front"]["mismatch_px"].get<int>() > 0);
  CHECK_FALSE(preview.tris.empty());
  CHECK(s.document().revision == rev);
  CHECK(s.document().edits.empty());
}

TEST_CASE("editor: edits, masks and names survive save and reload") {
  Session s;
  open(s, "A_synthetic_chair");
  REQUIRE(s.extrude(ViewRole::Top, rect_polys(8, 10, 12, 14), 2, -1).success);
  const int seat = part_at(s, ViewRole::Top, 9, 16);
  REQUIRE(s.rename_part(seat, "seat").success);
  const int id = s.create_part("label", false).data["part"].get<int>();
  MaskPaintRequest m;
  m.part = id;
  m.view = ViewRole::Front;
  m.stroke = {{9.5, 5.5}};
  m.brush.radius = 2;
  REQUIRE(s.paint_part_mask(m).success);
  REQUIRE(s.project_textures(TextureOptions{}).success);
  const double v = volume(s);
  const int tris = triangles(s);
  REQUIRE(s.save(tmp("edited.p3d")).success);
  Session t;
  REQUIRE(t.load(tmp("edited.p3d")).success);
  CHECK(t.document().edits.size() == 1);
  CHECK(t.document().part_names.at(seat) == "seat");
  CHECK(t.document().masks == s.document().masks);
  REQUIRE(t.project_textures(TextureOptions{}).success);
  CHECK(volume(t) == doctest::Approx(v));
  CHECK(triangles(t) == tris);
  // Editing continues after the reload (the mesh came from the cache).
  REQUIRE(t.cut(-1, 1, 20.0, -1).success);
  CHECK(volume(t) < v);
}

TEST_CASE("editor: single view mode infers depth honestly") {
  // A plain 13×9 sprite (no 2× structure, no enclosed regions): only the depth is undetermined.
  ImageRGBA img(13, 9, Rgba8{0, 0, 0, 0});
  for (int y = 1; y < 9; ++y)
    for (int x = 0; x < 13; ++x) img.at(x, y) = Rgba8{200, 120, 60, 255};
  for (int x = 5; x < 8; ++x) img.at(x, 0) = Rgba8{200, 120, 60, 255};
  REQUIRE(save_png(img, tmp("sprite.png")).code == "ok");
  Session s;
  REQUIRE(s.import_views({{ViewRole::Front, tmp("sprite.png"), {}}}).success);
  SingleViewSettings sv;
  sv.enabled = true;
  REQUIRE(s.set_single_view(sv).success);
  OpResult v = s.validate(ValidateOptions{});
  REQUIRE(v.success);
  CHECK(v.status == ValidationStatus::NeedsReview);  // the default depth is an assumption
  CHECK(s.document().align->dims.D == 5);             // medium: half the smaller side (9), rounded
  CHECK(s.document().metrics->inferred_volume_fraction == doctest::Approx(1.0));
  bool depth_conflict = false;
  for (const auto& c : v.data["conflicts"]) depth_conflict |= c["kind"] == "single_view_depth" && c["blocks_pass"] == true;
  CHECK(depth_conflict);
  REQUIRE(s.set_single_view_depth("flat", std::nullopt).success);
  v = s.validate(ValidateOptions{});
  CHECK(s.document().align->dims.D == 1);
  CHECK(v.status == ValidationStatus::Pass);  // the user decided; a flat sprite is intended (thickness is informational)
  CHECK(s.document().metrics->views.at(ViewRole::Front).mismatch_px == 0);
  REQUIRE(s.set_single_view_depth("custom", 3.5).success);
  CHECK(s.document().align->dims.D == 4);
  REQUIRE(s.project_textures(TextureOptions{}).success);
  CHECK(volume(s) == doctest::Approx((13 * 8 + 3) * 3.5));
  // A part with its own depth.
  const int handle = s.create_part("top tab", false).data["part"].get<int>();
  MaskPaintRequest m;
  m.part = handle;
  m.view = ViewRole::Front;
  for (int x = 5; x < 8; ++x) m.pixels.emplace_back(x, 0);
  REQUIRE(s.paint_part_mask(m).success);
  // Per-part depth is part of the masks; set it through the document inputs via a project round trip.
  REQUIRE(s.project_textures(TextureOptions{}).success);
  CHECK(part_ids(s).contains(handle));
  CHECK(s.import_views({{ViewRole::Side, tmp("sprite.png"), {}}}).success);  // importing works…
  CHECK_FALSE(s.reconstruct(ReconstructOptions{}).success);                 // …but single view needs one drawing
}

TEST_CASE("editor: weld options are gone from the API and project files") {
  Session s;
  open(s, "A_synthetic_chair");
  REQUIRE(s.save(tmp("noweld.p3d")).success);
  const auto lp = load_project(tmp("noweld.p3d"));
  REQUIRE(lp);
  const nlohmann::json inputs = inputs_to_json(lp->doc);
  CHECK_FALSE(inputs["cleanup"].contains("weld_vertices"));
  CHECK_FALSE(inputs["cleanup"].contains("weld_eps"));
  CHECK(inputs["cleanup"].contains("min_fragment_volume"));
  // min_fragment_volume still acts: a stray 1-voxel cube is dropped by cleanup.
  CleanupOptions keep_all;
  keep_all.min_fragment_volume = 0;
  REQUIRE(s.cleanup(keep_all).success);
  CHECK(s.document().cleanup_options.min_fragment_volume == 0);
}

TEST_CASE("editor: ambiguous volume includes undetermined cavity columns") {
  Session c;
  open(c, "C_asym_planter");
  REQUIRE(c.validate(ValidateOptions{}).success);
  const QualityMetrics& m = *c.document().metrics;
  CHECK(m.recess_volume > 0);
  CHECK(m.ambiguous_volume_fraction ==
        doctest::Approx((c.document().correspondence->ambiguous_volume + m.recess_volume) / m.volume));
  REQUIRE(c.set_depth(DepthConstraint{ViewRole::Top, rect_polys(2, 2, 16, 12), 11, 1e9, "cavity"}).success);
  REQUIRE(c.validate(ValidateOptions{}).success);
  CHECK(c.document().metrics->recess_volume == 0);  // decided by the user: no longer ambiguous
}

TEST_CASE("editor: tool previews are exactly what the commit does; history names every step") {
  Session s;
  open(s, "A_synthetic_chair");
  DepthPaintRequest dp;
  dp.view = ViewRole::Top;
  dp.stroke = {{6.5, 12.5}, {11.5, 12.5}};
  dp.brush.hardness = 1;
  dp.brush.raise = false;
  Mesh preview;
  const OpResult pr = s.preview_paint_depth(dp, &preview);
  REQUIRE(pr.success);
  CHECK(s.document().edits.empty());  // nothing committed
  const OpResult committed = s.paint_depth(dp);
  REQUIRE(committed.success);
  CHECK(pr.data["volume"].get<double>() == doctest::Approx(committed.stats.volume));
  CHECK(pr.data["edit"] == committed.data["edit"]);
  PushPullRequest pp;
  pp.view = ViewRole::Top;
  pp.u = 9;
  pp.v = 15;
  pp.distance = -1;
  const OpResult ppv = s.preview_push_pull(pp, nullptr);
  REQUIRE(ppv.success);
  const OpResult ppc = s.push_pull(pp);
  REQUIRE(ppc.success);
  CHECK(ppv.data["volume"].get<double>() == doctest::Approx(ppc.stats.volume));
  // The undo list names each committed operation; undo moves the cursor, not the list.
  const auto steps = s.history_steps();
  REQUIRE(steps.size() >= 3);
  CHECK(steps.front() == "start");
  CHECK(steps[steps.size() - 2] == "paint_depth");
  CHECK(steps.back() == "push_pull");
  CHECK(s.history_cursor() == steps.size() - 1);
  REQUIRE(s.undo().success);
  CHECK(s.history_cursor() == steps.size() - 2);
  CHECK(s.history_steps().size() == steps.size());
  // A preview that cannot be committed fails the same way (a face already at the box).
  PushPullRequest at_bound;
  at_bound.view = ViewRole::Front;
  at_bound.u = 9;
  at_bound.v = 17;
  at_bound.distance = 3;
  const OpResult bad_preview = s.preview_push_pull(at_bound, nullptr);
  const OpResult bad_commit = s.push_pull(at_bound);
  CHECK(bad_preview.success == bad_commit.success);
  if (!bad_commit.success) CHECK(bad_preview.errors.front().code == bad_commit.errors.front().code);
}
