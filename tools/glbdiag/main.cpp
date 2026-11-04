// pixel3d-glbdiag <project.p3d> <out_dir> — developer tool (not packaged), 1.2.0 P2.
// Diagnoses a failing `glb_roundtrip` check: opens the project (read only), validates it
// like every client does, then rebuilds exactly the GLB that pipeline::validate checks in
// memory (same model, same ExportOptions), writes those bytes to <out_dir>/validated.glb so
// independent tools (Khronos glTF-Validator, pixel3d-glbcheck, Godot) can judge the same
// file, and prints every sub-check: with and without the re-render comparison, plus the
// properties of the source mesh the check depends on (normals, UVs, finite values).
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <nlohmann/json.hpp>

#include "pixel3d/engine/pipeline.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/style/styled_model.hpp"

using namespace pixel3d;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: pixel3d-glbdiag <project.p3d> <out_dir>\n";
    return 1;
  }
  const fs::path project(reinterpret_cast<const char8_t*>(argv[1])), out(reinterpret_cast<const char8_t*>(argv[2]));
  fs::create_directories(out);
  nlohmann::json j;
  Session s;
  const OpResult loaded = s.load(project);
  j["load"] = {{"success", loaded.success}, {"errors", to_json(loaded)["errors"]}};
  if (!loaded.success) {
    std::cout << j.dump(2) << "\n";
    return 1;
  }
  const OpResult v = s.validate(ValidateOptions{});
  const nlohmann::json vj = to_json(v);
  j["validate"] = {{"success", v.success}, {"status", vj["status"]}, {"errors", vj["errors"]}};
  if (vj.contains("validation") && vj["validation"].is_object()) {
    j["validate"]["failed_checks"] = vj["validation"].value("failed_checks", nlohmann::json::array());
    if (vj["validation"].contains("metrics")) j["validate"]["glb"] = vj["validation"]["metrics"].value("glb", nlohmann::json{});
  }
  const Document& d = s.document();
  if (!d.align || (!d.texture && !d.styled)) {
    j["error"] = "the project has no validated model";
    std::cout << j.dump(2) << "\n";
    return 1;
  }
  const TexturedMesh& mesh = pipeline::model_mesh(d);
  ExportOptions eo;
  eo.pixels_per_meter = d.settings.pixels_per_meter;
  if (d.styled) apply_style_export(*d.styled, eo);
  const std::vector<uint8_t> glb = write_glb(mesh, eo);
  std::ofstream(out / "validated.glb", std::ios::binary).write(reinterpret_cast<const char*>(glb.data()),
                                                              static_cast<std::streamsize>(glb.size()));
  const Dims dims = d.align->dims;
  j["model"] = {{"styled", d.styled != nullptr}, {"dims", {dims.W, dims.H, dims.D}},
                {"pixels_per_meter", eo.pixels_per_meter}, {"vertices", mesh.positions.size()},
                {"triangles", mesh.indices.size() / 3}, {"glb_bytes", glb.size()},
                {"texture", mesh.texture ? nlohmann::json{mesh.texture->width(), mesh.texture->height()} : nlohmann::json{}}};
  // Properties of the exported mesh that the check (and glTF) require.
  int bad_normals = 0, nonfinite = 0, uv_out = 0, degenerate = 0;
  double worst_normal = 0;
  for (size_t i = 0; i < mesh.positions.size(); ++i) {
    const vec3d& p = mesh.positions[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) ++nonfinite;
    if (i < mesh.normals.size()) {
      const double len = length(mesh.normals[i]);
      worst_normal = std::max(worst_normal, std::abs(len - 1));
      bad_normals += !(std::abs(len - 1) <= 1e-4);
    }
    if (i < mesh.uvs.size() && (mesh.uvs[i].x < 0 || mesh.uvs[i].x > 1 || mesh.uvs[i].y < 0 || mesh.uvs[i].y > 1)) ++uv_out;
  }
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const vec3d a = mesh.positions[mesh.indices[t]], b = mesh.positions[mesh.indices[t + 1]],
                c = mesh.positions[mesh.indices[t + 2]];
    degenerate += length(cross(b - a, c - a)) < 1e-12;
  }
  j["mesh_properties"] = {{"normals_not_unit", bad_normals}, {"worst_normal_length_error", worst_normal},
                          {"nonfinite_positions", nonfinite}, {"uvs_outside_0_1", uv_out},
                          {"degenerate_triangles", degenerate}};
  j["check_with_rerender"] = to_json(check_glb_roundtrip(glb, mesh, eo, dims));
  j["check_without_rerender"] = to_json(check_glb_roundtrip(glb, mesh, eo, std::nullopt));
  std::ofstream(out / "glbdiag.json") << j.dump(2) << "\n";
  std::cout << j.dump(2) << "\n";
  return j["check_with_rerender"]["ok"].get<bool>() ? 0 : 2;
}
