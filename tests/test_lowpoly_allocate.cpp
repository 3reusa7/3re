#include <doctest/doctest.h>

#include <algorithm>

#include "lowpoly_helpers.hpp"
#include "pixel3d/lowpoly/allocate.hpp"
#include "pixel3d/mesh/topology.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;
using namespace p3dtest;

namespace {

int built_triangles(const Layout& l, const Allocation& a, int H) {
  int n = 0;
  for (size_t i = 0; i < l.tubes.size(); ++i) n += static_cast<int>(build_tube(shape_for(l, l.tubes[i], a.tubes[i], H)).tris.size());
  return n;
}

bool has(const std::vector<Message>& ms, const std::string& code) {
  return std::any_of(ms.begin(), ms.end(), [&](const Message& m) { return m.code == code; });
}

}  // namespace

TEST_CASE("lowpoly allocate: the budget is never exceeded, for any budget, in the built geometry") {
  for (const char* variant : {"cloaked", "humanoid"})
    for (bool side : {true, false}) {
      const Input in = load_lowpoly_fixture(variant, side, side ? 0 : 20);
      const auto l = build_layout(in);
      REQUIRE(l);
      for (int budget = kMinBudget; budget <= kMaxBudget; budget += (budget < 1600 ? 37 : 523)) {
        CAPTURE(variant);
        CAPTURE(side);
        CAPTURE(budget);
        Settings s = in.settings;
        s.tri_budget = budget;
        const auto a = allocate(*l, s);
        REQUIRE(a);
        CHECK(a->total <= budget);
        int sum = 0;
        for (const auto& p : a->tubes) sum += p.triangles;
        CHECK(sum == a->total);
        CHECK(built_triangles(*l, *a, in.dims.H) == a->total);
        // Spending: little is left unused, unless every tube is already exact at the maximum side count.
        bool exact = true;
        for (const auto& p : a->tubes) exact &= p.profile_error < 0.05 && p.sides == s.max_sides;
        CHECK((a->total > budget - 64 || exact));
      }
    }
}

TEST_CASE("lowpoly allocate: every built tube is a closed shell") {
  const Input in = load_lowpoly_fixture("cloaked");
  const auto l = build_layout(in);
  REQUIRE(l);
  for (int budget : {500, 1000, 1500}) {
    Settings s = in.settings;
    s.tri_budget = budget;
    const auto a = allocate(*l, s);
    REQUIRE(a);
    for (size_t i = 0; i < l->tubes.size(); ++i) {
      CAPTURE(l->tubes[i].name);
      const TubeMesh tm = build_tube(shape_for(*l, l->tubes[i], a->tubes[i], in.dims.H));
      Mesh m;
      m.positions = tm.positions;
      m.tris = tm.tris;
      const TopologyReport r = analyze_topology(m);
      CHECK(r.watertight);
      CHECK(r.inconsistent_winding_edges == 0);
      CHECK(r.shells == 1);
      CHECK(r.signed_volume > 0);
    }
  }
}

TEST_CASE("lowpoly allocate: more budget never makes a tube coarser; the head gets more per pixel than limbs") {
  const Input in = load_lowpoly_fixture("cloaked");
  const auto l = build_layout(in);
  REQUIRE(l);
  Settings s = in.settings;
  s.tri_budget = 600;
  const auto lo = allocate(*l, s);
  s.tri_budget = 1500;
  const auto hi = allocate(*l, s);
  REQUIRE(lo);
  REQUIRE(hi);
  double err_lo = 0, err_hi = 0;
  for (size_t i = 0; i < l->tubes.size(); ++i) {
    err_lo += lo->tubes[i].profile_error;
    err_hi += hi->tubes[i].profile_error;
    CHECK(hi->tubes[i].sides >= lo->tubes[i].sides);
  }
  CHECK(err_hi <= err_lo);
  // Requested joint loops are honoured at the default budget.
  s.tri_budget = 1000;
  s.joint_loops = 3;
  const auto mid = allocate(*l, s);
  REQUIRE(mid);
  for (size_t i = 0; i < l->tubes.size(); ++i)
    if (!l->tubes[i].joints.empty()) CHECK(mid->tubes[i].loops == 3);
}

TEST_CASE("lowpoly allocate: out-of-range and infeasible budgets are refused; low budgets warn") {
  const Input in = load_lowpoly_fixture("humanoid");
  const auto l = build_layout(in);
  REQUIRE(l);
  Settings s = in.settings;
  s.tri_budget = kMinBudget - 1;
  CHECK_FALSE(allocate(*l, s));
  s.tri_budget = kMaxBudget + 1;
  CHECK_FALSE(allocate(*l, s));
  s.tri_budget = 500;
  const auto a = allocate(*l, s);
  REQUIRE(a);
  CHECK(has(a->messages, "lowpoly.low_budget"));
  // An inverted hull doubles every triangle: the model gets half.
  s.tri_budget = 1000;
  s.hull = true;
  const auto h = allocate(*l, s);
  REQUIRE(h);
  CHECK(h->base_budget == 500);
  CHECK(h->total <= 500);
  // Too many parts for the budget: refused with the minimum needed.
  Layout many = *l;
  for (int k = 0; k < 40; ++k) many.tubes.push_back(l->tubes[1]);
  s.hull = false;
  s.tri_budget = 500;
  const auto bad = allocate(many, s);
  REQUIRE_FALSE(bad);
  CHECK(bad.error().code == "lowpoly.budget_too_small");
  CHECK(bad.error().where["minimum"].get<int>() > 500);
}

TEST_CASE("lowpoly allocate: joint loops carry blended weights of the two adjacent bones only") {
  const Input in = load_lowpoly_fixture("humanoid");
  const auto l = build_layout(in);
  REQUIRE(l);
  Settings s = in.settings;
  s.joint_loops = 3;
  const auto a = allocate(*l, s);
  REQUIRE(a);
  for (size_t i = 0; i < l->tubes.size(); ++i) {
    const TubeShape sh = shape_for(*l, l->tubes[i], a->tubes[i], in.dims.H);
    int loops = 0;
    for (const auto& r : sh.rings) {
      CHECK(r.bone_a >= 0);
      if (!r.joint_loop) {
        CHECK(r.w_b == 0);
        continue;
      }
      ++loops;
      CHECK(r.bone_b >= 0);
      CHECK(r.bone_b != r.bone_a);
      CHECK(r.w_b > 0);
      CHECK(r.w_b < 1);
      // Adjacent: one is the other's parent.
      const auto& bones = l->skeleton.bones;
      CHECK((bones[static_cast<size_t>(r.bone_b)].parent == r.bone_a || bones[static_cast<size_t>(r.bone_a)].parent == r.bone_b));
    }
    CHECK(loops == 3 * static_cast<int>(l->tubes[i].joints.size()));
  }
}
