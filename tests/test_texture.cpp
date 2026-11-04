#include <doctest/doctest.h>

#include <set>

#include "pipeline_helpers.hpp"
#include "pixel3d/reconstruct/true_solid.hpp"
#include "pixel3d/render/rasterizer.hpp"
#include "pixel3d/texture/atlas.hpp"
#include "pixel3d/texture/projector.hpp"

using namespace pixel3d;
using namespace p3dtest;

namespace {

MaterialFamilies families(const std::vector<SourceView>& views) {
  std::vector<const ImageRGBA*> imgs;
  for (const auto& v : views) imgs.push_back(v.aligned.get());
  return analyze_palette(imgs);
}

struct Textured {
  Corr corr;
  SolidResult solid;
  MaterialFamilies fam;
  TextureResult tex;
};

Textured texture_fixture(const std::string& name, const std::vector<TextureAssignment>& manual = {}) {
  Textured t{run_fixture(name), {}, {}, {}};
  t.solid = assemble_true_solid(t.corr.cands, {}, t.corr.dims, CleanupOptions{}, t.corr.part_of_candidate());
  t.fam = families(t.corr.views);
  t.tex = project_textures(t.solid.mesh, t.corr.views, t.fam, t.corr.dims, TextureOptions{}, manual);
  return t;
}

// Faces of the textured mesh whose geometric normal is close to `n`.
std::vector<size_t> faces_facing(const TextureResult& tr, vec3d n) {
  std::vector<size_t> out;
  for (size_t f = 0; f < tr.faces.size(); ++f) {
    const auto& m = tr.mesh;
    const vec3d a = m.positions[m.indices[3 * f]], b = m.positions[m.indices[3 * f + 1]], c = m.positions[m.indices[3 * f + 2]];
    if (dot(normalize(cross(b - a, c - a)), n) > 0.99) out.push_back(f);
  }
  return out;
}

}  // namespace

TEST_CASE("texture: atlas keeps source pixels byte-identical") {
  const Corr c = run_fixture("B_hand_chair");
  const Atlas atlas = build_atlas(c.views, families(c.views));
  for (const auto& v : c.views) {
    const AtlasCell* cell = atlas.cell(v.role);
    REQUIRE(cell);
    REQUIRE(cell->w == v.aligned->width());
    REQUIRE(cell->h == v.aligned->height());
    for (int y = 0; y < cell->h; ++y)
      for (int x = 0; x < cell->w; ++x)
        if (v.aligned->at(x, y).a >= 128) CHECK(atlas.image->at(cell->x0 + x, cell->y0 + y) == v.aligned->at(x, y));
  }
}

TEST_CASE("texture: gutter dilation prevents bleeding") {
  const Corr c = run_fixture("A_synthetic_chair");
  const Atlas atlas = build_atlas(c.views, families(c.views));
  const ImageRGBA& img = *atlas.image;
  int checked = 0;
  for (const auto& v : c.views) {
    const AtlasCell* cell = atlas.cell(v.role);
    for (int y = 0; y < cell->h; ++y)
      for (int x = 0; x < cell->w; ++x) {
        if (v.aligned->at(x, y).a < 128) continue;
        const int dx[] = {1, -1, 0, 0}, dy[] = {0, 0, 1, -1};
        for (int k = 0; k < 4; ++k) {
          const int ax = cell->x0 + x + dx[k], ay = cell->y0 + y + dy[k];
          if (x + dx[k] >= 0 && x + dx[k] < cell->w && y + dy[k] >= 0 && y + dy[k] < cell->h &&
              v.aligned->at(x + dx[k], y + dy[k]).a >= 128)
            continue;
          CHECK(img.at(ax, ay).a == 255);  // a neighbour outside the art is a dilated copy
          ++checked;
        }
      }
  }
  CHECK(checked > 100);
}

TEST_CASE("texture: atlas has a swatch per material family") {
  const Corr c = run_fixture("B_hand_chair");
  const MaterialFamilies fam = families(c.views);
  const Atlas atlas = build_atlas(c.views, fam);
  CHECK(static_cast<int>(atlas.swatch_uv.size()) == fam.family_count);
}

TEST_CASE("texture: A front re-render matches source colours exactly") {
  const Textured t = texture_fixture("A_synthetic_chair");
  for (ViewRole r : {ViewRole::Front, ViewRole::Side, ViewRole::Top}) {
    CAPTURE(to_string(r));
    const ImageRGBA& src = *[&] {
      for (const auto& v : t.corr.views)
        if (v.role == r) return v.aligned.get();
      return t.corr.views[0].aligned.get();
    }();
    const RenderTarget rt = render(t.tex.mesh, camera_for_view(r, t.corr.dims, 1), RenderOptions{});
    std::set<uint32_t> palette;
    for (const auto& v : t.corr.views)
      for (const Rgba8& p : v.aligned->pixels())
        if (p.a >= 128) palette.insert((uint32_t(p.r) << 16) | (uint32_t(p.g) << 8) | p.b);
    int both = 0, exact = 0, foreign = 0;
    for (int y = 0; y < src.height(); ++y)
      for (int x = 0; x < src.width(); ++x) {
        if (!rt.coverage.get(x, y)) continue;
        const Rgba8 got = rt.color.at(x, y);
        if (!palette.contains((uint32_t(got.r) << 16) | (uint32_t(got.g) << 8) | got.b)) ++foreign;
        if (src.at(x, y).a < 128) continue;
        ++both;
        exact += (got.r == src.at(x, y).r && got.g == src.at(x, y).g && got.b == src.at(x, y).b) ? 1 : 0;
      }
    CHECK(both > 0);
    // Front and Side see every face they render; on Top, A's reclined back is
    // textured from Front (the steeper view, cos 0.97 vs 0.24) by design.
    CHECK(static_cast<double>(exact) / both >= (r == ViewRole::Top ? 0.75 : 0.98));
    CHECK(foreign == 0);
  }
}

TEST_CASE("texture: A back faces are inferred with opposite projection") {
  const Textured t = texture_fixture("A_synthetic_chair");
  const auto back = faces_facing(t.tex, {0, 0, -1});
  REQUIRE_FALSE(back.empty());
  for (size_t f : back) {
    CHECK(t.tex.faces[f].inferred);
    CHECK(t.tex.faces[f].mode == SurfaceMode::OppositeProjection);
    CHECK(t.tex.faces[f].view == ViewRole::Front);
  }
  CHECK(t.tex.area_by_mode.at(SurfaceMode::Seen) > 0);
  bool reported = false;
  for (const auto& m : t.tex.messages) reported |= m.code == "inferred_surface";
  CHECK(reported);
}

TEST_CASE("texture: invalid opposite projection falls back to edge extend") {
  // A cube painted brown on Front/Top but green on its Side (left) view: the
  // hidden right face must not receive the green side art through the body.
  const Rgba8 brown{150, 100, 60, 255}, green{60, 160, 60, 255};
  const Dims d{6, 6, 6};
  Textured t{run_boxes({Box{0, 0, 0, 6, 6, 6, brown}}, d), {}, {}, {}};
  for (auto& v : t.corr.views)
    if (v.role == ViewRole::Side) v.aligned = std::make_shared<const ImageRGBA>(ImageRGBA(6, 6, green));
  t.solid = assemble_true_solid(t.corr.cands, {}, d, CleanupOptions{}, t.corr.part_of_candidate());
  t.fam = families(t.corr.views);
  t.tex = project_textures(t.solid.mesh, t.corr.views, t.fam, d, TextureOptions{}, {});
  const auto right = faces_facing(t.tex, {1, 0, 0});
  REQUIRE_FALSE(right.empty());
  for (size_t f : right) {
    CHECK(t.tex.faces[f].inferred);
    CHECK(t.tex.faces[f].mode == SurfaceMode::EdgeExtend);
  }
}

TEST_CASE("texture: uvs are pixel snapped on seen faces") {
  const Textured t = texture_fixture("B_hand_chair");
  const double w = t.tex.atlas.image->width(), h = t.tex.atlas.image->height();
  for (size_t f = 0; f < t.tex.faces.size(); ++f) {
    if (t.tex.faces[f].mode != SurfaceMode::Seen) continue;
    for (int k = 0; k < 3; ++k) {
      const vec2d uv = t.tex.mesh.uvs[t.tex.mesh.indices[3 * f + k]];
      // B is fully rectilinear: every UV lands on a texel corner.
      CHECK(std::abs(uv.x * w - std::round(uv.x * w)) < 1e-9);
      CHECK(std::abs(uv.y * h - std::round(uv.y * h)) < 1e-9);
    }
  }
}

TEST_CASE("texture: manual assignment overrides") {
  const Textured base = texture_fixture("A_synthetic_chair");
  const auto back = faces_facing(base.tex, {0, 0, -1});
  REQUIRE_FALSE(back.empty());
  // Faces of the textured mesh map 1:1 to solid triangles.
  const Textured t = texture_fixture("A_synthetic_chair", {TextureAssignment{{static_cast<int>(back[0])}, ViewRole::Top, SurfaceMode::FlatPalette, ""}});
  CHECK(t.tex.faces[back[0]].mode == SurfaceMode::Manual);
}
