#include <doctest/doctest.h>

#include <cstring>

#include "pipeline_helpers.hpp"
#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/reconstruct/true_solid.hpp"
#include "pixel3d/texture/projector.hpp"

using namespace pixel3d;
using namespace p3dtest;

namespace {

struct Exported {
  Dims dims;
  TextureResult tex;
  std::vector<uint8_t> glb;
};

Exported export_fixture(const std::string& name, const ExportOptions& opt = {}) {
  const Corr c = run_fixture(name);
  const SolidResult solid = assemble_true_solid(c.cands, {}, c.dims, CleanupOptions{}, c.part_of_candidate());
  std::vector<const ImageRGBA*> imgs;
  for (const auto& v : c.views) imgs.push_back(v.aligned.get());
  Exported e{c.dims, project_textures(solid.mesh, c.views, analyze_palette(imgs), c.dims, TextureOptions{}, {}), {}};
  e.glb = write_glb(e.tex.mesh, opt);
  return e;
}

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
  uint32_t v;
  std::memcpy(&v, b.data() + at, 4);
  return v;
}

}  // namespace

TEST_CASE("glb: header magic version and length") {
  const Exported e = export_fixture("A_synthetic_chair");
  REQUIRE(e.glb.size() > 20);
  CHECK(u32(e.glb, 0) == 0x46546C67u);  // "glTF"
  CHECK(u32(e.glb, 4) == 2u);
  CHECK(u32(e.glb, 8) == e.glb.size());
  CHECK(u32(e.glb, 16) == 0x4E4F534Au);  // JSON chunk
  CHECK(u32(e.glb, 12) % 4 == 0);
  CHECK(e.glb.size() % 4 == 0);
}

TEST_CASE("glb: tinygltf reloads A with identical data") {
  const Exported e = export_fixture("A_synthetic_chair");
  const GlbCheck chk = check_glb_roundtrip(e.glb, e.tex.mesh, ExportOptions{}, e.dims);
  for (const auto& p : chk.problems) MESSAGE(p);
  CHECK(chk.ok);
  CHECK(chk.problems.empty());
  CHECK(chk.vertices == static_cast<int>(e.tex.mesh.positions.size()));
  CHECK(chk.triangles == static_cast<int>(e.tex.mesh.indices.size() / 3));
  CHECK(chk.image_w == e.tex.atlas.image->width());
  CHECK(chk.image_h == e.tex.atlas.image->height());
  CHECK(chk.rerender_identical);
}

TEST_CASE("glb: sampler is nearest and material opaque") {
  const Exported e = export_fixture("B_hand_chair");
  const GlbCheck chk = check_glb_roundtrip(e.glb, e.tex.mesh, ExportOptions{}, std::nullopt);
  CHECK(chk.ok);
  CHECK(chk.sampler_nearest);
  CHECK(chk.material_opaque);
}

TEST_CASE("glb: scale and bottom-centre origin follow the export options") {
  ExportOptions opt;
  opt.pixels_per_meter = 16;
  const Exported e = export_fixture("A_synthetic_chair", opt);
  const GlbCheck chk = check_glb_roundtrip(e.glb, e.tex.mesh, opt, std::nullopt);
  CHECK(chk.ok);
  CHECK(chk.bbox_min_y == doctest::Approx(0.0));
  CHECK(chk.bbox_height == doctest::Approx(33.0 / 16.0));
  CHECK(chk.bbox_center_x == doctest::Approx(0.0).epsilon(1e-6));
}

TEST_CASE("glb: corrupted byte is detected") {
  Exported e = export_fixture("A_synthetic_chair");
  e.glb[20] = 'x';  // first byte of the JSON chunk ('{')
  const GlbCheck chk = check_glb_roundtrip(e.glb, e.tex.mesh, ExportOptions{}, std::nullopt);
  CHECK_FALSE(chk.ok);
  CHECK_FALSE(chk.problems.empty());
}
