// Polished styles, step 6: styled meshes take their texture from the crisp one.
#include <doctest/doctest.h>

#include <map>
#include <memory>

#include "pixel3d/engine/session.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/style/rounding.hpp"
#include "pixel3d/texture/transfer.hpp"
#include "pixel3d/validate/metrics.hpp"

using namespace pixel3d;

namespace {

std::string fixture(const char* name, const char* view) {
  return std::string(PIXEL3D_FIXTURES_DIR) + "/" + name + "/" + view + ".png";
}

std::unique_ptr<Session> open(const char* fx) {
  auto s = std::make_unique<Session>();
  REQUIRE(s->new_project(ProjectSettings{}).success);
  REQUIRE(s->import_views({{ViewRole::Front, fixture(fx, "front"), {}},
                           {ViewRole::Side, fixture(fx, "side"), {}},
                           {ViewRole::Top, fixture(fx, "top"), {}}})
              .success);
  REQUIRE(s->validate(ValidateOptions{}).success);
  return s;
}

// Share of pixels, covered by both renders with all four neighbours covered too,
// whose colours are identical.
double interior_match(const RenderTarget& a, const RenderTarget& b) {
  int same = 0, n = 0;
  const int w = a.color.width(), h = a.color.height();
  auto both = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && a.coverage.get(x, y) && b.coverage.get(x, y); };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      if (!both(x, y) || !both(x - 1, y) || !both(x + 1, y) || !both(x, y - 1) || !both(x, y + 1)) continue;
      ++n;
      same += a.color.at(x, y) == b.color.at(x, y);
    }
  return n ? static_cast<double>(same) / n : 1.0;
}

}  // namespace

// Affine UV map of triangle t of a textured mesh applied to point p (through
// p's projection onto the triangle's plane).
vec2d uv_by_map(const TexturedMesh& m, size_t t, vec3d p) {
  const vec3d a = m.positions[m.indices[3 * t]], b = m.positions[m.indices[3 * t + 1]], c = m.positions[m.indices[3 * t + 2]];
  const vec3d v0 = b - a, v1 = c - a, v2 = p - a;
  const double d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1), d20 = dot(v2, v0), d21 = dot(v2, v1);
  const double den = d00 * d11 - d01 * d01, v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den;
  return m.uvs[m.indices[3 * t]] * (1 - v - w) + m.uvs[m.indices[3 * t + 1]] * v + m.uvs[m.indices[3 * t + 2]] * w;
}

TEST_CASE("texture transfer: crisp onto itself keeps every projected face; Edge Extend is one stretch per face") {
  for (const char* fx : {"A_synthetic_chair", "B_hand_chair", "C_asym_planter", "D_character"}) {
    const std::string name = fx;
    CAPTURE(name);
    auto s = open(fx);
    const Document& d = s->document();
    const Mesh& mesh = d.solid->mesh;
    const TextureResult t = transfer_textures(mesh, mesh, *d.texture, d.align->dims);
    REQUIRE(t.faces.size() == mesh.tris.size());
    std::map<int, size_t> first_of_region;
    for (size_t f = 0; f < mesh.tris.size(); ++f) {
      CHECK(t.faces[f].mode == d.texture->faces[f].mode);
      CHECK(t.faces[f].view == d.texture->faces[f].view);
      for (int k = 0; k < 3; ++k) {
        const vec2d got = t.mesh.uvs[t.mesh.indices[3 * f + k]];
        if (t.faces[f].mode == SurfaceMode::EdgeExtend) {
          // Every Edge Extend triangle of one planar face follows the same map.
          const size_t ref = first_of_region.try_emplace(mesh.tri_region[f], f).first->second;
          const vec2d want = uv_by_map(t.mesh, ref, mesh.positions[mesh.tris[f][k]]);
          CHECK(length(got - want) < 1e-7);
        } else {
          CHECK(length(got - d.texture->mesh.uvs[d.texture->mesh.indices[3 * f + k]]) < 1e-12);
        }
      }
    }
  }
}

TEST_CASE("texture transfer: soft and lowpoly look like crisp from every axis") {
  for (const char* fx : {"A_synthetic_chair", "B_hand_chair", "C_asym_planter", "D_character"}) {
    auto s = open(fx);
    const Document& d = s->document();
    for (double exponent : {2.0, 1.0}) {
      const std::string name = std::string(fx) + (exponent == 2 ? " soft" : " lowpoly");
      CAPTURE(name);
      CornerOptions o;
      o.exponent = exponent;
      o.radius = exponent == 2 ? 1.5 : 1.0;
      o.bevel = 0.2;
      const SolidResult solid = assemble_parts(round_parts(*d.edited_parts, o), d.cleanup_options);
      const TextureResult t = transfer_textures(solid.mesh, d.solid->mesh, *d.texture, d.align->dims);
      REQUIRE(t.faces.size() == solid.mesh.tris.size());
      // The drawn views: the styled model shows the same colours as crisp.
      for (ViewRole role : {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
        CAPTURE(to_string(role));
        const Camera cam = camera_for_view(role, d.align->dims, 4);
        const double m = interior_match(render(t.mesh, cam, RenderOptions{}), render(d.texture->mesh, cam, RenderOptions{}));
        CHECK(m >= 0.97);
      }
      // Undrawn surfaces never fall back to flat colour more than crisp does.
      auto flat_share = [](const TextureResult& r) {
        double total = 0;
        for (const auto& [mode, a] : r.area_by_mode) total += a;
        return total > 0 ? r.area_by_mode.at(SurfaceMode::FlatPalette) / total : 0.0;
      };
      CHECK(flat_share(t) <= flat_share(*d.texture) + 0.02);
      const QualityMetrics q = compute_metrics(t.mesh, solid.mesh, d.views, d.align->dims, *d.correspondence, t, 128,
                                               nullptr, false);
      for (const auto& [role, v] : q.views) {
        CAPTURE(to_string(role));
        CHECK(v.color_exact_fraction >= d.metrics->views.at(role).color_exact_fraction - 0.01);
      }
    }
  }
}
