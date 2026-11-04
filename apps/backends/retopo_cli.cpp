// pixel3d-retopo: the Smart Retopology worker. Run by the 3re Retopology workspace (or by hand):
//
//   pixel3d-retopo remesh <source.glb|.obj> <out-dir> --engine DIR [--faces N] [--mode organic|mechanical]
//                         [--seconds N]
//   pixel3d-retopo export <source.glb> <mesh.obj> <out.glb> [--size 1024|2048|4096] [--seconds N]
//
// remesh: the source geometry goes through QuadWild-BiMDF (DIR holds quadwild.exe, quad_from_patches.exe
// and the upstream config/ folder): field + tracing (quadwild.exe, step 2), then the quadrangulation
// (quad_from_patches.exe) with the quad size set from --faces. Both run in a job object (killed with this
// worker) under a time limit; any nonzero exit rejects the result even if a file was written. Writes
// <out-dir>/result.obj (quads) and <out-dir>/retopo.json (counts, open/non-manifold edges, two-sided surface
// distance to the source). Exit 0 = result written, 2 = failed (retopo.json says why).
//
// export: <mesh.obj> (the result, possibly edited) becomes <out.glb> with a new UV atlas and the source's base
// colour taken from the closest source surface point, plus <out.obj> (the quads, for modelling tools). The GLB
// is read back and its triangles compared with the mesh before success is reported (<out.glb>.retopo.json).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include "auto_texture.hpp"
#include "backends/mesh_asset.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "retopo_engine.hpp"
#include "retopo_material.hpp"
#include "retopo_acceptance.hpp"
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

namespace {
using Clock = std::chrono::steady_clock;
constexpr double kQuadCalibration = 2.5;

void progress(int percent, const std::string& text) { std::cout << "RETOPO_PROGRESS " << percent << " " << text << std::endl; }

void write_json(const fs::path& path, const nlohmann::json& j) {
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << j.dump(2);
    if (!out.flush()) throw std::runtime_error("Cannot write " + pixel3d::to_utf8(path));
  }
  fs::rename(tmp, path);
}

double peak_working_set_mib() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS c{};
  c.cb = sizeof(c);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c))) return c.PeakWorkingSetSize / 1048576.0;
#endif
  return 0;
}

pixel3d::Mesh load_source(const fs::path& path) {
  auto ext = pixel3d::to_utf8(path.extension());
  for (auto& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (ext == ".obj") return retopo::triangulate(retopo::read_obj(path));
  return load_geometry_glb(path);
}

#ifdef _WIN32
// One engine stage: own job object (killed with this worker, memory-capped), stdout+stderr to `log`.
struct StageResult {
  DWORD exit_code = 0;
  double seconds = 0, peak_mib = 0;
};
StageResult run_stage(const fs::path& exe, const std::vector<std::wstring>& args, const fs::path& cwd, const fs::path& log,
                      double seconds) {
  std::wstring command = L"\"" + exe.wstring() + L"\"";
  for (const auto& a : args) command += L" \"" + a + L"\"";
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out = CreateFileW(log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create the engine log");
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY;
  MEMORYSTATUSEX mem{};
  mem.dwLength = sizeof(mem);
  if (!job || !GlobalMemoryStatusEx(&mem)) {
    if(job) CloseHandle(job); CloseHandle(out);
    throw std::runtime_error("Cannot establish engine memory protection");
  }
  // At most 60% of physical memory: the solver must never push the desktop into swapping.
  limits.JobMemoryLimit = SIZE_T(std::min<DWORDLONG>(mem.ullTotalPhys * 6 / 10, 12ull << 30));
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    CloseHandle(job); CloseHandle(out);
    throw std::runtime_error("Cannot enforce engine job memory limit");
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = out;
  si.hStdError = out;
  si.hStdInput = nullptr;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> line(command.begin(), command.end());
  line.push_back(0);
  // Engine DLLs (MinGW runtime) live beside the executables.
  const auto started = Clock::now();
  const BOOL ok = CreateProcessW(exe.wstring().c_str(), line.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr, cwd.wstring().c_str(), &si, &pi);
  CloseHandle(out);
  if (!ok) {
    CloseHandle(job);
    throw std::runtime_error("Cannot start " + pixel3d::to_utf8(exe.filename()));
  }
  if (!AssignProcessToJobObject(job, pi.hProcess) || ResumeThread(pi.hThread) == DWORD(-1)) {
    TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(job);
    throw std::runtime_error("Cannot start engine under cancellation and memory protection");
  }
  CloseHandle(pi.hThread);
  const DWORD wait = WaitForSingleObject(pi.hProcess, DWORD(std::clamp(seconds, 1.0, 3600.0) * 1000));
  StageResult r;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION used{};
  QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &used, sizeof(used), nullptr);
  r.peak_mib = double(used.PeakJobMemoryUsed) / 1048576.0;
  if (wait != WAIT_OBJECT_0) {
    TerminateJobObject(job, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hProcess);
    CloseHandle(job);
    throw std::runtime_error(pixel3d::to_utf8(exe.filename()) + " exceeded its time limit");
  }
  GetExitCodeProcess(pi.hProcess, &r.exit_code);
  r.seconds = std::chrono::duration<double>(Clock::now() - started).count();
  CloseHandle(pi.hProcess);
  CloseHandle(job);
  return r;
}
#endif

std::string tail(const fs::path& log, size_t bytes = 1500) {
  std::ifstream in(log, std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(in)), {});
  return s.size() > bytes ? s.substr(s.size() - bytes) : s;
}

// Self-intersection audit of a triangle mesh with 3re's own geometry audit (pixel3d-geometry-audit.exe beside
// this worker, the tool every generated model goes through). Empty json when the tool is missing.
nlohmann::json intersection_audit(const pixel3d::Mesh& mesh, const fs::path& work, const std::string& name) {
#ifdef _WIN32
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const fs::path tool = fs::path(self).parent_path() / "pixel3d-geometry-audit.exe";
  if (!fs::is_regular_file(tool)) return nlohmann::json::object();
  const fs::path bin = work / (name + ".bin"), out = work / (name + ".audit.json");
  std::error_code ec;
  fs::remove(bin, ec);
  fs::remove(out, ec);
  write_geometry_binary(mesh, bin);
  const auto r = run_stage(tool, {bin.wstring(), out.wstring()}, work, work / (name + ".audit.log"), 300);
  if (r.exit_code != 0 || !fs::is_regular_file(out)) return {{"error", "audit exit " + std::to_string(r.exit_code) + ": " + tail(work / (name + ".audit.log"), 200)}};
  std::ifstream in(out);
  auto j = nlohmann::json::parse(in);
  return {{"pair_classification", j.value("pair_classification", nlohmann::json::object())},
          {"edge_star_classification", j.value("edge_star_classification", nlohmann::json::object())},
          {"seconds", r.seconds}};
#else
  (void)mesh; (void)work; (void)name;
  return nlohmann::json::object();
#endif
}

int crossings(const nlohmann::json& audit) {
  if (!audit.is_object() || !audit.contains("pair_classification")) return -1;
  const auto& p = audit.value("pair_classification", nlohmann::json::object());
  return p.value("transverse_crossing", 0) + p.value("coplanar_area_overlap", 0) + p.value("edge_face_contact", 0) +
         audit.value("edge_star_classification", nlohmann::json::object()).value("crossing_sheet_stars", 0);
}


// The engine reads paths as narrow strings; it runs in an ASCII work folder under %TEMP%.
fs::path engine_work_dir() {
  fs::path base = fs::temp_directory_path();
  const std::wstring name = base.wstring();
  if (!std::all_of(name.begin(), name.end(), [](wchar_t c) { return c < 128; })) base = "C:/Windows/Temp";
  const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
#ifdef _WIN32
  const auto pid = GetCurrentProcessId();
#else
  const int pid = 0;
#endif
  fs::path dir = base / ("3re-retopo-" + std::to_string(pid) + "-" + std::to_string(stamp));
  fs::create_directories(dir);
  return dir;
}

int remesh(const fs::path& source, const fs::path& out_dir, const fs::path& engine, int faces, bool mechanical, double seconds) {
  const auto started = Clock::now();
  nlohmann::json report{{"tool", "pixel3d-retopo remesh"}, {"engine", "QuadWild-BiMDF 0.0.2 (GPL-3.0)"},
                        {"source", pixel3d::to_utf8(source)}, {"target_faces", faces}, {"mode", mechanical ? "mechanical" : "organic"}};
  fs::create_directories(out_dir);
  const fs::path report_path = out_dir / "retopo.json";
  fs::path work;
  try {
    const fs::path quadwild = engine / "quadwild.exe", quads = engine / "quad_from_patches.exe";
    const fs::path prep = engine / "config/prep_config" / (mechanical ? "basic_setup_Mechanical.txt" : "basic_setup_Organic.txt");
    const fs::path flow = engine / "config/main_config/flow.txt";
    if (!fs::is_regular_file(quadwild) || !fs::is_regular_file(quads) || !fs::is_regular_file(prep) || !fs::is_regular_file(flow))
      throw std::runtime_error("The QuadWild engine is incomplete (quadwild.exe, quad_from_patches.exe, config/)");
    if (faces < 50 || faces > 200000) throw std::runtime_error("Target face count must be 50 - 200000");
    progress(2, "Reading the model");
    const pixel3d::Mesh source_mesh = load_source(source);
    pixel3d::Mesh input = retopo::engine_input(source_mesh);
    if (input.tris.size() > 2000000) throw std::runtime_error("The model has more than 2 million triangles");
    report["input"] = {{"vertices", input.positions.size()}, {"triangles", input.tris.size()}};
    try {
      report["input"]["topology"] = inspect_geometry(input);
    } catch (const std::exception& e) {
      report["input"]["topology_error"] = e.what();
    }
    // Never close holes, remove cavities, or simplify the user's surface as
    // an implicit prerequisite. High-genus inputs remain unchanged and bounded
    // engine failures are reported rather than replaced by an outer shell.
    report["input"]["handles"] = retopo::handles(input);
    report["input"]["geometry_preprocessing"] = "connectivity only; no shell closing or simplification";
    work = engine_work_dir();
    retopo::write_obj(retopo::to_poly(input), work / "input.obj");
#ifdef _WIN32
    progress(8, "Field and patch tracing (QuadWild)");
    const auto stage1 = run_stage(quadwild, {(work / "input.obj").wstring(), L"2", prep.wstring()}, engine, out_dir / "quadwild.log",
                                  seconds * 0.6);
    report["stages"]["quadwild"] = {{"exit_code", stage1.exit_code}, {"seconds", stage1.seconds}, {"peak_mib", stage1.peak_mib}};
    if (stage1.exit_code != 0) {
      char hex[16];
      std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(stage1.exit_code));
      throw std::runtime_error(std::string("quadwild.exe failed (exit ") + hex + "): " + tail(out_dir / "quadwild.log", 300));
    }
    const fs::path prepared = work / "input_rem_p0.obj";
    if (!fs::is_regular_file(prepared)) throw std::runtime_error("quadwild.exe wrote no traced mesh");
    // Quad size from the requested face count over the prepared surface. QuadWild's flow scaleFact sizes
    // quads in units of the prepared mesh's average edge; measured on generated models its quads come out
    // ~kQuadCalibration times smaller in side than sqrt(area / faces) suggests. One correction pass (the
    // quadrangulation alone, ~1-2 min) follows when the count is more than 35% off.
    const auto prep_mesh = retopo::triangulate(retopo::read_obj(prepared));
    double area = 0, edge = 0;
    for (const auto& t : prep_mesh.tris) {
      const auto a = prep_mesh.positions[t[0]], b = prep_mesh.positions[t[1]], c = prep_mesh.positions[t[2]];
      area += 0.5 * pixel3d::length(pixel3d::cross(b - a, c - a));
      edge += pixel3d::length(b - a) + pixel3d::length(c - b) + pixel3d::length(a - c);
    }
    edge /= 3.0 * double(prep_mesh.tris.size());
    std::ifstream flow_in(flow, std::ios::binary);
    std::vector<std::string> flow_lines;
    for (std::string line; std::getline(flow_in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      flow_lines.push_back(line);
    }
    if (std::count_if(flow_lines.begin(), flow_lines.end(), [](const std::string& l) { return l.rfind("scaleFact ", 0) == 0; }) != 1)
      throw std::runtime_error("Unsupported QuadWild flow configuration");
    const fs::path produced = work / "input_rem_p0_1_quadrangulation_smooth.obj";
    double scale = std::clamp(retopo::quad_scale(area, edge, faces) * kQuadCalibration, 0.05, 50.0);
    retopo::PolyMesh result;
    for (int pass = 0; pass < 2; ++pass) {
      {
        std::ofstream out(work / "flow.txt", std::ios::binary | std::ios::trunc);
        for (const auto& l : flow_lines) out << (l.rfind("scaleFact ", 0) == 0 ? "scaleFact " + std::to_string(scale) : l) << '\n';
      }
      std::error_code ec;
      fs::remove(produced, ec);
      progress(pass ? 62 : 40, pass ? "Quadrangulation again at the corrected size" : "Quadrangulation (QuadWild BiMDF)");
      const double left = seconds - std::chrono::duration<double>(Clock::now() - started).count();
      const fs::path log = out_dir / (pass ? "quad_from_patches-2.log" : "quad_from_patches.log");
      const auto stage2 = run_stage(quads, {prepared.wstring(), L"1", (work / "flow.txt").wstring(), (work / "solver.json").wstring()},
                                    engine, log, std::max(30.0, left));
      report["stages"]["quad_from_patches"].push_back(
          {{"exit_code", stage2.exit_code}, {"seconds", stage2.seconds}, {"peak_mib", stage2.peak_mib}, {"scale_factor", scale}});
      if (stage2.exit_code != 0) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(stage2.exit_code));
        throw std::runtime_error(std::string("quad_from_patches.exe failed (exit ") + hex + "): " + tail(log, 300));
      }
      if (!fs::is_regular_file(produced) || fs::file_size(produced) == 0) throw std::runtime_error("QuadWild wrote no quad mesh");
      result = retopo::read_obj(produced);
      const double ratio = double(result.faces.size()) / double(faces);
      report["stages"]["quad_from_patches"].back()["faces"] = result.faces.size();
      if (pass == 1 || (ratio > 0.65 && ratio < 1.35)) break;
      const double left_now = seconds - std::chrono::duration<double>(Clock::now() - started).count();
      if (left_now < 2.5 * stage2.seconds) break;  // no time for a second pass: keep this one
      scale = std::clamp(scale * std::sqrt(ratio), 0.05, 50.0);
    }
    report["scale_factor"] = scale;
#else
    throw std::runtime_error("The QuadWild engine runs on Windows only");
    retopo::PolyMesh result;
#endif
    progress(85, "Measuring the result against the model");
    retopo::write_obj(result, out_dir / "result.obj");
    report["result"] = retopo::measure(source_mesh, result);
    report["result"]["topology"] = inspect_geometry(retopo::triangulate(result));
    report["result"]["safety"] = "UNKNOWN";
    const auto& topo = report["result"]["topology"];
    for (const auto* key : {"overfull_edges", "invalid_vertex_links", "inconsistent_winding_edges", "duplicate_faces", "zero_area_faces"})
      if (topo.value(key, 0) > 0) report["result"]["safety"] = "UNSAFE";
    // Quads are flat and of one global size: where the model is thinner than a quad (sword blade ~3 mm
    // against ~8 mm quads at 4k faces) opposite faces cut through each other. QuadWild has no local
    // density, so this is measured and reported, never hidden or "repaired" by moving the surface.
    progress(92, "Checking the result for self-intersections");
    const auto audit = intersection_audit(retopo::triangulate(result), work, "result");
    if (crossings(audit) < 0) {
      report["result"]["intersection_status"] = "NOT_TESTED";
      if (audit.contains("error")) report["result"]["intersection_error"] = audit["error"];
    } else {
      report["result"]["intersections"] = audit;
      report["result"]["self_intersections"] = crossings(audit);
      report["result"]["intersection_status"] = crossings(audit) ? "CROSSING" : "CLEAR";
      // Reported, never hidden: a crossing result stays viewable and editable but is not SAFE.
      if (crossings(audit)) report["result"]["safety"] = "UNSAFE";
      else if (report["result"]["safety"] == "UNKNOWN") report["result"]["safety"] = "CLEAR";
    }
    if (!report["input"].contains("topology"))
      throw std::runtime_error("Input topology audit unavailable; result cannot be accepted");
    retopo::require_preserved_topology(report["input"]["topology"], topo,
        report["input"]["handles"].get<int>(), retopo::handles(retopo::triangulate(result)));
    report["status"] = "OK";
  } catch (const std::exception& e) {
    report["status"] = "ERROR";
    report["error"] = e.what();
  }
  if (!work.empty()) {
    std::error_code ec;
    fs::remove_all(work, ec);
  }
  report["seconds"] = std::chrono::duration<double>(Clock::now() - started).count();
  report["worker_peak_working_set_mib"] = peak_working_set_mib();
  write_json(report_path, report);
  if (report["status"] != "OK") {
    std::cerr << "RETOPO_ERROR " << report["error"].get<std::string>() << std::endl;
    return 2;
  }
  progress(100, "Done");
  std::cout << "RETOPO_DONE " << pixel3d::to_utf8(out_dir / "result.obj") << std::endl;
  return 0;
}

int export_glb(const fs::path& source, const fs::path& mesh_obj, const fs::path& out, int size, double seconds) {
  const auto started = Clock::now();
  nlohmann::json report{{"tool", "pixel3d-retopo export"}, {"source", pixel3d::to_utf8(source)}, {"mesh", pixel3d::to_utf8(mesh_obj)}};
  const fs::path report_path = out.string() + ".retopo.json";
  try {
    fs::path quad_output = out; quad_output.replace_extension(".obj");
    if (fs::exists(out) || fs::exists(quad_output) || fs::exists(out.string() + ".tmp"))
      throw std::runtime_error("Refusing to overwrite an existing export");
    progress(5, "Reading the new mesh");
    const auto poly = retopo::read_obj(mesh_obj);
    const auto target = retopo::triangulate(poly);
    // Fail closed: unsupported materials must never become an apparently
    // successful grey/base-colour-only export.
    const auto source_material = material_info(source, true);
    if (source_material.value("kind", "") == "geometry_only")
      throw std::runtime_error("Textured export requires a supported source material; the original is unchanged");
    const auto source_glb = retopo::read_material_glb(source);
    const auto retained_material = retopo::source_material(source_glb);
    const auto appearance = load_generated_appearance(source, true);
    if (!appearance || !appearance->texture)
      throw std::runtime_error("Source texture could not be loaded; export cancelled without fallback");
    report["source_material"] = source_material;
    progress(20, appearance ? "Transferring colours from the model" : "Building UVs");
    nlohmann::json evidence;
    const auto textured = retopo::transfer_appearance(target, appearance.get(), size, seconds, evidence, source_material["maps"]["base_color"]["sampler"].value("magFilter", -1) == 9728);
    report["appearance"] = evidence;
    progress(80, "Writing the GLB");
    std::shared_ptr<const pixel3d::ImageRGBA> mr;
    if (retained_material.at("pbrMetallicRoughness").contains("metallicRoughnessTexture")) {
      auto mr_source = *appearance;
      mr_source.texture = retopo::material_map(source_glb, retained_material["pbrMetallicRoughness"]["metallicRoughnessTexture"]);
      nlohmann::json mr_evidence;
      const double remaining = seconds - std::chrono::duration<double>(Clock::now() - started).count();
      if (remaining <= 0) throw std::runtime_error("Material transfer time budget exhausted");
      const auto mr_baked = retopo::transfer_appearance(target, &mr_source, size, remaining, mr_evidence);
      if (mr_baked.indices != textured.indices || mr_baked.uvs.size() != textured.uvs.size())
        throw std::runtime_error("PBR atlas layout mismatch");
      for (size_t i=0;i<textured.uvs.size();++i)
        if (mr_baked.uvs[i].x != textured.uvs[i].x || mr_baked.uvs[i].y != textured.uvs[i].y)
          throw std::runtime_error("PBR atlas coordinates mismatch");
      mr = mr_baked.texture;
      report["metallic_roughness_transfer"] = mr_evidence;
    }
    auto bytes = autotex::write_glb(target, textured, {{"kind", "auto_texture"}, {"method", "retopology_source_transfer"}, {"generator", "3re Smart Retopology"}});
    bytes = retopo::preserve_material(std::move(bytes), retained_material, mr.get(), source_material["maps"]["base_color"]["sampler"].value("magFilter", -1) == 9728,
      source_material["maps"]["base_color"]["sampler"],
      source_material["maps"].contains("metallic_roughness") ? source_material["maps"]["metallic_roughness"]["sampler"] : nlohmann::json::object());
    const fs::path tmp = out.string() + ".tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
      if (!f.flush()) throw std::runtime_error("Cannot write the GLB");
    }

    progress(90, "Reading the GLB back");
    const auto reloaded = load_geometry_glb(tmp);
    const bool same = geometrically_identical(target, reloaded);
    const auto reread = load_generated_appearance(tmp, true);
    report["reimport_material"] = material_info(tmp, true);
    report["reimport"] = {{"triangles", reloaded.tris.size()}, {"geometry_identical", same}, {"texture", bool(reread && reread->texture)}};
    if (!same || !reread || !reread->texture) throw std::runtime_error("The written GLB does not read back as the same mesh");
    const auto& re_mat = report["reimport_material"];
    for(const auto* key : {"metallic_factor", "roughness_factor", "alpha_mode", "double_sided", "unlit", "base_color_factor"})
      if (re_mat.at(key) != source_material.at(key)) throw std::runtime_error("Material properties changed on export");
    if(re_mat.at("maps").size()!=source_material.at("maps").size())
      throw std::runtime_error("Material map count changed on export");
    fs::rename(tmp, out);
    retopo::write_obj(poly, quad_output);
    report["quads_obj"] = pixel3d::to_utf8(quad_output);
    report["status"] = "OK";
  } catch (const std::exception& e) {
    report["status"] = "ERROR";
    report["error"] = e.what();
  }
  report["seconds"] = std::chrono::duration<double>(Clock::now() - started).count();
  report["worker_peak_working_set_mib"] = peak_working_set_mib();
  write_json(report_path, report);
  if (report["status"] != "OK") {
    std::cerr << "RETOPO_ERROR " << report["error"].get<std::string>() << std::endl;
    return 2;
  }
  progress(100, "Done");
  std::cout << "RETOPO_EXPORTED " << pixel3d::to_utf8(out) << std::endl;
  return 0;
}
}  // namespace

int main() {
  std::vector<std::wstring> args;
#ifdef _WIN32
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
  LocalFree(argv);
#endif
  std::vector<fs::path> positional;
  std::optional<fs::path> engine;
  int faces = 4000, size = 2048;
  bool mechanical = false;
  double seconds = 600;
  try {
    for (size_t i = 0; i < args.size(); ++i) {
      const auto& a = args[i];
      auto next = [&]() -> std::wstring { if (i + 1 >= args.size()) throw std::runtime_error("missing value"); return args[++i]; };
      if (a == L"--engine") engine = fs::path(next());
      else if (a == L"--faces") faces = std::stoi(next());
      else if (a == L"--mode") mechanical = next() == L"mechanical";
      else if (a == L"--size") { const int v = std::stoi(next()); size = v >= 4096 ? 4096 : v >= 2048 ? 2048 : 1024; }
      else if (a == L"--seconds") seconds = std::clamp(std::stod(next()), 30.0, 3600.0);
      else positional.push_back(fs::path(a));
    }
    const std::wstring command = positional.empty() ? L"" : positional[0].wstring();
    if (command == L"remesh" && positional.size() == 3 && engine) return remesh(positional[1], positional[2], *engine, faces, mechanical, seconds);
    if (command == L"inspect" && positional.size() == 2) {
      const auto mesh = retopo::engine_input(load_source(positional[1]));
      std::cout << inspect_geometry(mesh).dump(1) << std::endl;
      return 0;
    }
    if (command == L"export" && positional.size() == 4) return export_glb(positional[1], positional[2], positional[3], size, seconds);
  } catch (const std::exception& e) {
    std::cerr << "RETOPO_ERROR " << e.what() << std::endl;
    return 2;
  }
  std::cerr << "usage: pixel3d-retopo remesh <source.glb|.obj> <out-dir> --engine DIR [--faces N] [--mode organic|mechanical] [--seconds N]\n"
               "       pixel3d-retopo export <source.glb> <mesh.obj> <out.glb> [--size 1024|2048|4096] [--seconds N]\n";
  return 1;
}
