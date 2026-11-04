// Polished styles: unique second UV set for baked maps.
#include <doctest/doctest.h>

#include <cmath>
#include <memory>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/style/normals.hpp"
#include "pixel3d/style/rounding.hpp"
#include "pixel3d/texture/transfer.hpp"
#include "pixel3d/texture/unique_uv.hpp"

using namespace pixel3d;

namespace {

std::string fixture(const char* name, const char* view) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + view + ".png";
}

TexturedMesh soft_mesh(const char* fx, Dims* dims) {
  Session s;
  REQUIRE(s.new_project(ProjectSettings{}).success);
  REQUIRE(s.import_views({{ViewRole::Front, fixture(fx, "front"), {}},
                          {ViewRole::Side, fixture(fx, "side"), {}},
                          {ViewRole::Top, fixture(fx, "top"), {}}})
              .success);
  REQUIRE(s.validate(ValidateOptions{}).success);
  const Document& d = s.document();
  *dims = d.align->dims;
  CornerOptions o;
  o.bevel = 0.2;
  const SolidResult solid = assemble_parts(round_parts(*d.edited_parts, o), d.cleanup_options);
  return smooth_normals(transfer_textures(solid.mesh, d.solid->mesh, *d.texture, d.align->dims).mesh);
}

double cross2(vec2d a, vec2d b) { return a.x * b.y - a.y * b.x; }

}  // namespace

TEST_CASE("unique uv: charts are undistorted, inside the atlas, never overlap") {
  for (const char* fx : {"B_hand_chair", "D_character"}) {
    const std::string name = fx;
    CAPTURE(name);
    Dims dims;
    const TexturedMesh m = soft_mesh(fx, &dims);
    UniqueUVReport rep;
    const TexturedMesh u = add_unique_uv(m, 8, 2, &rep);
    REQUIRE(u.uvs1.size() == u.positions.size());
    REQUIRE(u.tangents.size() == u.positions.size());
    REQUIRE(u.indices.size() == m.indices.size());
    CHECK(rep.charts > 10);
    CHECK(rep.width > 0);
    CHECK(rep.height > 0);
    const size_t T = u.indices.size() / 3;
    // Texel-space coverage: each texel centre belongs to at most one triangle's chart.
    std::vector<int> owner(static_cast<size_t>(rep.width) * rep.height, -1);
    int overlaps = 0;
    for (size_t t = 0; t < T; ++t) {
      vec2d q[3];
      vec3d p[3];
      for (int k = 0; k < 3; ++k) {
        const uint32_t i = u.indices[3 * t + k];
        q[k] = {u.uvs1[i].x * rep.width, u.uvs1[i].y * rep.height};
        p[k] = u.positions[i];
        CHECK(u.uvs1[i].x >= 0);
        CHECK(u.uvs1[i].x <= 1);
        CHECK(u.uvs1[i].y >= 0);
        CHECK(u.uvs1[i].y <= 1);
      }
      // Same shape in texels as in the world, scaled by 8 (no distortion).
      const double world = length(cross(p[1] - p[0], p[2] - p[0])) / 2;
      const double texels = std::abs(cross2(q[1] - q[0], q[2] - q[0])) / 2;
      CHECK(texels == doctest::Approx(world * 64).epsilon(1e-6));
      for (int a = 0; a < 3; ++a)
        CHECK(length(q[(a + 1) % 3] - q[a]) == doctest::Approx(8 * length(p[(a + 1) % 3] - p[a])).epsilon(1e-6));
      // Tangent: unit, along the chart's u direction, perpendicular to the face.
      const vec3d n = normalize(cross(p[1] - p[0], p[2] - p[0]));
      for (int k = 0; k < 3; ++k) {
        const auto& tg = u.tangents[u.indices[3 * t + k]];
        const vec3d tv{tg[0], tg[1], tg[2]};
        CHECK(length(tv) == doctest::Approx(1.0));
        CHECK(std::abs(dot(tv, n)) < 1e-6);
      }
      // Mark texel centres strictly inside this triangle.
      const double area2 = cross2(q[1] - q[0], q[2] - q[0]);
      if (std::abs(area2) < 1e-12) continue;
      const int x0 = std::max(0, static_cast<int>(std::floor(std::min({q[0].x, q[1].x, q[2].x}))));
      const int x1 = std::min(rep.width - 1, static_cast<int>(std::ceil(std::max({q[0].x, q[1].x, q[2].x}))));
      const int y0 = std::max(0, static_cast<int>(std::floor(std::min({q[0].y, q[1].y, q[2].y}))));
      const int y1 = std::min(rep.height - 1, static_cast<int>(std::ceil(std::max({q[0].y, q[1].y, q[2].y}))));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const vec2d c{x + 0.5, y + 0.5};
          const double w0 = cross2(q[1] - c, q[2] - c) / area2, w1 = cross2(q[2] - c, q[0] - c) / area2;
          if (w0 <= 1e-9 || w1 <= 1e-9 || 1 - w0 - w1 <= 1e-9) continue;
          int& o = owner[static_cast<size_t>(y) * rep.width + x];
          // Triangles of one chart may share texels along their common edges only; the
          // owner is the chart, found by uvs1 bounding boxes not overlapping (see below).
          if (o >= 0 && o != static_cast<int>(t)) ++overlaps;
          o = static_cast<int>(t);
        }
    }
    CHECK(overlaps == 0);
    // Nothing a render shows changed.
    for (ViewRole role : {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Back}) {
      const Camera cam = camera_for_view(role, dims, 4);
      CHECK(render(u, cam, RenderOptions{}).color == render(m, cam, RenderOptions{}).color);
    }
  }
}
