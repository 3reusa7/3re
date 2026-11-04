// pixel3d-cli — thin command-line client of the Pixel3D engine (spec §14).
// All work happens in pixel3d::Session; this file only parses, prints and writes files.
#include <cstdio>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include "cli/cli_args.hpp"
#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/common/version.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

namespace {

// Command-line arguments as UTF-8 (Windows argv is in the ANSI code page).
std::vector<std::string> utf8_args(int argc, char** argv) {
  std::vector<std::string> out;
#ifdef _WIN32
  (void)argc;
  (void)argv;
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  for (int i = 1; w && i < n; ++i) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, len - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
    out.push_back(std::move(s));
  }
  if (w) LocalFree(w);
#else
  for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
#endif
  return out;
}

bool report_errors(const OpResult& r, const char* step) {
  if (r.success) return false;
  std::cerr << "error in " << step << ":\n";
  for (const Message& m : r.errors) std::cerr << "  [" << m.code << "] " << m.text << "\n";
  return true;
}

void print_warnings(const OpResult& r) {
  for (const Message& m : r.warnings) std::cerr << "warning: [" << m.code << "] " << m.text << "\n";
}

// What input normalisation did to the images (never silent): input.* notes and warnings.
void print_normalisation(const OpResult& r) {
  for (const Message& m : r.info)
    if (m.code.rfind("input.", 0) == 0) std::printf("  input: %s\n", m.text.c_str());
  for (const Message& m : r.warnings)
    if (m.code.rfind("input.", 0) == 0) std::fprintf(stderr, "warning: [%s] %s\n", m.code.c_str(), m.text.c_str());
}

void print_info(const OpResult& r) {
  for (const Message& m : r.info) std::printf("  note: %s\n", m.text.c_str());
}

void print_summary(const Session& s, const OpResult& v) {
  const Document& d = s.document();
  std::printf("3re %s\n", std::string(version()).c_str());
  if (d.align) std::printf("  size (W x H x D): %d x %d x %d px\n", d.align->dims.W, d.align->dims.H, d.align->dims.D);
  if (d.solid) {
    const TopologyReport& t = d.solid->topology;
    std::printf("  mesh: %d triangles, %d vertices, %d parts, %s, volume %.1f\n", t.triangle_count, t.vertex_count,
                v.stats.parts, t.watertight ? "watertight" : "NOT watertight", t.signed_volume);
  }
  if (d.metrics) {
    std::printf("  projection IoU:");
    for (const auto& [role, m] : d.metrics->views) std::printf(" %s %.3f", std::string(to_string(role)).c_str(), m.iou);
    std::printf("\n  rotations IoU:");
    for (const auto& r : d.metrics->rotations) std::printf(" %d deg %.3f", r.yaw, r.iou);
    std::printf("; thickness %.2f; turntable min area %.2f\n", d.metrics->thickness_ratio,
                d.metrics->turntable.min_area_ratio);
    std::printf("  inferred volume %.1f%%, ambiguous volume %.1f%%\n", 100 * d.metrics->inferred_volume_fraction,
                100 * d.metrics->ambiguous_volume_fraction);
  }
  if (d.conflicts)
    for (const Conflict& c : *d.conflicts)
      std::printf("  %s %-24s %s\n", c.blocks_pass ? "!!" : "  ", c.kind.c_str(), c.text.c_str());
  std::printf("  STATUS: %s\n", std::string(to_string(v.status)).c_str());
}

int cmd_reconstruct(Session& s, const CliArgs& a) {
  ProjectSettings ps;
  ps.name = to_utf8(path_from_utf8(a.out).stem());
  ps.pixels_per_meter = a.pixels_per_meter;  // export scale lives in the project, like in every client
  if (a.profile) ps.profile = *a.profile;
  ps.normalize = a.normalize;
  ps.style.preset = a.style;
  if (report_errors(s.new_project(ps), "new_project")) return 1;
  std::vector<ViewImport> views;
  for (const auto& [role, path] : a.views) views.push_back({role, path_from_utf8(path), {}});
  if (report_errors(s.import_views(views), "import_views")) return 1;
  if (a.single_view_preset || a.single_view_depth) {
    // One drawing: the depth is the user's choice (a preset or pixels), never measured.
    SingleViewSettings sv;
    sv.enabled = true;
    sv.role = views.front().role;
    sv.preset = a.single_view_preset.value_or("custom");
    sv.depth = a.single_view_depth.value_or(0);
    sv.user_depth = true;
    if (report_errors(s.set_single_view(sv), "single_view")) return 1;
  }
  const OpResult rec = s.reconstruct(ReconstructOptions{a.mode});
  if (report_errors(rec, "reconstruct")) return 1;
  print_normalisation(rec);
  const OpResult v = s.validate(ValidateOptions{});
  if (report_errors(v, "validate")) return 1;
  // The CLI always writes the model; the exit code reports its status (spec §14).
  ModelExportRequest er;
  er.path = path_from_utf8(a.out);
  er.preset = a.preset;
  er.units = a.units;
  er.front_negative_z = a.front_negative_z;
  er.unlit = a.unlit;
  er.allow_needs_review = true;
  er.allow_fail = true;
  const OpResult ex = s.export_model(er);
  if (report_errors(ex, "export")) return 1;
  bool artifacts_ok = true;
  OpResult art;
  if (!a.renders.empty()) {
    ArtifactRequest ar;
    ar.dir = path_from_utf8(a.renders);
    ar.turntable = a.turntable;
    art = s.write_artifacts(ar);
    artifacts_ok = !report_errors(art, "renders");
  }
  if (!a.report.empty()) {
    nlohmann::json report = v.data["report"];
    report["export"] = ex.data;
    if (!a.renders.empty())
      report["artifacts"] = {{"ok", art.success},
                             {"files", art.data.value("files", nlohmann::json::array())},
                             {"errors", art.errors}};
    const Message w = write_file_atomic(path_from_utf8(a.report), report.dump(2) + "\n");
    if (w.code != "ok") {
      std::cerr << "error writing report: [" << w.code << "] " << w.text << "\n";
      artifacts_ok = false;
    }
  }
  if (!a.project.empty() && report_errors(s.save(path_from_utf8(a.project)), "save")) return 1;
  print_warnings(ex);
  print_summary(s, v);
  for (const auto& f : ex.data["files"]) std::printf("  wrote %s\n", f.get<std::string>().c_str());
  std::printf("  %s, %s, front %s, %g px per metre; size %.3g x %.3g x %.3g %s\n",
              ex.data["format"].get<std::string>().c_str(), ex.data["preset"].get<std::string>().c_str(),
              ex.data["front"].get<std::string>().c_str(), ex.data["pixels_per_meter"].get<double>(),
              ex.data["size"][0].get<double>(), ex.data["size"][1].get<double>(), ex.data["size"][2].get<double>(),
              ex.data["units"].get<std::string>().c_str());
  print_info(ex);
  if (!artifacts_ok) {
    std::fprintf(stderr, "error: requested output files were not all written\n");
    return 1;
  }
  return exit_code_for(v.status);
}

// LOW-POLY STUDIO: the same Session calls as the GUI and the MCP server.
int cmd_lowpoly(Session& s, const CliArgs& a) {
  if (!a.parts_project.empty()) {
    // Typed parts (and settings) of a saved Low-Poly Studio project; drawings given here replace its views.
    if (report_errors(s.load(path_from_utf8(a.parts_project)), "load --parts")) return 1;
    if (!s.document().settings.lowpoly.enabled) {
      std::cerr << "error: --parts " << a.parts_project << " is not a Low-Poly Studio project\n";
      return 1;
    }
  } else {
    ProjectSettings ps;
    ps.name = to_utf8(path_from_utf8(a.out).stem());
    ps.pixels_per_meter = a.pixels_per_meter;
    ps.normalize = a.normalize;
    ps.lowpoly.enabled = true;
    if (report_errors(s.new_project(ps), "new_project")) return 1;
  }
  std::vector<ViewImport> views;
  for (const auto& [role, path] : a.views) views.push_back({role, path_from_utf8(path), {}});
  if (!views.empty() && report_errors(s.import_views(views), "import_views")) return 1;
  if (a.layer_parts) {
    const OpResult lp = s.suggest_layer_parts({.apply = true, .replace = true, .accept = *a.layer_parts});
    if (report_errors(lp, "layer parts")) return 1;
    print_warnings(lp);
    std::printf("  parts from the drawing's layers:\n");
    for (const auto& l : lp.data["layers"])
      std::printf("    %-12s %-10s %s\n", l["name"].get<std::string>().c_str(), l.value("type", std::string()).c_str(),
                  l.contains("id") ? "accepted" : l.value("why_not", std::string("not chosen")).c_str());
  } else if (a.parts_project.empty()) {
    const OpResult sg = s.lowpoly_suggest_parts({.apply = true});
    if (report_errors(sg, "suggest parts")) return 1;
    std::printf("  parts suggested from the drawing (template %s):\n", sg.data["template"].get<std::string>().c_str());
    for (const auto& p : sg.data["parts"])
      std::printf("    %-12s %s\n", p["name"].get<std::string>().c_str(), p["type"].get<std::string>().c_str());
  }
  const OpResult b = s.lowpoly_build(a.lowpoly);
  if (report_errors(b, "lowpoly build")) return 1;
  print_normalisation(b);
  print_warnings(b);
  const nlohmann::json& lp = b.data["lowpoly"];
  std::printf("3re %s - Low-Poly Studio\n", std::string(version()).c_str());
  std::printf("  template %s, %d / %d triangles, %d bones\n", lp["template"].get<std::string>().c_str(),
              lp["triangles"].get<int>(), lp["budget"].get<int>(), lp["bones"].get<int>());
  for (const auto& p : lp["parts"])
    std::printf("    %-12s %-9s %4d tris  depth %s\n", p["name"].get<std::string>().c_str(),
                p["type"].get<std::string>().c_str(), p["triangles"].get<int>(),
                p["depth_source"].get<std::string>().c_str());
  for (const auto& [role, iou] : lp["silhouette_iou"].items())
    std::printf("  silhouette IoU %s %.3f\n", role.c_str(), iou.get<double>());
  for (const auto& c : lp["checks"])
    if (!c["passed"].get<bool>())
      std::printf("  %s %s\n", c["critical"].get<bool>() ? "FAIL" : "warn", c["name"].get<std::string>().c_str());
  for (const auto& c : lp["conflicts"])
    std::printf("  %s %-30s %s\n", c["blocking"].get<bool>() ? "!!" : "  ", c["kind"].get<std::string>().c_str(),
                c["text"].get<std::string>().c_str());
  std::printf("  STATUS: %s\n", std::string(to_string(b.status)).c_str());
  bool ok = true;
  LowpolyExportRequest er;
  er.path = path_from_utf8(a.out);
  er.allow_needs_review = a.accept_needs_review;
  er.engine_files = a.engine_files;
  const OpResult ex = s.lowpoly_export(er);
  if (ex.success) {
    print_warnings(ex);
    for (const auto& f : ex.data["files"]) std::printf("  wrote %s\n", f.get<std::string>().c_str());
  } else {
    for (const Message& m : ex.errors) std::fprintf(stderr, "not written: [%s] %s\n", m.code.c_str(), m.text.c_str());
  }
  if (!a.renders.empty()) {
    LowpolyRenderRequest rr;
    rr.yaws = {0, 45, 90, 135, 180, 270};
    rr.contact_sheet = true;
    std::vector<NamedImage> imgs;
    OpResult rres = s.lowpoly_render(rr, &imgs);
    rr.pose = true;
    rr.yaws = {0, 90};
    rr.contact_sheet = false;
    if (rres.success) rres = s.lowpoly_render(rr, &imgs);
    if (rres.success) write_named_images(path_from_utf8(a.renders), imgs, rres);
    ok &= !report_errors(rres, "renders");
  }
  if (!a.report.empty()) {
    nlohmann::json report = lp;
    report["export"] = ex.success ? ex.data : nlohmann::json{{"written", false}, {"errors", nlohmann::json(ex.errors)}};
    const Message w = write_file_atomic(path_from_utf8(a.report), report.dump(2) + "\n");
    if (w.code != "ok") {
      std::cerr << "error writing report: [" << w.code << "] " << w.text << "\n";
      ok = false;
    }
  }
  if (!a.project.empty() && report_errors(s.save(path_from_utf8(a.project)), "save")) return 1;
  if (!ok) return 1;
  return exit_code_for(b.status);
}

int cmd_validate(Session& s, const CliArgs& a) {
  if (report_errors(s.load(path_from_utf8(a.project)), "load")) return 1;
  ValidateOptions vo;
  vo.profile = a.profile;
  const OpResult v = s.validate(vo);
  if (report_errors(v, "validate")) return 1;
  print_summary(s, v);
  if (!a.report.empty()) {
    const Message w = write_file_atomic(path_from_utf8(a.report), v.data["report"].dump(2) + "\n");
    if (w.code != "ok") {
      std::cerr << "error writing report: [" << w.code << "] " << w.text << "\n";
      return 1;
    }
  }
  return exit_code_for(v.status);
}

int cmd_info(Session& s, const CliArgs& a) {
  const OpResult l = s.load(path_from_utf8(a.project));
  if (report_errors(l, "load")) return 1;
  const OpResult an = s.analyze();
  if (report_errors(an, "analyze")) return 1;
  nlohmann::json info = an.data;
  info["project"] = s.document().settings.name;
  info["decisions"] = s.document().decisions.size();
  info["depth_constraints"] = s.document().depth_constraints.size();
  info["checkpoints"] = s.checkpoint_names();
  info["log_entries"] = s.log().size();
  info["mesh_cache"] = l.data["mesh_cache"];
  std::printf("%s\n", info.dump(2).c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  // Messages are UTF-8 (paths in any script, e.g. on an Arabic Windows): the console decodes them as such.
  SetConsoleOutputCP(CP_UTF8);
#endif
  const auto a = parse_cli(utf8_args(argc, argv));
  if (!a) {
    std::cerr << "error: " << a.error().text << "\n\n" << cli_usage();
    return 1;
  }
  if (a->command == "version") {
    std::printf("pixel3d-cli %s\n", std::string(version()).c_str());
    return 0;
  }
  if (a->command == "help") {
    std::fputs(cli_usage().c_str(), stdout);
    return 0;
  }
  Session s(Source::Cli);
  if (a->command == "reconstruct") return cmd_reconstruct(s, *a);
  if (a->command == "validate") return cmd_validate(s, *a);
  if (a->command == "lowpoly") return cmd_lowpoly(s, *a);
  return cmd_info(s, *a);
}
