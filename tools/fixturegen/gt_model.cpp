#include "fixturegen/gt_model.hpp"

namespace p3dfix {

bool ConvexPart::contains(vec3d p, double eps) const {
  for (const Plane& pl : planes)
    if (pixel3d::dot(pl.n, p) > pl.d + eps) return false;
  return true;
}

bool AABB::contains(vec3d p, double eps) const {
  return p.x >= lo.x - eps && p.y >= lo.y - eps && p.z >= lo.z - eps && p.x <= hi.x + eps && p.y <= hi.y + eps &&
         p.z <= hi.z + eps;
}

bool GroundTruth::inside(vec3d p) const {
  for (const auto& part : parts)
    if (part.contains(p)) return true;
  return false;
}

double GroundTruth::volume(int s) const {
  long count = 0;
  const double step = 1.0 / s;
  for (int z = 0; z < dims.D * s; ++z)
    for (int y = 0; y < dims.H * s; ++y)
      for (int x = 0; x < dims.W * s; ++x)
        if (inside({(x + 0.5) * step, (y + 0.5) * step, (z + 0.5) * step})) ++count;
  return count * step * step * step;
}

ConvexPart box(std::string name, vec3d lo, vec3d hi, int material) {
  return ConvexPart{std::move(name),
                    {{{-1, 0, 0}, -lo.x},
                     {{1, 0, 0}, hi.x},
                     {{0, -1, 0}, -lo.y},
                     {{0, 1, 0}, hi.y},
                     {{0, 0, -1}, -lo.z},
                     {{0, 0, 1}, hi.z}},
                    material};
}

namespace {
const std::array<Rgba8, 4> kWood = {Rgba8{0x5c, 0x3a, 0x22, 0xff}, Rgba8{0x80, 0x54, 0x2f, 0xff},
                                    Rgba8{0xa4, 0x70, 0x42, 0xff}, Rgba8{0xc6, 0x92, 0x5c, 0xff}};
const std::array<Rgba8, 4> kTerracotta = {Rgba8{0x6b, 0x2f, 0x1f, 0xff}, Rgba8{0x9c, 0x4a, 0x2c, 0xff},
                                          Rgba8{0xc4, 0x67, 0x3d, 0xff}, Rgba8{0xe0, 0x8a, 0x5a, 0xff}};
// Steel-teal legs: a hue clearly apart from terracotta (materials closer than
// ~25° of hue form one family by design, so shading never splits a part).
const std::array<Rgba8, 4> kSteel = {Rgba8{0x2b, 0x45, 0x50, 0xff}, Rgba8{0x3a, 0x5d, 0x6b, 0xff},
                                     Rgba8{0x4c, 0x76, 0x87, 0xff}, Rgba8{0x63, 0x92, 0xa4, 0xff}};
}  // namespace

GroundTruth make_fixture_A() {
  GroundTruth g;
  g.name = "A_synthetic_chair";
  g.dims = Dims{20, 34, 22};
  g.ramps = {kWood};
  for (double x0 : {2.0, 16.0})
    for (double z0 : {6.0, 18.0})
      g.parts.push_back(box("leg", {x0, 0, z0}, {x0 + 2, 14, z0 + 2}, 0));
  g.parts.push_back(box("seat", {1, 14, 5}, {19, 17, 21}, 0));
  // Reclined back: z ∈ [z0(y), z0(y)+2] with z0(y) = 5 − (y − 17)/4 (slope 1:4 in the Side view).
  g.parts.push_back(ConvexPart{"back",
                               {{{-1, 0, 0}, -2},
                                {{1, 0, 0}, 18},
                                {{0, -1, 0}, -17},
                                {{0, 1, 0}, 33},
                                {{0, -0.25, -1}, -9.25},
                                {{0, 0.25, 1}, 11.25}},
                               0});
  return g;
}

GroundTruth make_fixture_C() {
  GroundTruth g;
  g.name = "C_asym_planter";
  g.dims = Dims{26, 22, 22};
  g.ramps = {kTerracotta, kSteel};
  // Open-top planter: outer x∈[4,22] z∈[4,18] y∈[8,22]; cavity x∈[6,20] z∈[6,16] y∈[11,22].
  g.parts.push_back(box("floor", {4, 8, 4}, {22, 11, 18}, 0));
  g.parts.push_back(box("wall_rear", {4, 11, 4}, {22, 22, 6}, 0));
  g.parts.push_back(box("wall_front", {4, 11, 16}, {22, 22, 18}, 0));
  g.parts.push_back(box("wall_left", {4, 11, 6}, {6, 22, 16}, 0));
  g.parts.push_back(box("wall_right", {20, 11, 6}, {22, 22, 16}, 0));
  // Three legs: three distinct X positions, two distinct Z positions (Front 3 strands × Side 2).
  g.parts.push_back(box("leg_front_left", {5, 0, 15}, {7, 8, 17}, 1));
  g.parts.push_back(box("leg_rear_middle", {12, 0, 5}, {14, 8, 7}, 1));
  g.parts.push_back(box("leg_front_right", {19, 0, 15}, {21, 8, 17}, 1));
  // One handle lug on the +X side only.
  g.parts.push_back(box("handle", {22, 14, 9}, {25, 17, 13}, 0));
  g.shadow_volumes.push_back(AABB{{6, 11, 6}, {20, 22, 16}});
  return g;
}

}  // namespace p3dfix
