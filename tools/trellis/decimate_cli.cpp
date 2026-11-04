// pixel3d-decimate-guarded in.glb out.glb --target N [--max-threshold T] [--allow-touching]
//
// The shipped topology-preserving QEM with the exact contact guard (decimate_qem_guarded.cpp:
// a collapse that would make the surface cross or touch itself is refused) on a geometry GLB,
// for the asset agent's triangle budgets. The cleanup default (threshold ladder capped at 1e-5)
// keeps faces rather than erase detail; a game budget is an explicit request, so the agent passes
// a higher cap and this tool measures what it cost: the distance from every original vertex to the
// decimated surface and back (exact closest points, tri_bvh), in % of the original bounding-box
// diagonal. Writes out.glb (positions + indices, no material) and out.glb.decimate.json; refuses
// to overwrite either.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "agent/agent_glb.hpp"
#include "backends/mesh_asset.hpp"
#include "decimate_qem_guarded.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "tri_bvh.h"

namespace {

using nlohmann::json;

struct Deviation {
  double max_pct = 0, p99_pct = 0, mean_pct = 0;
};

// Distances from `from` vertices to the surface (verts, faces), as % of diag.
std::vector<double> distances(const std::vector<float>& from, const std::vector<float>& verts, const std::vector<int32_t>& faces, double diag) {
  const auto bvh = trellis::TriBvh::build(verts.data(), int64_t(verts.size() / 3), faces.data(), int64_t(faces.size() / 3));
  const size_t n = from.size() / 3;
  const size_t step = std::max<size_t>(1, n / 200000);  // at most ~200k samples
  std::vector<double> d;
  d.reserve(n / step + 1);
  for (size_t i = 0; i < n; i += step) {
    const auto hit = bvh.closest(&from[3 * i]);
    d.push_back(std::sqrt(double(hit.dist2)) / diag * 100.0);
  }
  return d;
}

Deviation summarize(std::vector<double> a, std::vector<double> b) {
  a.insert(a.end(), b.begin(), b.end());
  Deviation r;
  if (a.empty()) return r;
  std::sort(a.begin(), a.end());
  r.max_pct = a.back();
  r.p99_pct = a[std::min(a.size() - 1, size_t(std::ceil(0.99 * double(a.size()))) - 1)];
  double s = 0;
  for (double v : a) s += v;
  r.mean_pct = s / double(a.size());
  return r;
}

p3dagent::Glb geometry_glb(const std::vector<float>& v, const std::vector<int32_t>& f) {
  p3dagent::Glb g;
  const size_t vb = v.size() * 4, ib = f.size() * 4;
  g.bin.resize(vb + ib);
  std::memcpy(g.bin.data(), v.data(), vb);
  std::memcpy(g.bin.data() + vb, f.data(), ib);
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t i = 0; i < v.size(); ++i) {
    lo[i % 3] = std::min(lo[i % 3], v[i]);
    hi[i % 3] = std::max(hi[i % 3], v[i]);
  }
  json prim = {{"attributes", {{"POSITION", 0}}}, {"indices", 1}, {"mode", 4}};
  g.json = {{"asset", {{"version", "2.0"}, {"generator", "3re pixel3d-decimate-guarded"}}},
            {"scene", 0},
            {"scenes", json::array({json{{"nodes", {0}}}})},
            {"nodes", json::array({json{{"mesh", 0}, {"name", "decimated"}}})},
            {"meshes", json::array({json{{"primitives", json::array({prim})}}})},
            {"buffers", json::array({json{{"byteLength", g.bin.size()}}})},
            {"bufferViews", json::array({json{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", vb}, {"target", 34962}},
                                         json{{"buffer", 0}, {"byteOffset", vb}, {"byteLength", ib}, {"target", 34963}}})},
            {"accessors", json::array({json{{"bufferView", 0}, {"componentType", 5126}, {"count", v.size() / 3}, {"type", "VEC3"},
                                            {"min", {lo[0], lo[1], lo[2]}}, {"max", {hi[0], hi[1], hi[2]}}},
                                       json{{"bufferView", 1}, {"componentType", 5125}, {"count", f.size()}, {"type", "SCALAR"}}})}};
  return g;
}

int usage() {
  std::fprintf(stderr, "usage: pixel3d-decimate-guarded in.glb out.glb --target N [--max-threshold T] [--allow-touching]\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 5) return usage();
  const std::filesystem::path in = argv[1], out = argv[2];
  int target = 0;
  float max_threshold = 1e-5f;
  bool allow_touching = false, no_guard = false;
  for (int i = 3; i < argc; ++i) {
    const std::wstring a = argv[i];
    if (a == L"--target" && i + 1 < argc) target = std::stoi(argv[++i]);
    else if (a == L"--max-threshold" && i + 1 < argc) max_threshold = std::stof(argv[++i]);
    else if (a == L"--allow-touching") allow_touching = true;
    else if (a == L"--no-guard") no_guard = true;  // game targets: crossings allowed, reported by the audit
    else return usage();
  }
  if (target < 4 || !(max_threshold > 0) || (no_guard && allow_touching)) return usage();
  const auto report_path = std::filesystem::path(out).concat(L".decimate.json");
  if (std::filesystem::exists(out) || std::filesystem::exists(report_path)) {
    std::fprintf(stderr, "refusing to overwrite %s\n", pixel3d::to_utf8(out).c_str());
    return 4;
  }
  try {
    const auto t0 = std::chrono::steady_clock::now();
    const auto mesh = p3dasset::load_geometry_glb(in);
    // Indices exactly as stored: vertices at the same position are NOT welded. TRELLIS/GeometrySafe
    // meshes have separate sheets that touch (point/edge contacts); welding them would create
    // non-manifold edges. The input is a geometry GLB (no UV seams).
    std::vector<float> v;
    std::vector<int32_t> f;
    v.reserve(mesh.positions.size() * 3);
    for (const auto& p : mesh.positions) v.insert(v.end(), {float(p.x), float(p.y), float(p.z)});
    f.reserve(mesh.tris.size() * 3);
    for (const auto& t : mesh.tris) f.insert(f.end(), {int32_t(t[0]), int32_t(t[1]), int32_t(t[2])});
    double diag = 0;
    {
      float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
      for (size_t i = 0; i < v.size(); ++i) {
        lo[i % 3] = std::min(lo[i % 3], v[i]);
        hi[i % 3] = std::max(hi[i % 3], v[i]);
      }
      for (int k = 0; k < 3; ++k) diag += double(hi[k] - lo[k]) * double(hi[k] - lo[k]);
      diag = std::sqrt(diag);
    }
    if (!(diag > 0)) throw std::runtime_error("empty or flat mesh");
    std::vector<float> ov;
    std::vector<int32_t> of;
    p3dtrellis::GuardedQemOptions opt;
    opt.contact_guard = !no_guard;
    opt.max_threshold = max_threshold;
    opt.allow_touching = allow_touching;
    const auto rep = p3dtrellis::decimate_qem_guarded(v, f, target, ov, of, opt);
    if (of.empty()) throw std::runtime_error("decimation produced no faces");
    const auto dev = summarize(distances(v, ov, of, diag), distances(ov, v, f, diag));
    p3dagent::write_new_file(out, p3dagent::encode_glb(geometry_glb(ov, of)));
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const json report = {{"tool", "pixel3d-decimate-guarded"}, {"input", pixel3d::to_utf8(in)}, {"output", pixel3d::to_utf8(out)},
                         {"target", target}, {"max_threshold", max_threshold}, {"contact_guard", !no_guard}, {"allow_touching", allow_touching},
                         {"input_faces", f.size() / 3}, {"output_faces", of.size() / 3}, {"budget_met", rep.budget_met},
                         {"final_threshold", rep.final_threshold}, {"rounds", rep.rounds}, {"applied", rep.applied},
                         {"rejected", {{"contact", rep.rejected_contact}, {"star", rep.rejected_star}, {"link", rep.rejected_link},
                                       {"degenerate", rep.rejected_degenerate}, {"batch", rep.rejected_batch},
                                       {"coincident", rep.rejected_coincident}}},
                         {"deviation", {{"max_pct", dev.max_pct}, {"p99_pct", dev.p99_pct}, {"mean_pct", dev.mean_pct},
                                        {"measure", "symmetric vertex-to-surface distance, % of the original bbox diagonal"}}},
                         {"seconds", seconds}};
    const auto text = report.dump(1);
    p3dagent::write_new_file(report_path, std::vector<uint8_t>(text.begin(), text.end()));
    std::printf("decimated %zu -> %zu faces (target %d, budget %s), deviation p99 %.3f%% max %.3f%%, %.1f s\n", f.size() / 3,
                of.size() / 3, target, rep.budget_met ? "met" : "NOT met", dev.p99_pct, dev.max_pct, seconds);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pixel3d-decimate-guarded: %s\n", e.what());
    return 1;
  }
}
