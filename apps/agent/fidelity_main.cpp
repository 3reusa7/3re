// pixel3d-mesh-fidelity: does a reduced mesh look like its original (agent milestone 2, A1)?
//
//   pixel3d-mesh-fidelity ORIGINAL.glb REDUCED.glb OUT.json [--size N] [--samples N] [--no-colour]
//       [--max-distance-p95 PCT] [--max-distance-mean PCT] [--max-distance-max PCT] [--min-iou X] [--max-normal DEG] [--max-colour DE]
//   pixel3d-mesh-fidelity --render IN.glb OUT.png [--yaw DEG] [--elevation DEG] [--size N]
//
// Writes OUT.json (measures + PASS/FAIL with reasons, docs/agent/FIDELITY.md); never overwrites. Exit 0 = measured
// (whatever the verdict), 1 = could not measure, 2 = usage.
#include <cstdio>
#include <string>

#include "agent_fidelity.hpp"
#include "agent_glb.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/image/image.hpp"

using namespace p3dagent;

namespace {
int usage() {
  std::fprintf(stderr,
               "usage: pixel3d-mesh-fidelity ORIGINAL.glb REDUCED.glb OUT.json [--size N] [--samples N] [--no-colour]\n"
               "         [--max-distance-p95 PCT] [--max-distance-mean PCT] [--max-distance-max PCT] [--min-iou X] [--max-normal DEG] [--max-colour DE]\n"
               "       pixel3d-mesh-fidelity --render IN.glb OUT.png [--yaw DEG] [--elevation DEG] [--size N]\n");
  return 2;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 4) return usage();
  try {
    if (std::wstring(argv[1]) == L"--render") {
      double yaw = 0, el = 10;
      int size = 256;
      for (int i = 4; i + 1 < argc; i += 2) {
        const std::wstring a = argv[i];
        if (a == L"--yaw") yaw = std::stod(argv[i + 1]);
        else if (a == L"--elevation") el = std::stod(argv[i + 1]);
        else if (a == L"--size") size = std::stoi(argv[i + 1]);
        else return usage();
      }
      if (size < 16 || size > 4096) return usage();
      const auto px = render_unlit(fmesh_from_glb(read_glb(argv[2])), yaw, el, size);
      pixel3d::ImageRGBA img(size, size);
      for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
          const uint8_t* p = &px[(size_t(y) * size + x) * 4];
          img.at(x, y) = {p[0], p[1], p[2], p[3]};
        }
      write_new_file(argv[3], pixel3d::encode_png(img));
      return 0;
    }
    FidelityOptions opt;
    FidelityThresholds th;
    for (int i = 4; i < argc; ++i) {
      const std::wstring a = argv[i];
      auto next = [&]() -> double {
        if (i + 1 >= argc) throw std::invalid_argument("missing value");
        return std::stod(argv[++i]);
      };
      if (a == L"--size") opt.size = int(next());
      else if (a == L"--samples") opt.samples = int(next());
      else if (a == L"--no-colour") opt.colour = false;
      else if (a == L"--max-distance-p95") th.max_distance_p95_pct = next();
      else if (a == L"--max-distance-mean") th.max_distance_mean_pct = next();
      else if (a == L"--max-distance-max") th.max_distance_max_pct = next();
      else if (a == L"--min-iou") th.min_silhouette_iou = next();
      else if (a == L"--max-normal") th.max_normal_mean_deg = next();
      else if (a == L"--max-colour") th.max_colour_mean_de = next();
      else return usage();
    }
    if (opt.size < 32 || opt.size > 2048 || opt.samples < 1000 || opt.samples > 2000000) return usage();
    const auto f = measure_fidelity(fmesh_from_glb(read_glb(argv[1])), fmesh_from_glb(read_glb(argv[2])), opt);
    auto j = f.to_json();
    j["judgement"] = judge_fidelity(f, th);
    j["original"] = pixel3d::to_utf8(argv[1]);
    j["reduced"] = pixel3d::to_utf8(argv[2]);
    const auto text = j.dump(1);
    write_new_file(argv[3], std::vector<uint8_t>(text.begin(), text.end()));
    std::printf("fidelity %s: distance p95 %.3f%% mean %.3f%%, IoU min %.4f, normals %.2f deg, colour %s\n",
                j["judgement"]["status"].get<std::string>().c_str(), f.distance_p95_pct, f.distance_mean_pct, f.silhouette_iou_min,
                f.normal_mean_deg, f.colour_mean_de < 0 ? "not measured" : std::to_string(f.colour_mean_de).c_str());
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pixel3d-mesh-fidelity: %s\n", e.what());
    return 1;
  }
}
