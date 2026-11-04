#include <doctest/doctest.h>

#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"

using namespace pixel3d;

namespace {

Mesh cube(vec3d lo, vec3d hi) {
  Mesh m;
  for (int i = 0; i < 8; ++i)
    m.positions.push_back({(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z});
  const uint32_t f[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                             {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
  for (const auto& t : f) m.tris.push_back({t[0], t[1], t[2]});
  m.tri_part.assign(12, 0);
  return m;
}

// Unit-depth quad in the plane z = zc facing +Z, covering [x0,x1]×[y0,y1].
void add_quad(Mesh& m, double x0, double y0, double x1, double y1, double zc) {
  const uint32_t b = static_cast<uint32_t>(m.positions.size());
  m.positions.push_back({x0, y0, zc});
  m.positions.push_back({x1, y0, zc});
  m.positions.push_back({x1, y1, zc});
  m.positions.push_back({x0, y1, zc});
  m.tris.push_back({b, b + 1, b + 2});
  m.tris.push_back({b, b + 2, b + 3});
  m.tri_part.push_back(0);
  m.tri_part.push_back(0);
}

}  // namespace

TEST_CASE("render: unit cube front coverage is exact") {
  const Dims d{3, 3, 3};
  const Camera cam = camera_for_view(ViewRole::Front, d, 1);
  CHECK(cam.width == 3);
  CHECK(cam.height == 3);
  const RenderTarget rt = render_mesh_flat(cube({0, 0, 0}, {3, 3, 3}), cam);
  CHECK(rt.coverage.count() == 9);
  CHECK(rt.depth[0] == doctest::Approx(0.0));  // front face sits at depth 0
}

TEST_CASE("render: view camera maps a voxel to its source pixel") {
  const Dims d{4, 5, 3};
  for (ViewRole r : kAllViewRoles) {
    const Camera cam = camera_for_view(r, d, 1);
    const RenderTarget rt = render_mesh_flat(cube({1, 2, 0}, {2, 3, 1}), cam);
    const vec3d uvd = view_from_world(r, d, {1.5, 2.5, 0.5});
    CAPTURE(to_string(r));
    CHECK(rt.coverage.count() == 1);
    CHECK(rt.coverage.get(static_cast<int>(uvd.x), static_cast<int>(uvd.y)));
  }
}

TEST_CASE("render: depth test picks nearest") {
  Mesh m;
  add_quad(m, 0, 0, 4, 4, 1);  // far (depth from +Z camera larger)
  add_quad(m, 1, 1, 3, 3, 3);  // near
  const RenderTarget rt = render_mesh_flat(m, camera_for_view(ViewRole::Front, Dims{4, 4, 4}, 1));
  CHECK(rt.tri_id[1 * 4 + 1] >= 2);  // centre pixels belong to the near quad
  CHECK(rt.tri_id[0] <= 1);
}

TEST_CASE("render: shared edge has no gaps or double coverage") {
  Mesh quad;
  add_quad(quad, 0.3, 0.1, 7.7, 6.9, 1);
  const Camera cam = camera_for_view(ViewRole::Front, Dims{8, 7, 2}, 4);
  Mesh t0 = quad, t1 = quad;
  t0.tris = {quad.tris[0]};
  t1.tris = {quad.tris[1]};
  t0.tri_part = t1.tri_part = {0};
  const RenderTarget a = render_mesh_flat(t0, cam), b = render_mesh_flat(t1, cam), both = render_mesh_flat(quad, cam);
  int overlap = 0;
  for (int y = 0; y < cam.height; ++y)
    for (int x = 0; x < cam.width; ++x) {
      overlap += (a.coverage.get(x, y) && b.coverage.get(x, y)) ? 1 : 0;
      CHECK(both.coverage.get(x, y) == (a.coverage.get(x, y) || b.coverage.get(x, y)));
    }
  CHECK(overlap == 0);
  CHECK(a.coverage.count() + b.coverage.count() == both.coverage.count());
}

TEST_CASE("render: nearest sampling exact") {
  ImageRGBA checker(4, 4);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) checker.at(x, y) = ((x + y) % 2) ? Rgba8{255, 0, 0, 255} : Rgba8{0, 0, 255, 255};
  TexturedMesh tm;
  tm.positions = {{0, 0, 1}, {4, 0, 1}, {4, 4, 1}, {0, 4, 1}};
  tm.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
  tm.uvs = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};  // glTF: v down, image row 0 at the top (world y = 4)
  tm.indices = {0, 1, 2, 0, 2, 3};
  tm.texture = std::make_shared<const ImageRGBA>(checker);
  tm.tri_part = {0, 0};
  const RenderTarget rt = render(tm, camera_for_view(ViewRole::Front, Dims{4, 4, 2}, 4), RenderOptions{});
  REQUIRE(rt.color.width() == 16);
  for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 16; ++x) CHECK(rt.color.at(x, y) == checker.at(x / 4, y / 4));
}

TEST_CASE("render: orbit camera at yaw 90 looks from +X") {
  const Dims d{2, 2, 6};
  const Camera cam = camera_orbit(d, 90, 0, 64, 64);
  CHECK(cam.forward.x == doctest::Approx(-1));
  // The box is 6 deep in Z: from +X it is wider than it is from the front.
  const RenderTarget side = render_mesh_flat(cube({0, 0, 0}, {2, 2, 6}), cam);
  const RenderTarget front = render_mesh_flat(cube({0, 0, 0}, {2, 2, 6}), camera_orbit(d, 0, 0, 64, 64));
  CHECK(side.coverage.bbox().width() > 2 * front.coverage.bbox().width());
}
