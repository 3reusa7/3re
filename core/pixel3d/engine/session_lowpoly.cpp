// LOW-POLY STUDIO session operations: settings and part types (undoable inputs),
// the part suggestion, the build (derived Document::lowpoly), renders and export.
#include <algorithm>
#include <cmath>

#include "pixel3d/common/file_io.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/engine/input_checks.hpp"
#include "pixel3d/engine/lowpoly_stage.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/lowpoly/engine_files.hpp"
#include "pixel3d/lowpoly/rig.hpp"
#include "pixel3d/lowpoly/style_render.hpp"
#include "pixel3d/lowpoly/suggest.hpp"

namespace pixel3d {

using pipeline::Stage;

namespace {

template <class F>
OpResult guarded(F&& fn) {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return OpResult::fail(Message{"internal.out_of_memory", "not enough memory for this operation", Severity::Error, {}});
  } catch (const std::exception& e) {
    return OpResult::fail(Message{"internal.exception", e.what(), Severity::Error, {}});
  }
}

Message wrong_mode() {
  return Message{"state.wrong_mode",
                 "not a Low-Poly Studio project: create one with new_project mode=lowpoly_studio, or switch this one with "
                 "set_lowpoly_settings enabled=true",
                 Severity::Error, {{"mode", "standard"}}};
}

nlohmann::json update_json(const LowpolyUpdate& u) {
  nlohmann::json j = nlohmann::json::object();
  if (u.enabled) j["enabled"] = *u.enabled;
  if (u.tri_budget) j["tri_budget"] = *u.tri_budget;
  if (u.joint_loops) j["joint_loops"] = *u.joint_loops;
  if (u.tones) j["tones"] = *u.tones;
  if (u.style) j["style"] = lowpoly::to_string(*u.style);
  if (u.templ) j["template"] = lowpoly::to_string(*u.templ);
  if (u.baked_cel) j["baked_cel"] = *u.baked_cel;
  if (u.hull) j["hull"] = *u.hull;
  if (u.hull_width) j["hull_width"] = *u.hull_width;
  if (u.accept_inferred_depth) j["accept_inferred_depth"] = *u.accept_inferred_depth;
  if (u.depth) j["depth"] = *u.depth;
  return j;
}

// Applies `u` to the document's settings; re-aligns when the mode or the front-only box changes.
std::optional<Message> apply_update(Document& d, const LowpolyUpdate& u, OpResult& r) {
  if (u.empty()) return std::nullopt;
  const LowpolyProjectSettings before = d.settings.lowpoly;
  LowpolyProjectSettings next = u.applied_to(before);
  if (auto e = check_lowpoly_settings(next)) return e;
  if (next.enabled && d.single_view.enabled) d.single_view.enabled = false;  // the modes exclude each other
  d.settings.lowpoly = next;
  if (next.enabled != before.enabled || next.build.depth != before.build.depth) pipeline::clear_from(d, Stage::Aligned);
  if (next.enabled != before.enabled)
    r.add(Message{"lowpoly.mode", next.enabled ? "project switched to Low-Poly Studio" : "project switched to the standard mode",
                  Severity::Info, {{"enabled", next.enabled}}});
  return std::nullopt;
}

lowpoly::Style shown_style(const lowpoly::Settings& s, std::optional<lowpoly::Style> asked) {
  const lowpoly::Style st = asked.value_or(s.style);
  if (!s.baked_cel) return st;
  // The ramp is in the texture: show it without shading it twice.
  return st == lowpoly::Style::CelOutline || st == lowpoly::Style::Outline ? lowpoly::Style::Outline : lowpoly::Style::Flat;
}

}  // namespace

LowpolyProjectSettings LowpolyUpdate::applied_to(LowpolyProjectSettings s) const {
  if (enabled) s.enabled = *enabled;
  if (tri_budget) s.build.tri_budget = *tri_budget;
  if (joint_loops) s.build.joint_loops = *joint_loops;
  if (tones) s.build.tones = *tones;
  if (style) s.build.style = *style;
  if (templ) s.build.templ = *templ;
  if (baked_cel) s.build.baked_cel = *baked_cel;
  if (hull) s.build.hull = *hull;
  if (hull_width) s.build.hull_width = *hull_width;
  if (accept_inferred_depth) s.build.accept_inferred_depth = *accept_inferred_depth;
  if (depth) {
    s.build.depth = *depth;
    s.build.user_depth = *depth > 0;  // a chosen depth is the user's decision; 0 returns to automatic
  }
  return s;
}

OpResult Session::set_lowpoly_settings(const LowpolyUpdate& u) {
  return apply("set_lowpoly_settings", update_json(u), Commit::Push, [&](Document& d) {
    OpResult r;
    if (u.empty()) return OpResult::fail(Message{"input.invalid", "no low-poly setting given", Severity::Error, {}});
    if (auto e = apply_update(d, u, r)) return OpResult::fail(*e);
    r.data["lowpoly"] = to_json(d.settings.lowpoly);
    r.success = true;
    r.changed = {"settings"};
    return r;
  });
}

OpResult Session::set_part_type(const PartTypeUpdate& u) {
  nlohmann::json params{{"part", u.part}};
  if (u.type) params["type"] = lowpoly::to_string(*u.type);
  if (u.depth_ratio) params["depth_ratio"] = *u.depth_ratio;
  return apply("set_part_type", params, Commit::Push, [&](Document& d) {
    if (!d.masks.parts.contains(u.part))
      return OpResult::fail(Message{"input.invalid", "no user part " + std::to_string(u.part), Severity::Error,
                                    {{"field", "part"}, {"value", u.part}}});
    if (!u.type && !u.depth_ratio)
      return OpResult::fail(Message{"input.invalid", "give a type or a depth_ratio", Severity::Error, {}});
    if (u.type) d.part_types[u.part] = *u.type;
    if (u.depth_ratio) {
      if (*u.depth_ratio == 0) d.settings.lowpoly.depth_ratio.erase(u.part);
      else d.settings.lowpoly.depth_ratio[u.part] = *u.depth_ratio;
      if (auto e = check_lowpoly_settings(d.settings.lowpoly)) return OpResult::fail(*e);
    }
    OpResult r;
    r.data["part"] = u.part;
    if (const auto t = d.part_types.find(u.part); t != d.part_types.end()) r.data["type"] = lowpoly::to_string(t->second);
    r.success = true;
    r.changed = {"parts"};
    return r;
  });
}

OpResult Session::lowpoly_suggest_parts(const LowpolySuggestRequest& req) {
  auto suggest_into = [&](Document& d, OpResult& r) -> std::optional<Message> {
    if (!d.settings.lowpoly.enabled) return wrong_mode();
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Segmented, {}, notes)) return e;
    for (const Message& m : notes) r.add(m);
    const SourceView* front = nullptr;
    const SourceView* side = nullptr;
    for (const SourceView& v : d.views) {
      if (v.role == ViewRole::Front) front = &v;
      if (v.role == ViewRole::Side) side = &v;
    }
    if (!front) return Message{"lowpoly.no_front", "low-poly studio needs a Front drawing", Severity::Error, {}};
    const lowpoly::Suggestion sg = lowpoly::suggest_parts(*front, *d.families, d.settings.alpha_threshold, nullptr, side);
    for (const Message& m : sg.messages) r.add(m);
    r.data["template"] = lowpoly::to_string(sg.templ);
    r.data["parts"] = nlohmann::json::array();
    std::vector<int> ids;
    if (req.apply) {
      const bool existing = !d.masks.parts.empty();
      if (existing && !req.replace)
        return Message{"lowpoly.parts_exist",
                       std::to_string(d.masks.parts.size()) +
                           " user part(s) exist: apply with replace=true to replace them by the suggestion",
                       Severity::Error, {{"parts", d.masks.parts.size()}}};
      if (existing)
        for (const auto& [id, up] : d.masks.parts) {
          d.part_names.erase(id);
          d.part_types.erase(id);
          d.settings.lowpoly.depth_ratio.erase(id);
        }
      d.masks.parts.clear();
      pipeline::clear_from(d, Stage::Reconstructed);
    }
    for (const lowpoly::Part& p : sg.parts) {
      nlohmann::json pj{{"name", p.name}, {"type", lowpoly::to_string(p.type)}};
      for (const auto& [role, m] : p.masks) {
        const auto b = m.bbox();
        pj["views"][std::string(to_string(role))] = {{"pixels", m.count()}, {"box", {b.x0, b.y0, b.x1, b.y1}}};
      }
      if (req.apply) {
        const int id = d.next_part_id++;
        UserPart up;
        up.masks = p.masks;
        d.masks.parts[id] = std::move(up);
        d.part_names[id] = p.name;
        d.part_types[id] = p.type;
        pj["id"] = id;
        ids.push_back(id);
      }
      r.data["parts"].push_back(std::move(pj));
    }
    r.data["applied"] = req.apply;
    if (req.apply) r.data["part_ids"] = ids;
    if (!req.apply)
      r.add(Message{"lowpoly.suggestion", "suggested parts are proposals: nothing was changed (apply=true creates them)",
                    Severity::Info, {{"parts", sg.parts.size()}}});
    return std::nullopt;
  };
  if (!req.apply) {
    // Proposals only: computed on a copy, nothing is committed or logged as a change.
    return guarded([&] {
      Document d = history_.current();
      OpResult r;
      if (auto e = suggest_into(d, r)) return OpResult::fail(*e);
      r.success = true;
      finish(r);
      return r;
    });
  }
  return apply("lowpoly_suggest_parts", {{"apply", true}, {"replace", req.replace}}, Commit::Push, [&](Document& d) {
    OpResult r;
    if (auto e = suggest_into(d, r)) return OpResult::fail(*e);
    r.success = true;
    r.changed = {"parts"};
    return r;
  });
}

OpResult Session::lowpoly_build(const LowpolyUpdate& u, const JobContext& job) {
  return apply("lowpoly_build", update_json(u), u.empty() ? Commit::Replace : Commit::Push, [&](Document& d) {
    OpResult r;
    if (auto e = apply_update(d, u, r)) return OpResult::fail(*e);
    if (!d.settings.lowpoly.enabled) return OpResult::fail(wrong_mode());
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Segmented, job, notes)) return OpResult::fail(*e);
    if (job.stop.stop_requested()) return OpResult::fail(Message{"job.cancelled", "cancelled", Severity::Error, {}});
    if (auto e = build_lowpoly_stage(d, notes)) return OpResult::fail(*e);
    for (const Message& m : notes) r.add(m);
    r.data["lowpoly"] = lowpoly_report(*d.lowpoly);
    r.success = true;
    r.changed = {"lowpoly"};
    return r;
  });
}

OpResult Session::lowpoly_render(const LowpolyRenderRequest& req, std::vector<NamedImage>* out) const {
  return guarded([&] {
    const Document& d = history_.current();
    const LowpolyBuild* b = current_lowpoly(d);
    if (!b)
      return OpResult::fail(Message{"state.not_ready", "no low-poly model for the current inputs: run lowpoly_build",
                                    Severity::Error, {}});
    if (req.yaws.empty() || req.yaws.size() > 72)
      return OpResult::fail(Message{"input.invalid", "give 1 to 72 yaw angles", Severity::Error, {{"field", "yaws"}}});
    if (req.size < 16 || req.size > 2048)
      return OpResult::fail(Message{"input.invalid", "size must be in [16, 2048]", Severity::Error, {{"field", "size"}}});
    const lowpoly::SkinnedMesh& m = b->result.mesh();
    lowpoly::PoseResult pr;
    lowpoly::Pose pose;
    if (req.pose) {
      pose = lowpoly::test_pose(b->result.layout);
      pr = lowpoly::apply_pose(m, b->result.layout.skeleton, pose);
    }
    // One frame for every image of the call (rest and pose share the scale).
    vec3d lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (const std::vector<vec3d>* ps : {&m.positions, static_cast<const std::vector<vec3d>*>(&pr.positions)})
      for (const vec3d& p : *ps) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
      }
    lowpoly::StyleRenderOptions o;
    o.width = o.height = req.size;
    o.style = shown_style(b->input.settings, req.style);
    o.tones = b->input.settings.tones;
    o.show_wire = req.wire;
    OpResult r;
    std::vector<NamedImage> frames;
    r.data["images"] = nlohmann::json::array();
    for (double yaw : req.yaws) {
      char name[32];
      std::snprintf(name, sizeof name, "%s_%03d", req.pose ? "pose" : "yaw", static_cast<int>(std::lround(yaw)));
      ImageRGBA img = req.pose ? lowpoly::render_style(m, b->result.bake.texture.get(), &pr.positions, &pr.normals, yaw,
                                                       req.elevation_deg, lo, hi, o)
                               : lowpoly::render_style(m, b->result.bake.texture.get(), nullptr, nullptr, yaw,
                                                       req.elevation_deg, lo, hi, o);
      r.data["images"].push_back({{"name", name}, {"yaw", yaw}});
      frames.push_back(NamedImage{name, std::move(img)});
    }
    if (req.pose) {
      r.data["pose"] = pose.name;
      r.data["pose_notes"] = pose.notes;
    }
    if (req.contact_sheet) {
      ImageRGBA sheet(req.size * static_cast<int>(frames.size()), req.size);
      for (size_t i = 0; i < frames.size(); ++i)
        for (int y = 0; y < req.size; ++y)
          for (int x = 0; x < req.size; ++x) sheet.at(static_cast<int>(i) * req.size + x, y) = frames[i].image.at(x, y);
      frames.push_back(NamedImage{"contact_sheet", std::move(sheet)});
      r.data["contact_sheet"] = {{"name", "contact_sheet"}, {"columns", frames.size() - 1}};
    }
    r.data["style"] = lowpoly::to_string(o.style);
    r.data["triangles"] = b->result.triangles;
    if (out)
      for (NamedImage& f : frames) out->push_back(std::move(f));
    r.success = true;
    finish(r);
    return r;
  });
}

OpResult Session::lowpoly_export(const LowpolyExportRequest& req) {
  return apply("lowpoly_export", {{"path", to_utf8(req.path)}, {"allow_needs_review", req.allow_needs_review}},
               Commit::Replace, [&](Document& d) {
                 OpResult r;
                 if (!d.settings.lowpoly.enabled) return OpResult::fail(wrong_mode());
                 if (req.path.empty())
                   return OpResult::fail(Message{"input.invalid", "give the GLB path", Severity::Error, {{"field", "path"}}});
                 pipeline::Notes notes;
                 if (auto e = pipeline::ensure(d, Stage::Segmented, {}, notes)) return OpResult::fail(*e);
                 if (auto e = build_lowpoly_stage(d, notes)) return OpResult::fail(*e);
                 const LowpolyBuild& b = *d.lowpoly;
                 const ValidationStatus st = b.validation.status;
                 r.data["lowpoly"] = lowpoly_summary(b);
                 if (st == ValidationStatus::Fail)
                   return OpResult::fail(Message{"lowpoly.export_refused",
                                                 "the low-poly model FAILS a critical check and is never exported",
                                                 Severity::Error, {{"status", to_string(st)}}});
                 if (st != ValidationStatus::Pass && !b.decision.allowed && !req.allow_needs_review)
                   return OpResult::fail(Message{"lowpoly.export_refused",
                                                 "the low-poly model is NEEDS_REVIEW: " + b.decision.reason +
                                                     ". Export only when the user accepts it (allow_needs_review)",
                                                 Severity::Error, {{"status", to_string(st)}}});
                 if (st != ValidationStatus::Pass)
                   r.add(Message{"export.status_accepted",
                                 "exported with status NEEDS_REVIEW (" +
                                     std::string(b.decision.allowed ? b.decision.reason : "accepted by the caller") + ")",
                                 Severity::Warning, {{"status", to_string(st)}}});
                 std::vector<std::pair<std::filesystem::path, std::string>> texts;
                 if (req.engine_files) {
                   lowpoly::EngineStyle es{b.input.settings.style, b.input.settings.tones, b.input.settings.baked_cel,
                                           b.export_options.outline_width_px, to_utf8(req.path.filename())};
                   const auto stem = req.path.parent_path() / req.path.stem();
                   texts.push_back({path_from_utf8(to_utf8(stem) + "_toon.gdshader"), lowpoly::godot_toon_shader(es)});
                   texts.push_back({path_from_utf8(to_utf8(stem) + "_outline.gdshader"), lowpoly::godot_outline_shader(es)});
                   texts.push_back({path_from_utf8(to_utf8(stem) + "_unity.md"), lowpoly::unity_setup_notes(es)});
                 }
                 const Message w = write_file_atomic(req.path, b.glb);
                 if (w.code != "ok") return OpResult::fail(w);
                 nlohmann::json files = nlohmann::json::array({to_utf8(req.path)});
                 for (const auto& [p, text] : texts) {
                   const Message wt = write_file_atomic(p, text);
                   if (wt.code != "ok") return OpResult::fail(wt);
                   files.push_back(to_utf8(p));
                 }
                 r.data["files"] = files;
                 r.data["bytes"] = b.glb.size();
                 r.data["glb"] = lowpoly::to_json(b.check);
                 r.success = true;
                 r.changed = {"export"};
                 return r;
               });
}

}  // namespace pixel3d
