#include "pixel3d/engine/lowpoly_stage.hpp"

#include "pixel3d/common/sha256.hpp"
#include "pixel3d/common/version.hpp"
#include "pixel3d/io/json_io.hpp"

namespace pixel3d {

std::string lowpoly_input_hash(const Document& d) {
  nlohmann::json j = inputs_to_json(d);
  // Inputs of the standard model only.
  for (const char* k : {"mode", "cleanup", "texture", "decisions", "depth_constraints", "edits", "texture_assignments",
                        "single_view"})
    j.erase(k);
  j["settings"].erase("name");
  j["settings"].erase("profile");
  j["settings"].erase("style");
  for (auto& v : j["views"]) v.erase("original_path");
  j["engine"] = std::string(version());
  return sha256_hex(j.dump());
}

Expected<lowpoly::Input> lowpoly_input(const Document& d, std::vector<Message>* notes) {
  if (!d.align) return make_error("state.not_ready", "align the views first");
  lowpoly::Input in;
  in.views = d.views;
  in.dims = d.align->dims;
  in.alpha_threshold = d.settings.alpha_threshold;
  in.settings = d.settings.lowpoly.build;
  std::vector<int> untyped;
  for (const auto& [id, up] : d.masks.parts) {
    bool any = false;
    for (const auto& [role, m] : up.masks) any |= m.count() > 0;
    if (!any) {
      if (notes)
        notes->push_back(Message{"lowpoly.part_empty", "part " + std::to_string(id) + " has no mask pixels and is not built",
                                 Severity::Warning, {{"part", id}}});
      continue;
    }
    const auto t = d.part_types.find(id);
    if (t == d.part_types.end()) {
      untyped.push_back(id);
      continue;
    }
    lowpoly::Part p;
    p.id = id;
    p.type = t->second;
    const auto n = d.part_names.find(id);
    p.name = n != d.part_names.end() ? n->second : "part " + std::to_string(id);
    for (const auto& [role, m] : up.masks)
      if (m.count() > 0) p.masks.emplace(role, m);
    p.depth = up.depth;
    if (const auto r = d.settings.lowpoly.depth_ratio.find(id); r != d.settings.lowpoly.depth_ratio.end())
      p.depth_ratio = r->second;
    in.parts.push_back(std::move(p));
  }
  if (!untyped.empty())
    return make_error("lowpoly.part_type_missing",
                      std::to_string(untyped.size()) +
                          " part(s) have no type (head, torso, limb, accessory or cloth): set_part_type, or accept "
                          "lowpoly_suggest_parts",
                      {{"parts", untyped}});
  if (in.parts.empty())
    return make_error("lowpoly.no_parts",
                      "no typed parts: run lowpoly_suggest_parts (apply) or paint part masks and give them types");
  return in;
}

const LowpolyBuild* current_lowpoly(const Document& d) {
  return d.lowpoly && d.lowpoly->key == lowpoly_input_hash(d) ? d.lowpoly.get() : nullptr;
}

std::optional<Message> build_lowpoly_stage(Document& d, std::vector<Message>& notes) {
  const std::string key = lowpoly_input_hash(d);
  if (d.lowpoly && d.lowpoly->key == key) return std::nullopt;
  d.lowpoly.reset();
  auto in = lowpoly_input(d, &notes);
  if (!in) return in.error();
  auto res = lowpoly::build_lowpoly(*in);
  if (!res) return res.error();
  auto b = std::make_shared<LowpolyBuild>();
  b->key = key;
  b->input = std::move(*in);
  b->result = std::move(*res);
  lowpoly::SkinnedExportOptions& eo = b->export_options;
  eo.frame.pixels_per_meter = d.settings.pixels_per_meter;
  eo.style = b->input.settings.style;
  eo.tones = b->input.settings.tones;
  eo.baked_cel = b->input.settings.baked_cel;
  eo.report = {{"template", lowpoly::to_string(b->result.layout.templ)},
               {"budget", b->input.settings.tri_budget},
               {"triangles", b->result.triangles},
               {"engine", std::string(version())}};
  b->glb = lowpoly::write_skinned_glb(b->result.mesh(), b->result.layout.skeleton, *b->result.bake.texture, eo);
  b->check = lowpoly::check_skinned_glb(b->glb, b->result.mesh(), b->result.layout.skeleton, eo);
  b->validation = lowpoly::validate_lowpoly(b->input, b->result, &b->check);
  b->decision = lowpoly::export_decision(b->validation, b->input.settings);
  for (const Message& m : b->result.messages) notes.push_back(m);
  d.lowpoly = std::move(b);
  return std::nullopt;
}

nlohmann::json lowpoly_report(const LowpolyBuild& b) {
  nlohmann::json j = lowpoly::to_json(b.validation);
  for (const auto& [role, iou] : b.validation.silhouette_iou) j["silhouette_iou"][std::string(to_string(role))] = iou;
  j["glb"] = lowpoly::to_json(b.check);
  j["glb_bytes"] = b.glb.size();
  j["export"] = {{"allowed", b.decision.allowed},
                 {"accepted_inferred_depth", b.decision.accepted_inferred_depth},
                 {"reason", b.decision.reason}};
  j["views"] = nlohmann::json::array();
  for (const SourceView& v : b.input.views) j["views"].push_back(to_string(v.role));
  j["dims"] = {b.input.dims.W, b.input.dims.H, b.input.dims.D};
  j["joint_volume"] = nlohmann::json::object();
  for (const auto& [name, ratio] : b.validation.rig.joint_volume) j["joint_volume"][name] = ratio;
  j["messages"] = nlohmann::json::array();
  for (const Message& m : b.result.messages) j["messages"].push_back({{"code", m.code}, {"text", m.text}, {"where", m.where}});
  return j;
}

nlohmann::json lowpoly_summary(const LowpolyBuild& b) {
  nlohmann::json blocking = nlohmann::json::array(), failed = nlohmann::json::array();
  for (const auto& c : b.validation.conflicts)
    if (c.blocking) blocking.push_back({{"kind", c.kind}, {"text", c.text}});
  for (const Check& c : b.validation.checks)
    if (!c.passed && c.critical) failed.push_back(c.name);
  return {{"status", to_string(b.validation.status)},
          {"mode", "lowpoly_studio"},
          {"triangles", b.result.triangles},
          {"budget", b.input.settings.tri_budget},
          {"template", lowpoly::to_string(b.result.layout.templ)},
          {"failed_critical_checks", failed},
          {"blocking_conflicts", blocking},
          {"export_allowed", b.decision.allowed}};
}

}  // namespace pixel3d
