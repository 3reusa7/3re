// pixel3d-fixturegen <fixtures_dir> [--only F_hooded_reference]
// Regenerates the committed test fixtures (--only writes just the named one):
//   A_synthetic_chair, C_asym_planter, C_asym_planter_bottom  (rendered from analytic ground truth)
//   B_hand_chair                                              (converted 1:1 from hand-authored text maps)
//   D_character                                               (converted 1:1 from hand-authored text maps)
//   E_lowpoly_character/{cloaked,humanoid}                    (analytic stylised characters + part-id maps)
//   F_hooded_reference                                        (1.2.0 input B: 3-view hooded character + per-part truth)
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "fixturegen/gt_model.hpp"
#include "fixturegen/gt_render.hpp"
#include "fixturegen/hand_maps.hpp"
#include "fixturegen/lowpoly_character.hpp"

namespace fs = std::filesystem;
using namespace pixel3d;
using namespace p3dfix;

namespace {

bool write_png(const ImageRGBA& img, const fs::path& p) {
  const Message m = save_png(img, p);
  if (m.code != "ok") std::cerr << m.text << "\n";
  return m.code == "ok";
}

bool write_gt(const GroundTruth& gt, const fs::path& dir, std::initializer_list<ViewRole> roles) {
  fs::create_directories(dir);
  bool ok = true;
  for (ViewRole r : roles) ok &= write_png(render_gt_view(gt, r), dir / (std::string(to_string(r)) + ".png"));
  nlohmann::json j;
  j["name"] = gt.name;
  j["dims"] = {gt.dims.W, gt.dims.H, gt.dims.D};
  for (const auto& part : gt.parts) {
    nlohmann::json planes = nlohmann::json::array();
    for (const auto& pl : part.planes) planes.push_back({pl.n.x, pl.n.y, pl.n.z, pl.d});
    j["parts"].push_back({{"name", part.name}, {"material", part.material}, {"planes", planes}});
  }
  j["volume"] = gt.volume(4);
  std::ofstream(dir / "gt.json") << j.dump(2) << "\n";
  return ok;
}

bool convert_hand(const fs::path& dir, const std::string& stem) {
  std::ifstream f(dir / (stem + ".txt"));
  if (!f) {
    std::cerr << "missing " << (dir / (stem + ".txt")).string() << "\n";
    return false;
  }
  std::stringstream ss;
  ss << f.rdbuf();
  const auto img = image_from_text_map(ss.str());
  if (!img) {
    std::cerr << stem << ": " << img.error().text << "\n";
    return false;
  }
  return write_png(*img, dir / (stem + ".png"));
}

bool write_character(const CharFixture& fx, const fs::path& dir) {
  fs::create_directories(dir);
  bool ok = write_png(fx.front.art, dir / "front.png") && write_png(fx.side.art, dir / "side.png") &&
            write_png(fx.front.parts, dir / "front_parts.png") && write_png(fx.side.parts, dir / "side_parts.png");
  if (fx.top.art.width() > 0)
    ok = ok && write_png(fx.top.art, dir / "top.png") && write_png(fx.top.parts, dir / "top_parts.png");
  nlohmann::json j;
  j["name"] = fx.name;
  j["dims"] = {fx.dims.W, fx.dims.H, fx.dims.D};
  for (const auto& p : fx.parts)
    j["parts"].push_back({{"id", p.id}, {"name", p.name}, {"type", p.type}, {"key", {p.key.r, p.key.g, p.key.b}}});
  std::ofstream(dir / "parts.json") << j.dump(2) << "\n";
  if (!fx.extents.empty()) {
    // Ground truth: per part bounding box in world px (x right, y up, z towards the
    // Front camera) and the volume it owns (first-wins priority).
    nlohmann::json g;
    g["name"] = fx.name;
    g["dims"] = {fx.dims.W, fx.dims.H, fx.dims.D};
    g["sampling_px"] = 0.25;
    for (size_t i = 0; i < fx.extents.size(); ++i) {
      const PartExtent& e = fx.extents[i];
      g["parts"].push_back({{"id", e.id},
                            {"name", fx.parts[i].name},
                            {"lo", {e.lo.x, e.lo.y, e.lo.z}},
                            {"hi", {e.hi.x, e.hi.y, e.hi.z}},
                            {"width", e.hi.x - e.lo.x},
                            {"height", e.hi.y - e.lo.y},
                            {"depth", e.hi.z - e.lo.z},
                            {"owned_volume", e.volume}});
    }
    std::ofstream(dir / "gt.json") << g.dump(2) << "\n";
  }
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const bool only = argc == 4 && std::string(argv[2]) == "--only";
  if (argc != 2 && !(only && std::string(argv[3]) == "F_hooded_reference")) {
    std::cerr << "usage: pixel3d-fixturegen <fixtures_dir> [--only F_hooded_reference]\n";
    return 1;
  }
  const fs::path root = argv[1];
  bool ok = true;
  if (only) {
    ok = write_character(make_hooded_reference(), root / "F_hooded_reference");
    std::cout << (ok ? "F_hooded_reference written to " : "FAILED writing F_hooded_reference to ") << root.string() << "\n";
    return ok ? 0 : 1;
  }
  ok &= write_gt(make_fixture_A(), root / "A_synthetic_chair", {ViewRole::Front, ViewRole::Side, ViewRole::Top});
  ok &= write_gt(make_fixture_C(), root / "C_asym_planter", {ViewRole::Front, ViewRole::Side, ViewRole::Top});
  ok &= write_gt(make_fixture_C(), root / "C_asym_planter_bottom",
                 {ViewRole::Front, ViewRole::Side, ViewRole::Top, ViewRole::Bottom});
  for (const char* stem : {"front", "side", "top", "front_1px_off"}) ok &= convert_hand(root / "B_hand_chair", stem);
  for (const char* stem : {"front", "side", "top"}) ok &= convert_hand(root / "D_character", stem);
  ok &= write_character(make_cloaked_character(), root / "E_lowpoly_character" / "cloaked");
  ok &= write_character(make_humanoid_character(), root / "E_lowpoly_character" / "humanoid");
  ok &= write_character(make_hooded_reference(), root / "F_hooded_reference");
  std::cout << (ok ? "fixtures written to " : "FAILED writing fixtures to ") << root.string() << "\n";
  return ok ? 0 : 1;
}
