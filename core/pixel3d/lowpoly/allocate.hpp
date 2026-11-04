#pragma once
// Budget allocator: splits the triangle budget over the tubes, low-poly by
// construction. A tube costs 2N(R−1) + caps triangles (N sides, R rings; a flat
// cap N−2, an apex N). Everything starts at the minimum (N = 4, one loop per
// joint, only the end stations) and is upgraded in priority order: every tube
// to 6 sides, every joint to the requested loop count, then greedily the
// station or side upgrade with the best importance × error reduction per
// triangle until nothing fits. The total never exceeds the budget.
#include <vector>

#include "pixel3d/lowpoly/layout.hpp"
#include "pixel3d/lowpoly/loft.hpp"

namespace pixel3d::lowpoly {

struct TubePlan {
  int sides = 4;
  int loops = 1;                 // joint loops per joint
  int extra = 0;                 // stations taken from the reducer's insertion order
  Cap cap_start = Cap::Flat, cap_end = Cap::Flat;
  std::vector<int> stations;     // kept dense station indices (sorted; apex ends included)
  std::vector<double> loop_s;    // joint loop positions (arc length)
  std::vector<int> loop_joint;   // joint index per loop
  std::vector<int> loop_index;   // position of the loop inside its joint (0 … loops−1)
  int rings = 0;
  int triangles = 0;
  double profile_error = 0;      // px, after the chosen stations
  double importance = 0;
};

struct Allocation {
  std::vector<TubePlan> tubes;
  int budget = 0;                // the user's budget
  int base_budget = 0;           // for the model itself (half when an inverted hull is added)
  int total = 0;                 // model triangles (hull excluded)
  std::vector<Message> messages; // sides_reduced, joint_loops_reduced, low_budget
};

// Errors: lowpoly.budget_out_of_range, lowpoly.budget_too_small (where.minimum).
Expected<Allocation> allocate(const Layout& l, const Settings& s);

// The rings and caps of a planned tube, with bones and joint-loop weights.
TubeShape shape_for(const Layout& l, const TubeSpec& t, const TubePlan& p, int model_height);

}  // namespace pixel3d::lowpoly
