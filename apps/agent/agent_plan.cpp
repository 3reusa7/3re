#include "agent_plan.hpp"

#include "pixel3d/common/path_utf8.hpp"

namespace p3dagent {

using nlohmann::json;

bool Capabilities::has(const std::string& tool) const {
  const auto it = tools.find(tool);
  return it != tools.end() && it->second.present && it->second.verified;
}

bool Capabilities::can_generate(std::string* why) const {
  std::string reason;
  for (const char* t : {"engine", "post_safe", "studio"})
    if (!has(t)) {
      const auto it = tools.find(t);
      reason = std::string(t) + (it == tools.end() || !it->second.present ? " is missing" : " failed its SHA-256 pin");
      break;
    }
  if (reason.empty() && !weights_verified) reason = "TRELLIS weights not verified: " + weights_note;
  if (reason.empty() && !gpu_present) reason = "no CUDA-capable GPU found";
  if (why) *why = reason;
  return reason.empty();
}

json Capabilities::to_json() const {
  json t = json::object();
  for (const auto& [name, info] : tools)
    t[name] = {{"path", pixel3d::to_utf8(info.path)}, {"present", info.present}, {"verified", info.verified},
               {"sha256", info.sha256}, {"expected_sha256", info.expected_sha256}, {"version", info.version},
               {"note", info.note}};
  std::string why;
  const bool gen = can_generate(&why);
  return {{"ram_total_mib", ram_total_mib}, {"ram_free_mib", ram_free_mib}, {"disk_free_mib", disk_free_mib},
          {"gpu", {{"present", gpu_present}, {"name", gpu_name}, {"vram_mib", vram_mib}}},
          {"weights_verified", weights_verified}, {"weights_note", weights_note},
          {"can_generate", gen}, {"cannot_generate_because", why}, {"tools", t}};
}

bool Plan::has(const std::string& stage) const {
  for (const auto& s : steps)
    if (s.stage == stage) return true;
  return false;
}

json Plan::to_json() const {
  json s = json::array();
  for (const auto& p : steps) s.push_back({{"stage", p.stage}, {"reason", p.reason}});
  return {{"blocked", blocked}, {"blocked_reason", blocked_reason}, {"steps", s}, {"texture_source", texture_source},
          {"texture_picture", texture_picture.empty() ? json(nullptr) : json(pixel3d::to_utf8(texture_picture))},
          {"notes", notes}};
}

Plan make_plan(const AssetSpec& spec, const Capabilities& caps, const InputFacts& f) {
  Plan p;
  auto add = [&](const char* stage, std::string reason) { p.steps.push_back({stage, std::move(reason)}); };
  auto block = [&](std::string why) {
    p.blocked = true;
    p.blocked_reason = std::move(why);
    return p;
  };
  const bool native = !spec.native_texture.empty();

  switch (spec.kind) {
    case InputKind::Picture: {
      if (!f.picture_exists) return block("picture not found: " + pixel3d::to_utf8(spec.picture));
      std::string why;
      if (!caps.can_generate(&why)) return block("cannot generate from the picture: " + why);
      add("generate", std::string("TRELLIS.2 through the 3re wizard (geometry + TRELLIS texture, GeometrySafe, audit, ") +
                          (f.picture_has_alpha ? "the picture's own transparency as the cutout)"
                                               : "no transparency: the wizard's automatic background removal makes the cutout)"));
      p.texture_source = "hybrid";
      break;
    }
    case InputKind::ReuseRun: {
      if (!f.run_exists || !f.run_has_geometry)
        return block("saved run has no geometry.glb: " + pixel3d::to_utf8(spec.reuse_run));
      add("reuse_run", "saved wizard run: generation skipped");
      const auto& post = caps.tools.count("post_safe") ? caps.tools.at("post_safe") : ToolInfo{};
      const auto pinned = post.expected_sha256.empty() ? post.sha256 : post.expected_sha256;
      if (!pinned.empty() && f.run_cleanup_sha256 != pinned) {
        if (f.run_has_raw_ply && caps.has("post_safe"))
          add("cleanup", "the run was cleaned by another cleanup (" + f.run_cleanup_sha256.substr(0, 8) +
                             "), not the pinned GeometrySafe (" + pinned.substr(0, 8) + "): re-run it on raw.ply");
        else p.notes.push_back("the run's cleanup is not the pinned GeometrySafe and raw.ply or post-safe is unavailable");
      }
      if (f.run_has_cutout) p.texture_picture = f.run_cutout.empty() ? spec.reuse_run / "texture-source.png" : f.run_cutout;
      else if (f.picture_exists && f.picture_has_alpha) p.texture_picture = spec.picture;
      if (!p.texture_picture.empty()) p.texture_source = native ? "hybrid" : "picture";
      else p.notes.push_back("no cutout picture for re-texturing: a decimated mesh cannot be re-textured");
      if (f.picture_exists && !f.picture_has_alpha && f.run_has_cutout)
        p.notes.push_back("the spec's picture has no transparency: the run's saved cutout (" +
                          pixel3d::to_utf8(p.texture_picture.filename()) + ") is used");
      break;
    }
    case InputKind::Glb: {
      if (!f.glb_exists) return block("GLB not found: " + pixel3d::to_utf8(spec.glb));
      add("import_glb", "GLB from another generator: generation skipped");
      p.texture_source = f.glb_textured ? "keep" : "none";
      if (f.picture_exists && f.picture_has_alpha) p.texture_picture = spec.picture;
      break;
    }
  }
  add("inspect", "topology + exact self-intersection audit (pixel3d-asset-inspect, pixel3d-geometry-audit)");
  add("evaluate", "triangle budget, scale/orientation, materials/UV, texture checks; recovery decides from these numbers");
  add("deliver", "Godot preset GLB: scaled to the spec's metres and grounded (baked into the vertices, or a root node for files with their own hierarchy)");
  add("khronos", caps.has("validator") ? "Khronos glTF validator, 0 errors required" : "Khronos validator missing: UNVERIFIED");
  add("reload", "independent reload (pixel3d-glbcheck, tinygltf)");
  add("godot_import", "real Godot headless import: triangles, AABB/scale, up axis, materials");
  add("report", "report.json + report.md with SHA-256 of every deliverable");
  if (!caps.has("godot")) p.notes.push_back("Godot not found: the Godot import is UNVERIFIED and the asset cannot PASS");
  if (!caps.has("validator")) p.notes.push_back("Khronos validator not found: validation is UNVERIFIED");
  if (!caps.has("decimate")) p.notes.push_back("pixel3d-decimate-guarded not found: an over-budget mesh cannot be reduced");
  return p;
}

}  // namespace p3dagent
