// pixel3d-decimate-gltf IN.glb OUT.glb --target N — UV-preserving multi-mesh decimation of a foreign GLB (agent
// milestone 2, A3). Materials, images, nodes and vertex data are kept; new index buffers only. Writes OUT.glb and
// OUT.glb.decimate.json; never overwrites. Exit 0 = written (budget met or not, see the report), 1 = error, 2 = usage.
#include <cstdio>
#include <string>

#include "agent_glb.hpp"
#include "agent_gltf_decimate.hpp"
#include "pixel3d/common/path_utf8.hpp"

using namespace p3dagent;

int wmain(int argc, wchar_t** argv) {
  if (argc != 5 || std::wstring(argv[3]) != L"--target") {
    std::fprintf(stderr, "usage: pixel3d-decimate-gltf IN.glb OUT.glb --target N\n");
    return 2;
  }
  try {
    const long long target = std::stoll(argv[4]);
    if (target < 4) throw std::invalid_argument("target must be >= 4");
    const fs::path out = argv[2];
    const auto rep_path = fs::path(out).concat(L".decimate.json");
    if (fs::exists(out) || fs::exists(rep_path)) throw std::runtime_error("refusing to overwrite " + pixel3d::to_utf8(out));
    auto r = decimate_gltf(read_glb(argv[1]), target);
    write_new_file(out, encode_glb(r.glb));
    r.report["input"] = pixel3d::to_utf8(argv[1]);
    r.report["output"] = pixel3d::to_utf8(out);
    const auto text = r.report.dump(1);
    write_new_file(rep_path, std::vector<uint8_t>(text.begin(), text.end()));
    std::printf("decimated %lld -> %lld triangles (target %lld, budget %s)\n", (long long)r.triangles_before, (long long)r.triangles_after,
                target, r.budget_met ? "met" : "NOT met");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pixel3d-decimate-gltf: %s\n", e.what());
    return 1;
  }
}
