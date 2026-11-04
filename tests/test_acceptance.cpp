// M1 acceptance (spec §12): fixtures A, B, C, C-bottom through the real Session.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <set>

#include "fixturegen/gt_model.hpp"
#include "fixturegen/gt_render.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/render/camera.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

std::string fixture(const std::string& name, const std::string& file) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + file + ".png";
}

struct Run {
  Session s;
  OpResult v;
};

std::unique_ptr<Run> run(const std::string& name, ToleranceProfile profile, std::vector<ViewImport> extra = {},
                         const std::string& front_file = "front") {
  auto r = std::make_unique<Run>();
  ProjectSettings ps;
  ps.profile = profile;
  REQUIRE(r->s.new_project(ps).success);
  std::vector<ViewImport> views = {{ViewRole::Front, fixture(name, front_file), {}},
                                   {ViewRole::Side, fixture(name, "side"), {}},
                                   {ViewRole::Top, fixture(name, "top"), {}}};
  views.insert(views.end(), extra.begin(), extra.end());
  REQUIRE(r->s.import_views(views).success);
  r->v = r->s.validate(ValidateOptions{});
  REQUIRE(r->v.success);
  return r;
}

std::set<std::string> blocking_kinds(const OpResult& v) {
  std::set<std::string> k;
  for (const auto& c : v.data["conflicts"])
    if (c["blocks_pass"].get<bool>()) k.insert(c["kind"].get<std::string>());
  return k;
}

bool has_kind(const OpResult& v, const std::string& kind) {
  for (const auto& c : v.data["conflicts"])
    if (c["kind"] == kind) return true;
  return false;
}

// World offset of the aligned box inside the fixture's ground-truth frame.
vec3d crop_offset(const Document& d, const p3dfix::GroundTruth& gt) {
  vec3d o{0, 0, 0};
  for (const SourceView& v : d.views) {
    if (v.role == ViewRole::Front) {
      o.x = v.crop.x0 - v.pad_x;
      o.y = gt.dims.H - v.crop.y1 - (d.align->dims.H - (v.crop.y1 - v.crop.y0) - v.pad_y);
    }
    if (v.role == ViewRole::Side) o.z = v.crop.x0 - v.pad_x;
  }
  return o;
}

// Inside test for a closed mesh at sample points: one ray per (y, z) scanline.
std::vector<uint8_t> sample_mesh(const Mesh& m, Dims d, int s) {
  const int nx = d.W * s, ny = d.H * s, nz = d.D * s;
  std::vector<uint8_t> in(static_cast<size_t>(nx) * ny * nz, 0);
  for (int k = 0; k < nz; ++k)
    for (int j = 0; j < ny; ++j) {
      // Nudge the ray off the sample lattice so it never runs exactly along a
      // shared triangle edge (which would count one crossing twice).
      const double y = (j + 0.5) / s + 1.2345e-7, z = (k + 0.5) / s + 2.7182e-7;
      std::vector<double> xs;
      for (const auto& t : m.tris) {
        const vec3d a = m.positions[t[0]], b = m.positions[t[1]], c = m.positions[t[2]];
        // Barycentric test of (y, z) in the triangle's YZ projection.
        const double det = (b.y - a.y) * (c.z - a.z) - (c.y - a.y) * (b.z - a.z);
        if (std::abs(det) < 1e-12) continue;
        const double u = ((y - a.y) * (c.z - a.z) - (c.y - a.y) * (z - a.z)) / det;
        const double w = ((b.y - a.y) * (z - a.z) - (y - a.y) * (b.z - a.z)) / det;
        if (u < 0 || w < 0 || u + w > 1) continue;
        xs.push_back(a.x + u * (b.x - a.x) + w * (c.x - a.x));
      }
      std::sort(xs.begin(), xs.end());
      for (int i = 0; i < nx; ++i) {
        const double x = (i + 0.5) / s;
        const auto crossings = std::lower_bound(xs.begin(), xs.end(), x) - xs.begin();
        if (crossings % 2 == 1) in[(static_cast<size_t>(k) * ny + j) * nx + i] = 1;
      }
    }
  return in;
}

double volume_iou(const Mesh& m, Dims d, const p3dfix::GroundTruth& gt, vec3d off, int s = 2) {
  const auto in = sample_mesh(m, d, s);
  long inter = 0, uni = 0;
  for (int k = 0; k < d.D * s; ++k)
    for (int j = 0; j < d.H * s; ++j)
      for (int i = 0; i < d.W * s; ++i) {
        const bool a = in[(static_cast<size_t>(k) * d.H * s + j) * d.W * s + i];
        const bool b = gt.inside({(i + 0.5) / s + off.x, (j + 0.5) / s + off.y, (k + 0.5) / s + off.z});
        inter += a && b;
        uni += a || b;
      }
  return uni ? static_cast<double>(inter) / uni : 1.0;
}

}  // namespace

TEST_CASE("acceptance: A PASS strict") {
  auto r = run("A_synthetic_chair", ToleranceProfile::Strict);
  CHECK(r->v.status == ValidationStatus::Pass);
  const Document& d = r->s.document();
  for (const auto& [role, m] : d.metrics->views) CHECK(m.iou >= 0.98);
  for (const auto& rot : d.metrics->rotations) CHECK(rot.iou >= 0.98);
  CHECK(d.metrics->topology.watertight);
  CHECK(d.metrics->topology.nonmanifold_edges == 0);
  CHECK(d.metrics->topology.internal_faces == 0);
  CHECK(d.metrics->topology.triangle_count <= 600);
  CHECK(r->v.metrics["glb"]["ok"] == true);
  CHECK(d.metrics->turntable.all_non_empty);
  CHECK(d.metrics->turntable.min_area_ratio >= 0.25);
  CHECK(has_kind(r->v, "inferred_grid"));
  const auto gt = p3dfix::make_fixture_A();
  const double iou = volume_iou(d.solid->mesh, d.align->dims, gt, crop_offset(d, gt));
  CAPTURE(iou);
  CHECK(iou >= 0.95);
}

TEST_CASE("acceptance: B PASS hand") {
  auto r = run("B_hand_chair", ToleranceProfile::Hand);
  CHECK(r->v.status == ValidationStatus::Pass);
  const Document& d = r->s.document();
  for (ViewRole role : {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
    CAPTURE(to_string(role));
    CHECK(d.metrics->views.at(role).color_exact_fraction >= 0.98);
  }
  CHECK(d.metrics->topology.watertight);
  // Slat gaps stay open seen from behind (yaw 180 = Back camera): rows of world Y 24-25 and 20-21.
  const RenderTarget back = render_mesh_flat(d.solid->mesh, camera_for_view(ViewRole::Back, d.align->dims, 1));
  for (int y : {20, 21, 24, 25})
    for (int x = 4; x <= 11; ++x) {
      CAPTURE(y);
      CHECK_FALSE(back.coverage.get(d.align->dims.W - 1 - x, d.align->dims.H - 1 - y));
    }
}

TEST_CASE("acceptance: C NEEDS_REVIEW with expected kinds") {
  auto r = run("C_asym_planter", ToleranceProfile::Strict);
  CHECK(r->v.status == ValidationStatus::NeedsReview);
  CHECK(blocking_kinds(r->v) == std::set<std::string>{"ambiguous_correspondence", "possible_recess"});
  const Document& d = r->s.document();
  // The one-sided handle is reconstructed with certainty.
  const auto gt = p3dfix::make_fixture_C();
  const vec3d off = crop_offset(d, gt);
  for (const Candidate& c : d.candidates->cands) {
    const int x_world = static_cast<int>(c.footprint.x1 + off.x);
    if (x_world == 25) CHECK(c.status == CandStatus::Certain);  // handle reaches x = 25
  }
  // Ambiguity recall: phantom volume (hull − GT) inside flagged regions.
  const Dims dims = d.align->dims;
  const auto masks = pipeline::masks_of(d);
  const VoxelGrid hull = voxelize_candidates(*d.candidates, masks, *d.bands, dims);
  CandidateSet ambiguous = *d.candidates;
  for (Candidate& c : ambiguous.cands)
    if (c.status != CandStatus::Ambiguous) c.status = CandStatus::Removed;
  const VoxelGrid flagged_parts = voxelize_candidates(ambiguous, masks, *d.bands, dims);
  long phantom = 0, caught = 0, missed = 0, gt_cells = 0, gt_missing = 0;
  for (int z = 0; z < dims.D; ++z)
    for (int y = 0; y < dims.H; ++y)
      for (int x = 0; x < dims.W; ++x) {
        const bool in_gt = gt.inside({x + 0.5 + off.x, y + 0.5 + off.y, z + 0.5 + off.z});
        const bool in_hull = hull.get(x, y, z);
        gt_cells += in_gt;
        gt_missing += in_gt && !in_hull;
        if (!in_hull || in_gt) continue;
        ++phantom;
        bool flagged = flagged_parts.get(x, y, z);
        for (const RecessFinding& f : *d.recesses)
          if (f.kind == "possible_recess" && f.view == ViewRole::Top)
            flagged |= x >= f.box.x0 && x < f.box.x1 && z >= f.box.y0 && z < f.box.y1;
        (flagged ? caught : missed)++;
      }
  CAPTURE(phantom);
  CAPTURE(caught);
  REQUIRE(phantom > 0);
  CHECK(static_cast<double>(caught) / phantom >= 0.9);
  CHECK(missed + gt_missing <= 0.02 * gt_cells);
}

TEST_CASE("acceptance: C-bottom legs certain") {
  auto r = run("C_asym_planter_bottom", ToleranceProfile::Strict, {{ViewRole::Bottom, fixture("C_asym_planter_bottom", "bottom"), {}}});
  CHECK(blocking_kinds(r->v) == std::set<std::string>{"possible_recess"});
  const Document& d = r->s.document();
  const Band* legs = d.bands->band_containing(3);
  REQUIRE(legs);
  int kept = 0;
  for (const Candidate& c : d.candidates->cands)
    if (c.band == legs->id && is_kept(c.status)) {
      ++kept;
      CHECK(c.status == CandStatus::Certain);
    }
  CHECK(kept == 3);
}

TEST_CASE("pipeline: B with 1px leg discrepancy stays PASS and lists conflict") {
  auto r = run("B_hand_chair", ToleranceProfile::Hand, {}, "front_1px_off");
  CHECK(r->v.status == ValidationStatus::Pass);
  CHECK(has_kind(r->v, "silhouette_conflict"));
  CHECK(r->s.document().metrics->views.at(ViewRole::Front).mismatch_px >= 1);
}

TEST_CASE("pipeline: stray pixel in front yields conflict and no extra shell") {
  auto front = load_png(fixture("A_synthetic_chair", "front"));
  REQUIRE(front.has_value());
  front->at(0, 0) = Rgba8{30, 200, 30, 255};  // far from the chair
  const fs::path dir = fs::temp_directory_path() / "pixel3d_tests";
  fs::create_directories(dir);
  const fs::path stray = dir / "front_stray.png";
  REQUIRE(save_png(*front, stray).code == "ok");
  Session s;
  REQUIRE(s.import_views({{ViewRole::Front, stray, {}},
                          {ViewRole::Side, fixture("A_synthetic_chair", "side"), {}},
                          {ViewRole::Top, fixture("A_synthetic_chair", "top"), {}}})
              .success);
  const OpResult v = s.validate(ValidateOptions{});
  REQUIRE(v.success);
  CHECK(has_kind(v, "silhouette_conflict"));
  CHECK(s.document().solid->topology.shells == 1);
  CHECK(v.status != ValidationStatus::Fail);
}

TEST_CASE("pipeline: many scattered parts stay watertight and exportable") {
  // 40 random boxes: many separate shells, edge/point contacts, ambiguous pairings.
  p3dfix::GroundTruth g;
  g.dims = Dims{64, 64, 64};
  g.ramps = {{Rgba8{90, 60, 30, 255}, Rgba8{120, 80, 40, 255}, Rgba8{150, 100, 60, 255}, Rgba8{180, 130, 80, 255}}};
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> pos(0, 56), sz(2, 8);
  for (int i = 0; i < 40; ++i) {
    const double x = pos(rng), y = pos(rng), z = pos(rng);
    g.parts.push_back(p3dfix::box("b", {x, y, z}, {x + sz(rng), y + sz(rng), z + sz(rng)}, 0));
  }
  const fs::path dir = fs::temp_directory_path() / "pixel3d_tests";
  fs::create_directories(dir);
  std::vector<ViewImport> views;
  for (ViewRole r : {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
    const fs::path p = dir / ("scatter_" + std::string(to_string(r)) + ".png");
    REQUIRE(save_png(p3dfix::render_gt_view(g, r), p).code == "ok");
    views.push_back({r, p, {}});
  }
  Session s;
  REQUIRE(s.import_views(views).success);
  const OpResult v = s.validate(ValidateOptions{});
  REQUIRE(v.success);
  CHECK(s.document().solid->topology.watertight);
  CHECK(s.document().solid->topology.nonmanifold_edges == 0);
  CHECK(v.metrics["glb"]["ok"] == true);
  CHECK(v.status == ValidationStatus::NeedsReview);  // scattered parts are honestly ambiguous, not broken
}
