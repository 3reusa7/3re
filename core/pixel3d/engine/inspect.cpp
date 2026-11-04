#include "pixel3d/engine/lowpoly_stage.hpp"
#include "pixel3d/engine/inspect.hpp"

#include <map>

#include "pixel3d/common/limits.hpp"
#include "pixel3d/common/version.hpp"
#include "pixel3d/validate/report.hpp"

namespace pixel3d {
namespace {

nlohmann::json box_json(const Mask2D::Box& b) { return {b.x0, b.y0, b.x1, b.y1}; }

nlohmann::json settings_json(const ProjectSettings& s) {
  return {{"name", s.name},
          {"alpha_threshold", s.alpha_threshold},
          {"pixels_per_meter", s.pixels_per_meter},
          {"profile", to_string(s.profile)},
          {"simplify", {{"angle_snap", s.simplify.angle_snap}, {"min_unit_steps", s.simplify.min_unit_steps}}},
          {"normalize", to_json(s.normalize)},
          {"style", to_json(s.style)}};
}

nlohmann::json topology_json(const TopologyReport& t) {
  return {{"watertight", t.watertight},
          {"boundary_edges", t.boundary_edges},
          {"non_manifold_edges", t.nonmanifold_edges},
          {"non_manifold_vertices", t.nonmanifold_vertices},
          {"inconsistent_winding_edges", t.inconsistent_winding_edges},
          {"duplicate_faces", t.duplicate_faces},
          {"internal_faces", t.internal_faces},
          {"zero_area_faces", t.zero_area_faces},
          {"shells", t.shells},
          {"volume", t.signed_volume},
          {"triangles", t.triangle_count},
          {"vertices", t.vertex_count},
          {"planar_regions", t.planar_regions}};
}

}  // namespace

nlohmann::json inspect_project(const Session& s) {
  const Document& d = s.document();
  nlohmann::json j;
  j["engine_version"] = std::string(version());
  j["settings"] = settings_json(d.settings);
  j["stage"] = std::string(pipeline::to_string(pipeline::stage_of(d)));
  j["revision"] = d.revision;
  j["mode"] = std::string(to_string(d.mode));
  j["can_undo"] = s.can_undo();
  j["can_redo"] = s.can_redo();
  j["checkpoints"] = s.checkpoint_names();
  j["views"] = nlohmann::json::array();
  for (const SourceView& v : d.views) j["views"].push_back(to_string(v.role));
  if (d.align) j["dims"] = {d.align->dims.W, d.align->dims.H, d.align->dims.D};
  j["decisions"] = d.decisions.size();
  j["depth_constraints"] = d.depth_constraints.size();
  j["texture_assignments"] = d.texture_assignments.size();
  j["edits"] = d.edits.size();
  j["user_parts"] = d.masks.parts.size();
  j["single_view"] = d.single_view.enabled;
  j["lowpoly_studio"] = d.settings.lowpoly.enabled;
  if (d.settings.lowpoly.enabled) {
    const LowpolyBuild* b = current_lowpoly(d);
    j["lowpoly"] = b ? lowpoly_summary(*b) : nlohmann::json{{"status", "NOT_BUILT"}};
  }
  j["parts"] = parts_json(d);
  if (d.solid)
    j["mesh"] = {{"triangles", d.solid->topology.triangle_count},
                 {"watertight", d.solid->topology.watertight},
                 {"volume", d.solid->topology.signed_volume}};
  j["textured"] = d.texture != nullptr;
  j["status"] = std::string(to_string(d.gate ? d.gate->status : ValidationStatus::NotValidated));
  if (d.gate) {
    std::vector<std::string> blocking;
    if (d.conflicts)
      for (const Conflict& c : *d.conflicts)
        if (c.blocks_pass) blocking.push_back(c.kind);
    j["blocking_conflicts"] = blocking;
  }
  return j;
}

nlohmann::json inspect_views(const Document& d) {
  nlohmann::json j;
  j["aligned"] = d.align != nullptr;
  if (d.align) {
    j["dims"] = {{"W", d.align->dims.W}, {"H", d.align->dims.H}, {"D", d.align->dims.D}};
    j["alignment_messages"] = d.align->messages;
  }
  j["coordinates"] =
      "region coordinates are pixels of the aligned view image (u right, v down); Front/Back are W x H, "
      "Side/Right D x H, Top/Bottom W x D. Top: front edge at the bottom.";
  j["views"] = nlohmann::json::array();
  for (const SourceView& v : d.views) {
    nlohmann::json vj{{"role", to_string(v.role)},
                      {"path", v.original_path},
                      {"sha256", v.sha256},
                      {"original_size", {v.original ? v.original->width() : 0, v.original ? v.original->height() : 0}}};
    if (v.aligned) {
      vj["aligned_size"] = {v.aligned->width(), v.aligned->height()};
      vj["scale"] = v.scale.k;
      vj["crop"] = box_json(v.crop);
      vj["padding"] = {v.pad_x, v.pad_y};
      vj["flipped"] = v.flipped_x;
      vj["flip_forced"] = v.flip_user_override;
      vj["normalization"] = normalization_json(v);
    }
    if (d.align) {
      if (const auto it = d.align->flip_confidence.find(v.role); it != d.align->flip_confidence.end())
        vj["flip_confidence"] = it->second;
      if (const auto it = d.align->profile_conflicts.find(v.role); it != d.align->profile_conflicts.end())
        vj["unexplained_px"] = it->second;
    }
    j["views"].push_back(vj);
  }
  if (d.families) {
    j["material_families"] = d.families->family_count;
    j["outline_family"] = d.families->outline_family;
    nlohmann::json pal = nlohmann::json::array();
    for (const PaletteEntry& e : d.families->entries) {
      char hex[8];
      std::snprintf(hex, sizeof hex, "#%02x%02x%02x", e.color.r, e.color.g, e.color.b);
      pal.push_back({{"color", hex}, {"count", e.count}, {"family", e.family}});
    }
    j["palette"] = pal;
  }
  return j;
}

nlohmann::json inspect_parts(const Document& d) {
  nlohmann::json j;
  j["matched"] = d.candidates != nullptr;
  j["bands"] = nlohmann::json::array();
  if (d.bands)
    for (const Band& b : d.bands->bands) {
      nlohmann::json bj{{"id", b.id}, {"y0", b.y0}, {"y1", b.y1}};
      for (const BandView& bv : b.views) {
        nlohmann::json strands = nlohmann::json::array();
        for (const Strand& s : bv.strands) strands.push_back({s.a, s.b});
        bj["strands"][std::string(to_string(bv.role))] = strands;
      }
      j["bands"].push_back(bj);
    }
  j["candidates"] = nlohmann::json::array();
  if (d.candidates)
    for (const Candidate& c : d.candidates->cands) {
      const int part = d.correspondence && c.id < static_cast<int>(d.correspondence->part_of_candidate.size())
                           ? d.correspondence->part_of_candidate[c.id]
                           : -1;
      j["candidates"].push_back({{"id", c.id},
                                 {"band", c.band},
                                 {"front_atom", c.front_atom},
                                 {"side_atom", c.side_atom},
                                 {"footprint_xz", box_json(c.footprint)},
                                 {"volume", c.volume},
                                 {"status", to_string(c.status)},
                                 {"reason", c.reason},
                                 {"part", part}});
    }
  j["parts"] = nlohmann::json::array();
  if (d.correspondence) {
    for (const Part3D& p : d.correspondence->parts)
      j["parts"].push_back({{"id", p.id}, {"label", p.label}, {"status", to_string(p.status)}, {"candidates", p.candidates}});
    j["inferred_volume"] = d.correspondence->inferred_volume;
    j["ambiguous_volume"] = d.correspondence->ambiguous_volume;
  }
  j["model_parts"] = parts_json(d);  // every part of the model, including user and edit-created parts
  j["decisions"] = nlohmann::json::array();
  for (const UserDecision& u : d.decisions) j["decisions"].push_back({{"candidate", u.candidate}, {"keep", u.keep}});
  j["depth_constraints"] = nlohmann::json::array();
  for (size_t i = 0; i < d.depth_constraints.size(); ++i) {
    const DepthConstraint& c = d.depth_constraints[i];
    j["depth_constraints"].push_back({{"index", i},
                                      {"view", to_string(c.view)},
                                      {"depth_from", c.depth_from},
                                      {"depth_to", c.depth_to},
                                      {"label", c.label},
                                      {"region_area_px", area(c.region)}});
  }
  return j;
}

nlohmann::json inspect_mesh(const Document& d) {
  nlohmann::json j;
  j["reconstructed"] = d.solid != nullptr;
  if (!d.solid) return j;
  // The polished model (built at validation) is what is validated and exported.
  if (d.styled) {
    j["style"] = to_json(*d.styled);
    j["style"]["topology"] = topology_json(d.styled->solid.topology);
  } else if (d.settings.style.preset != StylePreset::Crisp) {
    j["style"] = {{"style", to_string(d.settings.style.preset)}, {"built", false}};
  }
  j["mode"] = std::string(to_string(d.mode));
  j["topology"] = topology_json(d.solid->topology);
  const CleanupReport& c = d.solid->cleanup;
  j["cleanup"] = {{"welded", c.welded},
                  {"removed_zero_area", c.removed_zero_area},
                  {"removed_duplicates", c.removed_duplicates},
                  {"removed_internal", c.removed_internal},
                  {"reoriented_faces", c.reoriented_faces},
                  {"flipped_shells", c.flipped_shells},
                  {"removed_fragments", c.removed_fragments},
                  {"kept_shell_volumes", c.kept_shell_volumes}};
  j["from_project_cache"] = d.solid == d.cached_solid;
  std::map<int, int> per_part;
  for (int p : d.solid->mesh.tri_part) ++per_part[p];
  nlohmann::json parts = nlohmann::json::object();
  for (const auto& [p, n] : per_part) parts[std::to_string(p)] = n;
  j["triangles_per_part"] = parts;
  if (d.align) j["bbox_units"] = {d.align->dims.W, d.align->dims.H, d.align->dims.D};
  j["bbox_meters"] = d.align ? nlohmann::json{d.align->dims.W / d.settings.pixels_per_meter,
                                              d.align->dims.H / d.settings.pixels_per_meter,
                                              d.align->dims.D / d.settings.pixels_per_meter}
                             : nlohmann::json();
  if (d.texture) {
    nlohmann::json t;
    for (const auto& [mode, a] : d.texture->area_by_mode) t["area_by_mode"][std::string(to_string(mode))] = a;
    std::map<std::string, int> faces;
    for (const FaceTexture& f : d.texture->faces) ++faces[std::string(to_string(f.mode))];
    t["faces_by_mode"] = faces;
    t["atlas_size"] = {d.texture->atlas.image->width(), d.texture->atlas.image->height()};
    t["texel_density"] = {d.texture->texel_density_min, d.texture->texel_density_max};
    t["render_vertices"] = d.texture->mesh.positions.size();
    j["texture"] = t;
  }
  return j;
}

nlohmann::json inspect_conflicts(const Session& s) {
  const OpResult r = s.get_conflicts();
  nlohmann::json j = r.data;
  j["status"] = std::string(to_string(r.status));
  if (!r.validation.is_null()) j["validation"] = r.validation;
  return j;
}

nlohmann::json inspect_diagnostics(const Session& s, size_t log_tail) {
  nlohmann::json j;
  j["engine_version"] = std::string(version());
  j["limits"] = {{"max_image_file_bytes", limits::kMaxImageFileBytes},
                 {"max_image_side", limits::kMaxImageSide},
                 {"max_model_side", limits::kMaxModelSide},
                 {"max_model_cells", limits::kMaxModelCells},
                 {"max_render_side", limits::kMaxRenderSide},
                 {"max_turntable_frames", limits::kMaxTurntableFrames},
                 {"max_project_file_bytes", limits::kMaxProjectFileBytes},
                 {"max_checkpoints", limits::kMaxCheckpoints}};
  const auto& log = s.log();
  j["log_entries"] = log.size();
  j["log"] = nlohmann::json::array();
  for (size_t i = log.size() > log_tail ? log.size() - log_tail : 0; i < log.size(); ++i) j["log"].push_back(log[i]);
  const Document& d = s.document();
  if (d.align) j["alignment_messages"] = d.align->messages;
  if (d.texture) j["texture_messages"] = d.texture->messages;
  return j;
}

nlohmann::json inspect_validation(const Document& d) {
  if (!d.gate || !d.metrics || !d.conflicts || !d.correspondence)
    return {{"status", "NOT_VALIDATED"},
            {"revision", d.revision},
            {"text", "run validate: the document changed since the last validation, or was never validated"}};
  nlohmann::json j = make_report(*d.gate, *d.metrics, *d.conflicts, *d.correspondence,
                                 {{"project", d.settings.name}, {"mode", to_string(d.mode)}}, d.gate->profile);
  j["revision"] = d.revision;
  return j;
}

nlohmann::json inspect_live(const Document& d) {
  if (!d.live) return {{"available", false}, {"text", "no textured model yet"}};
  const QualityMetrics& m = *d.live;
  nlohmann::json views = nlohmann::json::object();
  for (const auto& [role, v] : m.views)
    views[std::string(to_string(role))] = {{"iou", v.iou},
                                           {"mismatch_px", v.mismatch_px},
                                           {"color_exact_fraction", v.color_exact_fraction}};
  const TopologyReport& t = m.topology;
  nlohmann::json masks = nlohmann::json::array();
  if (d.mask_conflicts)
    for (const MaskConflict& c : *d.mask_conflicts)
      masks.push_back({{"part", c.part},
                       {"view", to_string(c.view)},
                       {"unmatched_px", c.unmatched_px},
                       {"mask_px", c.mask_px},
                       {"box", {c.box.x0, c.box.y0, c.box.x1, c.box.y1}}});
  return {{"available", true},
          {"views", views},
          {"watertight", t.watertight},
          {"non_manifold_edges", t.nonmanifold_edges},
          {"non_manifold_vertices", t.nonmanifold_vertices},
          {"shells", t.shells},
          {"triangles", t.triangle_count},
          {"volume", t.signed_volume},
          {"inferred_volume_fraction", m.inferred_volume_fraction},
          {"ambiguous_volume_fraction", m.ambiguous_volume_fraction},
          {"texture", m.texture},
          {"mask_conflicts", masks},
          {"status", "NOT_VALIDATED until validate runs"}};
}

nlohmann::json inspect_lowpoly(const Document& d) {
  nlohmann::json j;
  j["enabled"] = d.settings.lowpoly.enabled;
  j["settings"] = to_json(d.settings.lowpoly);
  j["parts"] = nlohmann::json::array();
  for (const auto& [id, up] : d.masks.parts) {
    nlohmann::json p{{"id", id}};
    if (const auto n = d.part_names.find(id); n != d.part_names.end()) p["name"] = n->second;
    if (const auto t = d.part_types.find(id); t != d.part_types.end()) p["type"] = lowpoly::to_string(t->second);
    else p["type"] = nullptr;
    for (const auto& [role, m] : up.masks)
      if (const int n = m.count(); n > 0) p["pixels"][std::string(to_string(role))] = n;
    j["parts"].push_back(std::move(p));
  }
  const LowpolyBuild* b = current_lowpoly(d);
  j["built"] = b != nullptr;
  if (b) j["report"] = lowpoly_report(*b);
  return j;
}

nlohmann::json inspect_edits(const Document& d) {
  nlohmann::json edits = nlohmann::json::array();
  for (size_t i = 0; i < d.edits.size(); ++i) {
    nlohmann::json e = edit_to_json(d.edits[i]);
    e["index"] = i;
    e["name"] = edit_name(d.edits[i]);
    if (e.contains("samples")) {
      e["sample_count"] = e["samples"].size();
      e.erase("samples");  // pixel lists are long; the count is enough here
    }
    edits.push_back(e);
  }
  nlohmann::json masks = nlohmann::json::object();
  for (const auto& [id, up] : d.masks.parts) {
    nlohmann::json views = nlohmann::json::object();
    for (const auto& [role, m] : up.masks)
      if (const int n = m.count(); n > 0) views[std::string(to_string(role))] = n;
    masks[std::to_string(id)] = {{"define", up.define}, {"pixels", views}};
    if (up.depth) masks[std::to_string(id)]["depth"] = *up.depth;
  }
  return {{"count", d.edits.size()},
          {"edits", edits},
          {"user_parts", masks},
          {"next_part_id", d.next_part_id},
          {"single_view", single_view_to_json(d.single_view)}};
}

}  // namespace pixel3d
