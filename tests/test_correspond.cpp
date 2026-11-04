#include <doctest/doctest.h>

#include "pipeline_helpers.hpp"

using namespace pixel3d;
using namespace p3dtest;



TEST_CASE("correspond: A legs are inferred_grid and kept") {
  const Corr c = run_fixture("A_synthetic_chair");
  const auto legs = c.in_band(5);
  REQUIRE(legs.size() == 4);
  for (const Candidate* l : legs) CHECK(l->status == CandStatus::InferredGrid);
  CHECK(c.result.ambiguities.empty());
  for (const Candidate* s : c.in_band(15)) CHECK(s->status == CandStatus::Certain);
  // Grid prior: 4 legs kept, 2 would suffice — half the leg volume is inferred.
  double leg_vol = 0;
  for (const Candidate* l : legs) leg_vol += l->volume;
  CHECK(c.result.inferred_volume == doctest::Approx(leg_vol / 2).epsilon(0.01));
}

TEST_CASE("correspond: C legs are ambiguous with alternative covers") {
  const Corr c = run_fixture("C_asym_planter");
  const auto legs = c.in_band(3);
  REQUIRE(legs.size() == 6);
  for (const Candidate* l : legs) CHECK(l->status == CandStatus::Ambiguous);
  REQUIRE(c.count_kind("ambiguous_correspondence") == 1);
  const Ambiguity& a = c.result.ambiguities.front();
  CHECK(a.candidates.size() == 6);
  CHECK(a.alternative_covers.size() >= 2);
  for (const auto& cover : a.alternative_covers) CHECK(cover.size() == 3);
  CHECK(c.result.ambiguous_volume > 0);
  // The handle and body are certain.
  for (const Candidate* b : c.in_band(15)) CHECK(b->status == CandStatus::Certain);
}

TEST_CASE("correspond: C-bottom resolves exactly 3 certain legs") {
  const Corr c = run_fixture("C_asym_planter_bottom", {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Bottom});
  // Real legs in aligned coordinates (world minus crop offset 4): (1..3, 11..13), (8..10, 1..3), (15..17, 11..13).
  const Candidate* real[] = {c.at(3, 1, 11), c.at(3, 8, 1), c.at(3, 15, 11)};
  const Candidate* phantom[] = {c.at(3, 1, 1), c.at(3, 8, 11), c.at(3, 15, 1)};
  for (const Candidate* r : real) {
    REQUIRE(r);
    CHECK(r->status == CandStatus::Certain);
  }
  for (const Candidate* p : phantom) {
    REQUIRE(p);
    CHECK(p->status == CandStatus::Removed);
  }
  CHECK(c.count_kind("ambiguous_correspondence") == 0);
}

TEST_CASE("correspond: front-only stretcher is ambiguous") {
  // Four 2×2 legs, a seat, and a stretcher between the FRONT legs only.
  const Dims d{8, 12, 8};
  const Corr c = run_boxes({Box{0, 0, 0, 2, 10, 2}, Box{6, 0, 0, 8, 10, 2}, Box{0, 0, 6, 2, 10, 8},
                            Box{6, 0, 6, 8, 10, 8}, Box{0, 10, 0, 8, 12, 8}, Box{2, 4, 6, 6, 6, 8}},
                           d);
  const auto stretcher_band = c.in_band(4);
  REQUIRE(stretcher_band.size() == 6);
  CHECK(c.count_kind("ambiguous_correspondence") == 1);
  // Leg pieces through the stretcher band continue the legs above and below.
  CHECK(c.at(4, 0, 0)->status == CandStatus::InferredContinuation);
  CHECK(c.at(4, 7, 7)->status == CandStatus::InferredContinuation);
  CHECK(c.at(4, 3, 7)->status == CandStatus::Ambiguous);  // stretcher at the front…
  CHECK(c.at(4, 3, 0)->status == CandStatus::Ambiguous);  // …or at the back?
}

TEST_CASE("correspond: B slats are certain") {
  const Corr c = run_fixture("B_hand_chair");
  CHECK(c.result.ambiguities.empty());
  for (int y : {18, 20, 22, 24, 26}) {
    CAPTURE(y);
    for (const Candidate* p : c.in_band(y)) CHECK(p->status == CandStatus::Certain);
  }
  for (const Candidate* l : c.in_band(5)) CHECK(l->status == CandStatus::InferredGrid);
}

TEST_CASE("correspond: user decision resolves C legs") {
  const Corr first = run_fixture("C_asym_planter");
  std::vector<UserDecision> dec;
  const int real_xz[3][2] = {{1, 11}, {8, 1}, {15, 11}};
  for (const auto& xz : real_xz) dec.push_back({first.at(3, xz[0], xz[1])->id, true});
  for (const Candidate* l : first.in_band(3)) {
    bool real = false;
    for (const auto& d : dec) real |= d.candidate == l->id;
    if (!real) dec.push_back({l->id, false});
  }
  const Corr c = run_fixture("C_asym_planter", {ViewRole::Front, ViewRole::Side, ViewRole::Top}, dec);
  CHECK(c.count_kind("ambiguous_correspondence") == 0);
  int kept = 0;
  for (const Candidate* l : c.in_band(3)) kept += is_kept(l->status) ? 1 : 0;
  CHECK(kept == 3);
}

TEST_CASE("correspond: removing sole coverer is refused") {
  // B's left back post in a slat-gap band is the only thing explaining that Front strand.
  const Corr first = run_fixture("B_hand_chair");
  const Candidate* post = first.at(24, 1, 1);
  REQUIRE(post);
  const Corr c = run_fixture("B_hand_chair", {ViewRole::Front, ViewRole::Side, ViewRole::Top}, {{post->id, false}});
  CHECK(c.count_kind("edit.uncovers_atom") == 1);
  CHECK(is_kept(c.cands.cands[post->id].status));
}

TEST_CASE("correspond: parts are strand blocks so legs stay separate") {
  const Corr c = run_fixture("A_synthetic_chair");
  CHECK(c.result.parts.size() == 5);  // four legs + seat/back
  for (const Part3D& p : c.result.parts) {
    std::set<std::pair<int, int>> strands;  // (band, front strand)
    for (int id : p.candidates) {
      const Candidate& cand = c.cands.cands[id];
      const Atom& fa = c.bands.view(cand.band, ViewRole::Front)->atoms[cand.front_atom];
      strands.insert({cand.band, fa.strand});
    }
    std::set<int> bands_seen;
    for (auto [band, strand] : strands) CHECK(bands_seen.insert(band).second);  // one front strand per band
  }
}

TEST_CASE("correspond: palette pins legs that the silhouettes cannot") {
  // Three legs (Front 3 strands × Side 2): red legs at the back, a blue leg at the front.
  // Silhouettes alone allow 6 combinations; the colours allow exactly one.
  const Rgba8 red{200, 40, 40, 255}, blue{40, 60, 200, 255}, wood{150, 100, 60, 255};
  const Dims d{9, 10, 8};
  const Corr c = run_boxes({Box{0, 0, 0, 2, 8, 2, red}, Box{3, 0, 6, 5, 8, 8, blue}, Box{7, 0, 0, 9, 8, 2, red},
                            Box{0, 8, 0, 9, 10, 8, wood}},
                           d);
  CHECK(c.count_kind("ambiguous_correspondence") == 0);
  CHECK(c.at(3, 0, 0)->status == CandStatus::CertainPalette);
  CHECK(c.at(3, 3, 6)->status == CandStatus::CertainPalette);
  CHECK(c.at(3, 7, 0)->status == CandStatus::CertainPalette);
  CHECK(c.at(3, 0, 6)->status == CandStatus::Removed);
  CHECK(c.at(3, 3, 0)->status == CandStatus::Removed);
  CHECK(c.at(3, 7, 6)->status == CandStatus::Removed);
}
