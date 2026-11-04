// 3re asset-quality tool (docs/agent/ASSET-QUALITY.md).
//   pixel3d-asset-quality hygiene FILE.glb [...]        G0 audit, one JSON line per file
//   pixel3d-asset-quality bake IN.glb OUT.glb           G0 repair: bake node transforms into the vertices
#include <cstdio>
#include <iostream>
#include <string>

#include "agent_hygiene.hpp"
#include "pixel3d/common/path_utf8.hpp"

using namespace p3dagent;
using nlohmann::json;

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::cerr << "usage: pixel3d-asset-quality hygiene FILE.glb [...] | bake IN.glb OUT.glb\n";
    return 2;
  }
  const std::wstring cmd = argv[1];
  constexpr double kMaxDisagree = 0.02;  // ASSET-QUALITY.md, G0 thresholds
  if (cmd == L"hygiene") {
    int worst = 0;
    for (int i = 2; i < argc; ++i) {
      json r = {{"file", pixel3d::to_utf8(fs::path(argv[i]))}};
      try {
        const auto a = audit_hygiene(read_glb(argv[i]));
        const auto v = judge_hygiene(a, kMaxDisagree);
        r["audit"] = a.to_json();
        r["status"] = v.status;
        r["reasons"] = v.reasons;
        if (v.status == "FAIL") worst = 1;
      } catch (const std::exception& e) {
        r["status"] = "FAIL";
        r["reasons"] = {std::string("unreadable: ") + e.what()};
        worst = 1;
      }
      std::cout << r.dump() << "\n";
    }
    return worst;
  }
  if (cmd == L"bake" && argc == 4) {
    try {
      std::string why;
      const auto g = bake_node_transforms(read_glb(argv[2]), &why);
      if (!g) {
        std::cerr << "cannot bake: " << why << "\n";
        return 1;
      }
      write_new_file(argv[3], encode_glb(*g));
      std::cout << json{{"status", judge_hygiene(audit_hygiene(*g), kMaxDisagree).status}}.dump() << "\n";
      return 0;
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      return 1;
    }
  }
  std::cerr << "unknown command\n";
  return 2;
}
