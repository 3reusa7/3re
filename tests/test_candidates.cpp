#include <doctest/doctest.h>

#include <manifold/manifold.h>

#include "pixel3d/correspond/candidates.hpp"
#include "pixel3d/reconstruct/prism.hpp"
#include "pixel3d/reconstruct/volume.hpp"
#include "test_helpers.hpp"

using namespace pixel3d;
using p3dtest::Box;

namespace {

std::map<ViewRole, Mask2D> masks_of(const std::vector<SourceView>& views) {
  std::map<ViewRole, Mask2D> m;
  for (const auto& v : views) m.emplace(v.role, opaque_mask(*v.aligned));
  return m;
}

std::map<ViewRole, Mask2D> y_masks(const std::map<ViewRole, Mask2D>& all) {
  std::map<ViewRole, Mask2D> m;
  for (const auto& [r, mk] : all)
    if (r != ViewRole::Top && r != ViewRole::Bottom) m.emplace(r, mk);
  return m;
}

struct Built {
  p3dtest::Fixture fix;
  std::map<ViewRole, Mask2D> masks;
  BandSet bands;
  CandidateSet cands;
};

Built build(const std::string& name) {
  Built b{p3dtest::load_fixture(name), {}, {}, {}};
  b.masks = masks_of(b.fix.views);
  b.bands = compute_bands(y_masks(b.masks), b.fix.align.dims.H);
  const auto polys = build_view_polygons(b.fix.views, SimplifyOptions{}, 128);
  b.cands = build_candidates(b.bands, polys, b.fix.align.dims);
  return b;
}

int candidates_in_band_at(const Built& b, int y) {
  const Band* band = b.bands.band_containing(y);
  int n = 0;
  for (const auto& c : b.cands.cands) n += c.band == band->id ? 1 : 0;
  return n;
}

}  // namespace

TEST_CASE("candidates: cube from three squares has volume 27") {
  const Dims d{3, 3, 3};
  auto imgs = p3dtest::render_views({Box{0, 0, 0, 3, 3, 3}}, d);
  std::vector<SourceView> views;
  for (auto& [r, img] : imgs) {
    SourceView s;
    s.role = r;
    s.original = s.aligned = std::make_shared<const ImageRGBA>(img);
    views.push_back(s);
  }
  const auto masks = masks_of(views);
  const BandSet bs = compute_bands(y_masks(masks), 3);
  const CandidateSet cs = build_candidates(bs, build_view_polygons(views, SimplifyOptions{}, 128), d);
  REQUIRE(cs.cands.size() == 1);
  CHECK(cs.cands[0].volume == doctest::Approx(27));
  CHECK(cs.cands[0].solid.Status() == manifold::Manifold::Error::NoError);
  CHECK(voxelize_candidates(cs, masks, bs, d).count() == 27);
}

TEST_CASE("candidates: prism of a unit pixel spans the full depth") {
  const Dims d{4, 3, 5};
  const manifold::Manifold p = make_prism(ViewRole::Front, d, rect_polys(1, 0, 2, 1));
  // Front pixel (1,0) covers X∈[1,2], Y∈[2,3]; extruded through Z∈[-1, D+1].
  const auto bb = p.BoundingBox();
  CHECK(bb.min.x == doctest::Approx(1));
  CHECK(bb.max.x == doctest::Approx(2));
  CHECK(bb.min.y == doctest::Approx(2));
  CHECK(bb.max.y == doctest::Approx(3));
  CHECK(bb.min.z == doctest::Approx(-1));
  CHECK(bb.max.z == doctest::Approx(6));
  CHECK(p.Volume() == doctest::Approx(7));
}

TEST_CASE("candidates: union of all candidates equals visual hull") {
  for (const char* name : {"A_synthetic_chair", "B_hand_chair", "C_asym_planter"}) {
    CAPTURE(name);
    const Built b = build(name);
    const VoxelGrid hull = visual_hull(b.masks, b.fix.align.dims);
    const VoxelGrid un = voxelize_candidates(b.cands, b.masks, b.bands, b.fix.align.dims);
    CHECK(hull.count() > 0);
    CHECK(un == hull);
  }
}

TEST_CASE("candidates: solids agree with voxels at cell centres") {
  const Built b = build("A_synthetic_chair");
  std::vector<manifold::Manifold> solids;
  for (const auto& c : b.cands.cands) solids.push_back(c.solid);
  const manifold::Manifold all = manifold::Manifold::BatchBoolean(solids, manifold::OpType::Add);
  const VoxelGrid un = voxelize_candidates(b.cands, b.masks, b.bands, b.fix.align.dims);
  // Volume of the polygonal solid is within half a voxel layer of the voxel count.
  CHECK(std::abs(all.Volume() - un.count()) < 0.06 * un.count());
}

TEST_CASE("candidates: A leg band has 4 candidates") { CHECK(candidates_in_band_at(build("A_synthetic_chair"), 5) == 4); }

TEST_CASE("candidates: C leg band has 6 candidates") { CHECK(candidates_in_band_at(build("C_asym_planter"), 3) == 6); }

TEST_CASE("candidates: footprints use front and side atom columns") {
  const Built b = build("A_synthetic_chair");
  const Band* legs = b.bands.band_containing(5);
  for (const auto& c : b.cands.cands) {
    if (c.band != legs->id) continue;
    CHECK(c.footprint.width() == 2);
    CHECK(c.footprint.height() == 2);
  }
}

TEST_CASE("candidates: reclined back of A is sloped") {
  const Built b = build("A_synthetic_chair");
  bool sloped = false;
  for (const auto& c : b.cands.cands) {
    const manifold::MeshGL64 mesh = c.solid.GetMeshGL64();
    for (size_t t = 0; t + 2 < mesh.triVerts.size(); t += 3) {
      auto P = [&](size_t i) {
        const size_t v = mesh.triVerts[t + i] * mesh.numProp;
        return vec3d{mesh.vertProperties[v], mesh.vertProperties[v + 1], mesh.vertProperties[v + 2]};
      };
      const vec3d n = normalize(cross(P(1) - P(0), P(2) - P(0)));
      if (std::abs(n.y) > 0.1 && std::abs(n.z) > 0.1) sloped = true;
    }
  }
  CHECK(sloped);
}
