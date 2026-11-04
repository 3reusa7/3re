// Polished styles: baked ambient occlusion (step 7) and shading bevel normals (step 4b).
#include <doctest/doctest.h>

#include <cmath>
#include <optional>

#include "pixel3d/reconstruct/part_geometry.hpp"
#include "pixel3d/style/bake.hpp"
#include "pixel3d/texture/unique_uv.hpp"

using namespace pixel3d;

namespace {

manifold::Manifold box(double x0, double y0, double z0, double x1, double y1, double z1) {
  return manifold::Manifold::Cube({x1 - x0, y1 - y0, z1 - z0}).Translate({x0, y0, z0});
}

// Flat-shaded textured mesh of a solid (one part), plus its crisp Mesh.
struct Built {
  Mesh crisp;
  TexturedMesh tm;
  UniqueUVReport uv;
};

Built build(const manifold::Manifold& m, double tpu = 4) {
  PartGeometry g;
  g.parts[0] = m;
  Built b;
  b.crisp = assemble_parts(g, CleanupOptions{}).mesh;
  TexturedMesh t;
  for (size_t i = 0; i < b.crisp.tris.size(); ++i) {
    const vec3d n = normalize(tri_normal_raw(b.crisp, i));
    for (int k = 0; k < 3; ++k) {
      t.indices.push_back(static_cast<uint32_t>(t.positions.size()));
      t.positions.push_back(b.crisp.positions[b.crisp.tris[i][k]]);
      t.normals.push_back(n);
      t.uvs.push_back({0, 0});
    }
    t.tri_part.push_back(0);
  }
  b.tm = add_unique_uv(t, tpu, 2, &b.uv);
  return b;
}

// Texel of the baked image at world point p (which must lie on the surface, facing n).
std::optional<Rgba8> sample(const Built& b, const ImageRGBA& img, vec3d p, vec3d n) {
  const TexturedMesh& m = b.tm;
  for (size_t t = 0; t < m.indices.size() / 3; ++t) {
    const vec3d a = m.positions[m.indices[3 * t]], bb = m.positions[m.indices[3 * t + 1]], c = m.positions[m.indices[3 * t + 2]];
    const vec3d fn = normalize(cross(bb - a, c - a));
    if (dot(fn, n) < 0.99 || std::abs(dot(p - a, fn)) > 1e-6) continue;
    const vec3d v0 = bb - a, v1 = c - a, v2 = p - a;
    const double d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
    const double den = d00 * d11 - d01 * d01, v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
    if (v < -1e-9 || w < -1e-9 || v + w > 1 + 1e-9) continue;
    const vec2d uv = m.uvs1[m.indices[3 * t]] * (1 - v - w) + m.uvs1[m.indices[3 * t + 1]] * v + m.uvs1[m.indices[3 * t + 2]] * w;
    return img.at(std::min(img.width() - 1, static_cast<int>(uv.x * img.width())),
                  std::min(img.height() - 1, static_cast<int>(uv.y * img.height())));
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("ao: open faces are unoccluded, an inner corner is darker, results are deterministic") {
  // A step: a low block with a tall block on its back half.
  const Built b = build(box(0, 0, 0, 10, 2, 10) + box(0, 2, 0, 10, 8, 5));
  AOOptions o;
  o.rays = 64;
  o.max_distance = 4;
  const ImageRGBA ao = bake_ambient_occlusion(b.tm, b.uv.width, b.uv.height, o);
  CHECK(ao == bake_ambient_occlusion(b.tm, b.uv.width, b.uv.height, o));
  REQUIRE(ao.width() == b.uv.width);
  const auto top = sample(b, ao, {5, 8, 2.5}, {0, 1, 0});
  const auto open_step = sample(b, ao, {5, 2, 9.5}, {0, 1, 0});  // step top, far from the wall
  const auto corner = sample(b, ao, {5, 2, 5.3}, {0, 1, 0});     // step top, against the wall
  REQUIRE(top);
  REQUIRE(open_step);
  REQUIRE(corner);
  CHECK(top->r == 255);
  CHECK(open_step->r >= 240);
  CHECK(corner->r < 180);
  CHECK(corner->r < open_step->r);
  for (const Rgba8& px : ao.pixels()) CHECK(px.a == 255);
}

TEST_CASE("bevel normals: convex edges look round within the radius, faces stay flat, concave edges stay sharp") {
  const Built b = build(box(0, 0, 0, 10, 2, 10) + box(0, 2, 0, 10, 8, 5), 8);
  BevelOptions o;
  o.radius = 2;
  const ImageRGBA nm = bake_bevel_normals(b.tm, b.crisp, b.uv.width, b.uv.height, o);
  auto decode = [](Rgba8 c) { return vec3d{c.r / 127.5 - 1, c.g / 127.5 - 1, c.b / 127.5 - 1}; };
  // Middle of the tall block's top: flat (tangent space +Z).
  const auto mid = sample(b, nm, {5, 8, 2.5}, {0, 1, 0});
  REQUIRE(mid);
  CHECK(decode(*mid).z > 0.99);
  // Top face, 0.25 px from its convex front edge (z = 5): tilted by close to 45°.
  const auto edge = sample(b, nm, {5, 8, 4.75}, {0, 1, 0});
  REQUIRE(edge);
  CHECK(decode(*edge).z < 0.85);
  CHECK(decode(*edge).z > 0.6);
  // 3 px from that edge (beyond the radius): flat again.
  const auto far = sample(b, nm, {5, 8, 2.0}, {0, 1, 0});
  REQUIRE(far);
  CHECK(decode(*far).z > 0.99);
  // Step top right next to the wall (a concave edge): flat.
  const auto cove = sample(b, nm, {5, 2, 5.25}, {0, 1, 0});
  REQUIRE(cove);
  CHECK(decode(*cove).z > 0.99);
}

TEST_CASE("bevel normals: a fillet never exceeds half the local thickness") {
  // A 2 px post next to a thick block, both 1.5 px below their top edge.
  const Built b = build(box(0, 0, 0, 2, 10, 2) + box(6, 0, 0, 14, 10, 8), 8);
  BevelOptions o;
  o.radius = 3;
  const ImageRGBA nm = bake_bevel_normals(b.tm, b.crisp, b.uv.width, b.uv.height, o);
  auto decode = [](Rgba8 c) { return vec3d{c.r / 127.5 - 1, c.g / 127.5 - 1, c.b / 127.5 - 1}; };
  const auto post = sample(b, nm, {1, 8.5, 2}, {0, 0, 1});
  const auto block = sample(b, nm, {10, 8.5, 8}, {0, 0, 1});
  REQUIRE(post);
  REQUIRE(block);
  CHECK(decode(*post).z > 0.99);   // radius limited to 1 px: flat at 1.5 px
  CHECK(decode(*block).z < 0.97);  // full 3 px radius: still bending at 1.5 px
}
