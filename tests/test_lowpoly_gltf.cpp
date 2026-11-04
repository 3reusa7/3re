#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "lowpoly_helpers.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/lowpoly/build.hpp"
#include "pixel3d/lowpoly/skinned_gltf.hpp"

using namespace pixel3d;
using namespace pixel3d::lowpoly;
using namespace p3dtest;
namespace fs = std::filesystem;

namespace {

struct Exported {
  Result r;
  SkinnedExportOptions opt;
  std::vector<uint8_t> glb;
};

Exported export_fixture(const std::string& variant, int budget, bool side, bool hull, Style style = Style::CelOutline) {
  Input in = load_lowpoly_fixture(variant, side, side ? 0 : 20);
  in.settings.tri_budget = budget;
  in.settings.hull = hull;
  in.settings.style = style;
  auto r = build_lowpoly(in);
  REQUIRE(r);
  Exported e{std::move(*r), {}, {}};
  e.opt.style = style;
  e.opt.frame.pixels_per_meter = 64;
  e.opt.report = {{"budget", budget}, {"triangles", e.r.triangles}};
  e.glb = write_skinned_glb(e.r.mesh(), e.r.layout.skeleton, *e.r.bake.texture, e.opt);
  return e;
}

}  // namespace

TEST_CASE("lowpoly glTF: skinned GLB round-trips through tinygltf exactly") {
  for (const char* variant : {"cloaked", "humanoid"})
    for (bool side : {true, false})
      for (bool hull : {false, true}) {
        CAPTURE(variant);
        CAPTURE(side);
        CAPTURE(hull);
        const Exported e = export_fixture(variant, 1000, side, hull);
        const SkinnedCheck c = check_skinned_glb(e.glb, e.r.mesh(), e.r.layout.skeleton, e.opt);
        INFO(to_json(c).dump());
        CHECK(c.ok);
        CHECK(c.triangles == e.r.triangles);
        CHECK(c.triangles <= 1000);
        CHECK(c.joints == static_cast<int>(e.r.layout.skeleton.bones.size()));
        CHECK(c.max_weight_sum_error < 1e-6);
      }
}

TEST_CASE("lowpoly glTF: front −Z export rotates the skeleton with the mesh") {
  Exported e = export_fixture("humanoid", 900, true, false);
  e.opt.frame.front_negative_z = true;
  e.glb = write_skinned_glb(e.r.mesh(), e.r.layout.skeleton, *e.r.bake.texture, e.opt);
  const SkinnedCheck c = check_skinned_glb(e.glb, e.r.mesh(), e.r.layout.skeleton, e.opt);
  INFO(to_json(c).dump());
  CHECK(c.ok);
}

TEST_CASE("lowpoly glTF: the Khronos glTF-Validator reports no errors") {
#ifndef PIXEL3D_GLTF_VALIDATOR
  FAIL("glTF-Validator not configured: run tools/fetch_gltf_validator.ps1 and re-run CMake");
#else
  const fs::path dir = fs::path(PIXEL3D_TEST_OUT_DIR);
  fs::create_directories(dir);
  int n = 0;
  for (const char* variant : {"cloaked", "humanoid"})
    for (bool hull : {false, true})
      for (Style style : {Style::CelOutline, Style::Flat}) {
        CAPTURE(variant);
        CAPTURE(hull);
        const Exported e = export_fixture(variant, 1000, true, hull, style);
        const fs::path glb = dir / ("validator_" + std::to_string(n++) + ".glb");
        const fs::path rep = fs::path(glb).replace_extension(".report.json");
        std::ofstream(glb, std::ios::binary).write(reinterpret_cast<const char*>(e.glb.data()), static_cast<std::streamsize>(e.glb.size()));
        fs::remove(rep);
        const std::string cmd = "\"\"" + std::string(PIXEL3D_GLTF_VALIDATOR) + "\" -o \"" + glb.string() + "\" > \"" + rep.string() + "\" 2>NUL\"";
        std::system(cmd.c_str());
        std::ifstream f(rep);
        REQUIRE(f);
        const auto j = nlohmann::json::parse(f);
        INFO(j["issues"].dump(1));
        CHECK(j["issues"]["numErrors"].get<int>() == 0);
        CHECK(j["issues"]["numWarnings"].get<int>() == 0);
        CHECK(j["info"]["animationCount"].get<int>() == 0);
        CHECK(j["info"]["totalTriangleCount"].get<int>() == e.r.triangles);
      }
#endif
}
