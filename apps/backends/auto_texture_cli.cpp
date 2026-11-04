// pixel3d-auto-texture: the Auto Texture worker. Run by the 3re backend after GeometrySafe and the
// geometry audit (and for retexturing a saved GLB), or by hand for batch retexturing:
//
//   pixel3d-auto-texture <geometry.glb> <source.png> <output.glb> [--crisp] [--max-size 1024|2048]
//                        [--seconds N] [--views DIR] [--compare other.glb]... [--native run/native.post]
//
// --view FILE LABEL (repeatable, up to 5): another picture of the same object (multi-angle import, 2.3).
// LABEL = front_left | left | back_left | back | back_right | right | front_right | auto, optionally ":YAW"
// (degrees from the front, overriding the label's angle), or just a number. Each picture is registered to the
// mesh around its angle; rejected pictures are listed with the reason in the report ("views").
// --native: TRELLIS's own texture volume from the same generation (hybrid: the picture owns what it shows,
// the calibrated TRELLIS texture fills everything else; spec 2026-10-20-hybrid-texture-design).
//
// Writes <output.glb> (only when the auto texture is the selected result), <output>.texture.json
// (always: registration, every attempt, every candidate's quality checks, limitations),
// <output>.base-color.png and <output>.provenance.png. Exit 0 = auto texture written,
// 3 = the GLB's own material was better and is kept, 2 = no usable texture (report says why).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include "auto_texture.hpp"
#include "backends/mesh_asset.hpp"
#include "pixel3d/common/path_utf8.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
using namespace p3dasset;
using namespace p3dasset::autotex;

namespace {
double available_ram_mib() {
#ifdef _WIN32
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof(m);
  if (GlobalMemoryStatusEx(&m)) return double(m.ullAvailPhys) / 1048576.0;
#endif
  return 0;
}
double peak_working_set_mib() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS c{};
  c.cb = sizeof(c);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c))) return c.PeakWorkingSetSize / 1048576.0;
#endif
  return 0;
}
int rank(const nlohmann::json& q) {
  const auto v = q.value("verdict", std::string("FAIL"));
  return v == "PASS" ? 2 : v == "REVIEW" ? 1 : 0;
}
bool better(const nlohmann::json& a, const nlohmann::json& b) {  // a better than b
  if (b.is_null()) return true;
  if (rank(a) != rank(b)) return rank(a) > rank(b);
  return a.value("score", 0.0) > b.value("score", 0.0) + 1e-9;
}
void write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
  if (fs::exists(path)) throw std::runtime_error("Refusing to overwrite " + pixel3d::to_utf8(path));
  const fs::path tmp = fs::path(path).concat(L".partial");
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) throw std::runtime_error("Could not write " + pixel3d::to_utf8(path));
  }
  fs::rename(tmp, path);
}
void write_json(const fs::path& path, const nlohmann::json& j) {
  const auto s = j.dump(2);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(s.data(), std::streamsize(s.size()));
}
void write_png(const pixel3d::ImageRGBA& image, const fs::path& path) {
  write_file(path, pixel3d::encode_png(image));
}
// Contact sheet: front, 3/4, side, back, other side at a slight elevation, from the picture's camera.
pixel3d::ImageRGBA sheet(const pixel3d::TexturedMesh& m, int size, double front_yaw) {
  const double yaws[] = {front_yaw, front_yaw + 45, front_yaw + 90, front_yaw + 180, front_yaw + 270};
  pixel3d::ImageRGBA out(size * 5, size);
  for (int i = 0; i < 5; ++i) {
    const auto v = render_view(m, yaws[i], 10, size);
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) out.at(i * size + x, y) = v.at(x, y);
  }
  return out;
}
}  // namespace

struct MultiViewArgs {
  std::optional<fs::path> export_dir, views_dir;
  double strength = 1.0;
  bool material_lock = true;
  bool material_match = true;
  double grazing_cos0 = 0.40, grazing_cos1 = 0.75;  // picture fade-out on steep surfaces (hybrid only)
  double streak_radius = 0.008;                     // thin-streak removal in the material lock (x diagonal; 0 = off)
  double density_ratio = 2.0;                       // atlas density of detailed picture surface vs hidden (1 = uniform)
  std::vector<std::pair<fs::path, std::string>> views;  // extra pictures: file, label
};
int run(const std::vector<fs::path>& positional, bool crisp, int max_size, double seconds, std::optional<fs::path> views,
        const std::vector<fs::path>& compare, std::optional<fs::path> native_post, const MultiViewArgs& mva) {
  const auto started = std::chrono::steady_clock::now();
  const fs::path geometry = positional[0], source_file = positional[1], output = positional[2];
  const fs::path report_path = fs::path(output).concat(L".texture.json");
  nlohmann::json report{{"schema", 1}, {"engine", "3re Auto Texture 1"}, {"geometry", pixel3d::to_utf8(geometry)},
                        {"source", pixel3d::to_utf8(source_file)}, {"output", pixel3d::to_utf8(output)}, {"geometry_changed", false}};
  nlohmann::json limitations = nlohmann::json::array();
  try {
    if (fs::exists(output) || fs::exists(report_path)) throw std::runtime_error("Output already exists; choose a new file");
    if (output.has_parent_path()) fs::create_directories(output.parent_path());
    const auto logical = load_geometry_glb(geometry);
    report["mesh"] = {{"vertices", logical.positions.size()}, {"triangles", logical.tris.size()}};
    auto image = pixel3d::load_png(source_file);
    if (!image) throw std::runtime_error("Could not read the source picture: " + image.error().text);
    const Source src = Source::from(*image, crisp);
    report["source_size"] = {src.width, src.height};
    const Deadline deadline = Deadline::in_seconds(seconds);
    const Registration reg = register_source(logical, src, deadline);
    report["registration"] = reg.evidence;
    // Extra pictures from other angles: registered around their labelled angle, checked, harmonised.
    std::vector<Source> extra_sources;
    std::vector<ExtraInput> extra_inputs;
    nlohmann::json view_load = nlohmann::json::array();
    extra_sources.reserve(mva.views.size());
    for (const auto& [file, label_arg] : mva.views) {
      ExtraInput in;
      in.name = pixel3d::to_utf8(file.filename());
      in.label = label_arg;
      if (const auto colon = label_arg.find(':'); colon != std::string::npos) {
        in.label = label_arg.substr(0, colon);
        in.yaw = std::stod(label_arg.substr(colon + 1));
        in.yaw_given = true;
      } else if (!label_arg.empty() && (std::isdigit(static_cast<unsigned char>(label_arg[0])) || label_arg[0] == '-')) {
        in.label = "custom";
        in.yaw = std::stod(label_arg);
        in.yaw_given = true;
      }
      auto pic = pixel3d::load_png(file);
      if (pic) {
        try {
          extra_sources.push_back(Source::from(*pic, crisp));
          in.source = &extra_sources.back();
        } catch (const std::exception& e) {
          view_load.push_back({{"name", in.name}, {"error", e.what()}});
        }
      }
      extra_inputs.push_back(in);
    }
    std::vector<ExtraView> extra;
    if (!extra_inputs.empty()) {
      nlohmann::json vr;
      extra = register_extra_views(logical, src, reg, extra_inputs, deadline, vr);
      report["views"] = vr;
      if (!view_load.empty()) report["views_load_errors"] = view_load;
      for (const auto& v : vr)
        if (v.value("status", std::string()) != "accepted")
          limitations.push_back("The " + v.value("label", std::string("extra")) + " picture (" + v.value("name", std::string()) +
                                ") was not used: " + v.value("reason", std::string("unknown reason")) + ".");
    }
    const std::vector<ExtraView>* extra_ptr = extra.empty() ? nullptr : &extra;
    // Multi-view: write the control images for the generator and stop (no texture yet).
    if (mva.export_dir) {
      report["multiview_controls"] = MultiViewSet::export_controls(logical, reg, pixel3d::to_utf8(*mva.export_dir));
      report["status"] = "CONTROLS";
      report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
      write_json(report_path, report);
      std::cout << "MULTIVIEW_CONTROLS " << pixel3d::to_utf8(*mva.export_dir) << std::endl;
      return 0;
    }
    // TRELLIS native texture volume: a load problem is reported and the picture-only reconstruction is used.
    std::optional<NativeVolume> native;
    if (native_post) {
      try {
        native = NativeVolume::load_post(pixel3d::to_utf8(*native_post));
        report["native_texture"] = {{"file", pixel3d::to_utf8(*native_post)}, {"loaded", true}, {"volume", native->evidence}};
      } catch (const std::exception& e) {
        report["native_texture"] = {{"file", pixel3d::to_utf8(*native_post)}, {"loaded", false}, {"error", e.what()}};
        limitations.push_back(std::string("TRELLIS's own texture could not be used (") + e.what() + "); hidden surfaces are reconstructed from the picture.");
      }
    }
    std::optional<MultiViewSet> multiview;
    if (mva.views_dir && native && native->translucent) {
      // Glass: the generated views paint opaque, darker glass with seams between views (regression bowl);
      // TRELLIS's translucent volume is kept alone.
      report["multiview"] = {{"loaded", false}, {"skipped", "translucent object (glass): TRELLIS's own texture and opacity are used"}};
    } else if (mva.views_dir) {
      try {
        multiview = MultiViewSet::load(logical, reg, pixel3d::to_utf8(*mva.views_dir));
        report["multiview"] = {{"loaded", true}, {"evidence", multiview->evidence}};
      } catch (const std::exception& e) {
        report["multiview"] = {{"loaded", false}, {"error", e.what()}};
        limitations.push_back(std::string("The generated views could not be used (") + e.what() + ").");
      }
    }
    // Atlas size from the picture's pixel density on the surface (no upscaled blur, no waste).
    double area = 0;
    for (const auto& t : logical.tris)
      area += pixel3d::length(pixel3d::cross(logical.positions[t[1]] - logical.positions[t[0]], logical.positions[t[2]] - logical.positions[t[0]])) / 2;
    const double texels = area * reg.pose.scale * reg.pose.scale / 0.6;
    int size = texels > 1200.0 * 1200.0 ? 2048 : 1024;
    // 4096 (2.3) when a 2048 sheet would hold the picture at less than ~0.8 texel per pixel (detailed
    // characters) and the machine has the memory for it (bake peak ~0.9 GiB at 4096 + the 1 GiB reserve).
    const double free_mib = available_ram_mib();
    std::string why = "picture density";
    if (texels > 2600.0 * 2600.0) {
      if (max_size < 4096) why = "4096 would hold the picture better; limited by --max-size";
      else if (free_mib > 0 && free_mib < 2048) why = "4096 would hold the picture better; not enough free memory (" + std::to_string(int(free_mib)) + " MiB)";
      else size = 4096;
    }
    size = std::min(size, max_size);
    report["atlas_choice"] = {{"surface_area", area}, {"texels_at_source_density", texels}, {"size", size}, {"max_size", max_size},
                              {"free_ram_mib", free_mib}, {"reason", why}};
    // Atlas space follows what the picture shows in detail (faces, hands, emblems).
    std::vector<float> density;
    if (mva.density_ratio > 1.01) {
      nlohmann::json dev;
      density = picture_detail_density(logical, src, reg, mva.density_ratio, dev);
      report["atlas_density"] = dev;
    }
    // Attempts: default charts; on UV-related issues other chart options; on budget a smaller atlas.
    struct Result { Bake bake; nlohmann::json quality; };
    std::optional<Result> best;
    nlohmann::json attempts = nlohmann::json::array();
    const std::vector<std::pair<int, int>> plan{{0, size}, {1, size}, {2, size}, {0, std::max(512, std::min(2048, size / 2))}};
    bool budget_hit = false;
    for (size_t i = 0; i < plan.size(); ++i) {
      const auto [mode, sz] = plan[i];
      nlohmann::json a{{"chart_mode", mode}, {"size", sz}};
      // The smaller-atlas retry after a budget overrun gets its own short budget (worker total <= 1.4x).
      const Deadline dl = budget_hit ? Deadline::in_seconds(std::max(30.0, seconds * 0.4)) : deadline;
      try {
        auto atlas = build_atlas(logical, sz, mode, dl, density.empty() ? nullptr : &density);
        a["atlas"] = atlas.metrics;
        BakeOptions bo;
        bo.native = native ? &*native : nullptr;
        bo.multiview = multiview ? &*multiview : nullptr;
        bo.multiview_strength = mva.strength;
        bo.material_lock = mva.material_lock;
        bo.material_match = mva.material_match;
        bo.grazing_cos0 = mva.grazing_cos0;
        bo.grazing_cos1 = mva.grazing_cos1;
        bo.extra_views = extra_ptr;
        bo.streak_radius = mva.streak_radius;
        auto b = bake(logical, atlas, src, reg, bo, dl);
        auto q = evaluate(logical, b.mesh, src, reg, extra_ptr);
        if (multiview) {
          // Generated views are kept only where they win on the checks: the same atlas baked without them is
          // the other candidate (same gate, same score).
          BakeOptions plain = bo;
          plain.multiview = nullptr;
          auto b0 = bake(logical, atlas, src, reg, plain, dl);
          auto q0 = evaluate(logical, b0.mesh, src, reg, extra_ptr);
          const bool views_win = better(q, q0);
          a["multiview_choice"] = {{"selected", views_win ? "with_generated_views" : "without_generated_views"},
                                   {"with_views", {{"verdict", q["verdict"]}, {"score", q["score"]}, {"issues", q["issues"]},
                                                   {"unsupported_patch_fraction", q["hidden_side"].value("unsupported_patch_fraction", 0.0)}}},
                                   {"without_views", {{"verdict", q0["verdict"]}, {"score", q0["score"]}, {"issues", q0["issues"]},
                                                      {"unsupported_patch_fraction", q0["hidden_side"].value("unsupported_patch_fraction", 0.0)}}}};
          if (!views_win) {
            b = std::move(b0);
            q = std::move(q0);
          }
        }
        a["bake"] = b.evidence;
        a["quality"] = q;
        const bool improved = !best || better(q, best->quality);
        if (improved) best = Result{std::move(b), q};
        attempts.push_back(a);
        if (q["verdict"] == "PASS") break;
        // Only UV-shaped problems are worth another atlas; colour/visibility issues will not change.
        const auto& uv = q["uv"];
        const bool uv_problem = uv.value("overlap_fraction", 0.0) > 0.005 || uv.value("stretched_area_fraction", 0.0) > 0.05 ||
                                q["seams"].value("delta_e_mean", 0.0) > 0.04 || q.value("coverage_missing_fraction", 0.0) > 0.02;
        if (!uv_problem) break;
        if (i == 2) break;  // the half-size attempt is for time budget only
      } catch (const BudgetExceeded& e) {
        a["error"] = e.what();
        attempts.push_back(a);
        if (best || budget_hit) break;
        budget_hit = true;
        i = 2;  // jump to the smaller atlas
      } catch (const std::exception& e) {
        a["error"] = e.what();
        attempts.push_back(a);
      }
    }
    report["attempts"] = attempts;
    // Candidates: the auto texture, the GLB's own material (e.g. Generated PBR), anything to compare.
    nlohmann::json candidates = nlohmann::json::array();
    nlohmann::json winner;
    std::string selected;
    if (best) {
      candidates.push_back({{"name", "auto_texture"}, {"quality", best->quality}});
      winner = best->quality;
      selected = "auto_texture";
    }
    try {
      const auto info = material_info(geometry);
      if (info.value("kind", std::string("geometry_only")) != "geometry_only") {
        auto embedded = load_generated_appearance(geometry);
        if (embedded) {
          auto q = evaluate(logical, *embedded, src, reg, extra_ptr);
          candidates.push_back({{"name", "embedded"}, {"kind", info["kind"]}, {"quality", q}});
          if (better(q, winner)) { winner = q; selected = "embedded"; }
        }
      }
    } catch (const std::exception& e) {
      candidates.push_back({{"name", "embedded"}, {"error", e.what()}});
    }
    std::vector<std::pair<std::string, std::shared_ptr<const pixel3d::TexturedMesh>>> compared;
    for (const auto& c : compare) {
      try {
        auto m = load_generated_appearance(c);
        if (!m) throw std::runtime_error("no material");
        if (!geometrically_identical(load_geometry_glb(c), logical)) throw std::runtime_error("different geometry");
        candidates.push_back({{"name", "compare"}, {"file", pixel3d::to_utf8(c)}, {"reference_only", true}, {"quality", evaluate(logical, *m, src, reg, extra_ptr)}});
        compared.push_back({pixel3d::to_utf8(c.filename()), m});
      } catch (const std::exception& e) {
        candidates.push_back({{"name", "compare"}, {"file", pixel3d::to_utf8(c)}, {"error", e.what()}});
      }
    }
    report["candidates"] = candidates;
    if (selected.empty()) throw std::runtime_error("No texture attempt succeeded");
    report["selected"] = selected;
    // Overall verdict: the selected texture's checks plus how well the picture could be matched at all
    // (a texture can be clean and still mostly reconstructed).
    std::string verdict = winner["verdict"];
    nlohmann::json issues = winner["issues"];
    auto review = [&](const std::string& s) { issues.push_back("REVIEW: " + s); if (verdict == "PASS") verdict = "REVIEW"; };
    if (!reg.silhouette_usable) review("the picture has no usable transparency, so its viewpoint could not be measured");
    else if (reg.iou < 0.6) review("the model's outline matches the picture poorly (" + std::to_string(int(reg.iou * 100)) + "%)");
    if (selected == "auto_texture") {
      const auto& ev = best->bake.evidence;
      if (ev.value("observed_fraction", 0.0) + ev.value("blended_fraction", 0.0) < 0.05) review("almost none of the surface could be matched to the picture");
      // Provenance gate: colour spread outward from the picture's visible edges is a guess about the back,
      // and on the regression assets it is the camouflage-patch failure. More than half the surface = FAIL.
      const double propagated = ev.value("reconstructed_fraction", 0.0) + ev.value("fallback_fraction", 0.0);
      if (propagated > 0.5) {
        issues.push_back("FAIL: " + std::to_string(int(std::lround(propagated * 100))) +
                         "% of the surface is extrapolated from the picture's visible edges (no texture information for the hidden sides)");
        verdict = "FAIL";
      }
    }
    report["verdict"] = verdict;
    report["issues"] = issues;
    winner["verdict"] = verdict;
    // Honest limitations.
    if (!reg.silhouette_usable) limitations.push_back("The picture has no usable transparency, so its viewpoint could not be measured; the model front was assumed.");
    if (reg.ambiguous) limitations.push_back("The outline fits more than one viewpoint (symmetric shape); the TRELLIS front was assumed where it fit as well.");
    if (reg.iou < 0.7) limitations.push_back("The model's outline differs from the picture (silhouette match " + std::to_string(int(reg.iou * 100)) + "%); colours near edges may land slightly off.");
    if (best) {
      const auto& ev = best->bake.evidence;
      const double seen = ev.value("observed_fraction", 0.0) + ev.value("blended_fraction", 0.0);
      const double unseen = ev.contains("picture_ownership") ? ev["picture_ownership"].value("not_seen_by_any_picture", 1 - seen) : 1 - seen;
      if (!extra.empty())
        limitations.push_back("About " + std::to_string(int(std::lround(unseen * 100))) + "% of the surface is seen by none of the " +
                              std::to_string(extra.size() + 1) + " pictures; it shows " +
                              (ev.value("native_fraction", 0.0) > 0 ? "TRELLIS's own texture, colour-calibrated to the pictures (model-inferred)." : "a reconstruction from nearby visible colours."));
      else if (ev.value("native_fraction", 0.0) > 0)
        limitations.push_back("About " + std::to_string(int(std::lround((1 - seen) * 100))) +
                              "% of the surface is not visible in the picture; it shows TRELLIS's own texture of the object, colour-calibrated to the picture: model-inferred, not recovered from the picture.");
      else
        limitations.push_back("About " + std::to_string(int(std::lround((1 - seen) * 100))) +
                              "% of the surface is not visible in the picture; it is a reconstruction from nearby visible colours, not recovered detail.");
      if (ev.value("fallback_components", 0) > 0)
        limitations.push_back(std::to_string(ev.value("fallback_components", 0)) + " separate part(s) never appear in the picture; they take the nearest visible colour.");
    }
    limitations.push_back("Lighting, shadows and reflections in the picture are part of the colour (no de-lighting).");
    report["limitations"] = limitations;
    int code = 3;
    if (selected == "auto_texture") {
      const bool hybrid = best->bake.evidence.value("native_fraction", 0.0) > 0;
      nlohmann::json provenance{{"kind", "auto_texture"}, {"engine", hybrid ? "3re Hybrid Texture 1" : "3re Auto Texture 1"},
                                {"hidden_surfaces", hybrid ? "TRELLIS native texture calibrated to the picture (model-inferred, not ground truth)" : "reconstructed estimate, not ground truth"},
                                {"verdict", winner["verdict"]}, {"registration_iou", reg.iou}, {"view", {{"yaw", reg.pose.yaw}, {"elevation", reg.pose.elevation}}},
                                {"observed_fraction", best->bake.evidence["observed_fraction"]},
                                {"pictures", best->bake.evidence.value("pictures", 1)}, {"picture_ownership", best->bake.evidence.value("picture_ownership", nlohmann::json::object())}};
      const auto bytes = write_glb(logical, best->bake.mesh, provenance);
      write_file(output, bytes);
      // Re-import: geometry identical, material readable.
      if (!geometrically_identical(load_geometry_glb(output), logical)) throw std::runtime_error("Re-imported geometry differs");
      const auto reread = load_generated_appearance(output);
      if (!reread || !reread->texture || reread->texture->width() != best->bake.mesh.texture->width()) throw std::runtime_error("Re-imported material differs");
      report["reimport"] = {{"geometry_identical", true}, {"material", material_info(output)}};
      write_png(*best->bake.mesh.texture, fs::path(output).concat(L".base-color.png"));
      write_png(best->bake.provenance, fs::path(output).concat(L".provenance.png"));
      code = 0;
    }
    if (views) {
      fs::create_directories(*views);
      if (best) write_png(sheet(best->bake.mesh, 384, reg.pose.yaw), *views / "auto_texture.png");
      for (const auto& [name, m] : compared) write_png(sheet(*m, 384, reg.pose.yaw), *views / pixel3d::path_from_utf8(name + ".png"));
    }
    report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    report["peak_working_set_mib"] = peak_working_set_mib();
    report["status"] = "OK";
    write_json(report_path, report);
    std::cout << "AUTO_TEXTURE " << selected << " " << report["verdict"].get<std::string>() << " " << report["seconds"].get<double>() << " s" << std::endl;
    return code;
  } catch (const std::exception& e) {
    report["status"] = "ERROR";
    report["error"] = e.what();
    report["limitations"] = limitations;
    report["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    report["peak_working_set_mib"] = peak_working_set_mib();
    try { write_json(report_path, report); } catch (...) {}
    std::cerr << "AUTO_TEXTURE_ERROR " << e.what() << std::endl;
    return 2;
  }
}

int main() {
  std::vector<std::wstring> args;
#ifdef _WIN32
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  LocalFree(argv);
#endif
  std::vector<fs::path> positional, compare;
  bool crisp = false;
  int max_size = 4096;
  double seconds = 300;
  std::optional<fs::path> views, native;
  MultiViewArgs mva;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    auto next = [&]() -> std::wstring { if (i + 1 >= args.size()) throw std::runtime_error("missing value"); return args[++i]; };
    try {
      if (a == L"--crisp") crisp = true;
      else if (a == L"--max-size") { const int v = std::stoi(next()); max_size = v >= 4096 ? 4096 : v >= 2048 ? 2048 : 1024; }
      else if (a == L"--streak-radius") mva.streak_radius = std::clamp(std::stod(next()), 0.0, 0.05);
      else if (a == L"--density-ratio") mva.density_ratio = std::clamp(std::stod(next()), 1.0, 3.0);
      else if (a == L"--view") {
        const fs::path f(next());
        mva.views.push_back({f, pixel3d::to_utf8(fs::path(next()))});
        if (mva.views.size() > 5) throw std::runtime_error("at most 5 extra pictures");
      }
      else if (a == L"--seconds") seconds = std::clamp(std::stod(next()), 10.0, 1800.0);
      else if (a == L"--views") views = fs::path(next());
      else if (a == L"--compare") compare.push_back(fs::path(next()));
      else if (a == L"--native") native = fs::path(next());
      else if (a == L"--mv-export") mva.export_dir = fs::path(next());
      else if (a == L"--mv-views") mva.views_dir = fs::path(next());
      else if (a == L"--no-material-lock") mva.material_lock = false;
      else if (a == L"--no-material-match") mva.material_match = false;
      else if (a == L"--grazing") {
        mva.grazing_cos0 = std::clamp(std::stod(next()), -1.0, 0.95);
        mva.grazing_cos1 = std::clamp(std::stod(next()), mva.grazing_cos0 + 0.01, 1.0);
      }
      else if (a == L"--mv-strength") mva.strength = std::clamp(std::stod(next()), 0.0, 1.0);
      else positional.push_back(fs::path(a));
    } catch (const std::exception&) {
      std::cerr << "Invalid argument" << std::endl;
      return 64;
    }
  }
  if (positional.size() != 3) {
    std::cerr << "Usage: pixel3d-auto-texture geometry.glb source.png output.glb [--crisp] [--max-size 1024|2048|4096] "
                 "[--seconds N] [--views DIR] [--compare other.glb]... [--native native.post]" << std::endl;
    return 64;
  }
  return run(positional, crisp, max_size, seconds, views, compare, native, mva);
}
