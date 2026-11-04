#include "pixel3d/engine/pipeline.hpp"

#include <cmath>

#include "pixel3d/io/glb_check.hpp"
#include "pixel3d/io/glb_writer.hpp"
#include "pixel3d/io/json_io.hpp"
#include "pixel3d/lowpoly/views.hpp"
#include "pixel3d/reconstruct/part_geometry.hpp"
#include "pixel3d/reconstruct/prism.hpp"

namespace pixel3d::pipeline {
namespace {

Dims dims_of(const Document& d) { return d.align ? d.align->dims : Dims{}; }

// A mesh cache is reused only if it was built from exactly these inputs and
// its part ids exist (correspondence parts, or user/edit parts).
bool cache_usable(const Document& d) {
  if (!d.cached_solid || d.cached_input_hash != mesh_input_hash(d)) return false;
  const int parts = static_cast<int>(d.correspondence->parts.size());
  for (int p : d.cached_solid->mesh.tri_part)
    if (p < -1 || (p >= parts && p < kFirstUserPart)) return false;
  return true;
}

// A possible recess whose region the user gave a depth for (carving or not) is decided.
bool depth_decided(const RecessFinding& f, const std::vector<DepthConstraint>& cs) {
  const Polygons2D box = rect_polys(f.box.x0, f.box.y0, f.box.x1, f.box.y1);
  const double box_area = area(box);
  for (const DepthConstraint& c : cs)
    if (c.view == f.view && box_area > 0 && area(clip(box, c.region)) >= 0.5 * box_area) return true;
  return false;
}

// Manual texture assignments made for the current mesh; stale ones are reported.
std::vector<TextureAssignment> live_assignments(const Document& d, Notes& notes) {
  if (d.texture_assignments.empty()) return {};
  const std::string key = mesh_input_hash(d);
  std::vector<TextureAssignment> live;
  int stale = 0;
  for (const TextureAssignment& a : d.texture_assignments) {
    if (a.mesh_key == key) live.push_back(a);
    else ++stale;
  }
  if (stale > 0)
    notes.push_back(Message{"texture_assignment.stale",
                            std::to_string(stale) +
                                " manual texture assignment(s) were made for an earlier mesh and are not applied",
                            Severity::Warning, {{"count", stale}}});
  return live;
}

const RegionMap* region_of(const Document& d, ViewRole r) {
  if (!d.regions) return nullptr;
  const auto it = d.regions->find(r);
  return it == d.regions->end() ? nullptr : &it->second;
}

double max_user_part_depth(const Document& d) {
  double m = 0;
  for (const auto& [id, up] : d.masks.parts)
    if (up.depth) m = std::max(m, *up.depth);
  return m;
}

// Every user part is listed as a part (geometry may still be empty).
void add_user_part_entries(CorrespondenceResult& corr, const Document& d) {
  for (const auto& [id, up] : d.masks.parts)
    corr.parts.push_back(Part3D{id, {}, CandStatus::Certain, up.define ? "user part (defined by masks)" : "user part"});
}

manifold::Manifold drawings_hull(const Document& d, Dims dims) {
  manifold::Manifold hull;
  bool first = true;
  for (const auto& [role, vp] : build_view_polygons(d.views, d.settings.simplify, d.settings.alpha_threshold)) {
    const manifold::Manifold p = make_prism(role, dims, vp.simplified);
    hull = first ? p : hull ^ p;
    first = false;
  }
  return hull;
}

const SourceView* single_source(const Document& d) { return d.views.size() == 1 ? &d.views.front() : nullptr; }

double single_depth(const Document& d) {
  const SourceView* v = single_source(d);
  if (!v || !v->aligned) return 1;
  return single_view_depth(d.single_view, v->aligned->width(), v->aligned->height());
}

}  // namespace

std::string_view to_string(Stage s) {
  switch (s) {
    case Stage::Empty: return "empty";
    case Stage::Imported: return "imported";
    case Stage::Aligned: return "aligned";
    case Stage::Segmented: return "segmented";
    case Stage::Matched: return "matched";
    case Stage::Reconstructed: return "reconstructed";
    case Stage::Textured: return "textured";
    case Stage::Validated: return "validated";
  }
  return "empty";
}

Stage stage_of(const Document& d) {
  if (d.gate) return Stage::Validated;
  if (d.texture) return Stage::Textured;
  if (d.solid) return Stage::Reconstructed;
  if (d.correspondence) return Stage::Matched;
  if (d.bands) return Stage::Segmented;
  if (d.align) return Stage::Aligned;
  if (!d.views.empty()) return Stage::Imported;
  return Stage::Empty;
}

void clear_from(Document& d, Stage s) {
  if (s <= Stage::Aligned) d.align.reset(), d.lowpoly.reset();
  if (s <= Stage::Segmented) d.families.reset(), d.regions.reset(), d.bands.reset();
  if (s <= Stage::Matched) d.candidates.reset(), d.evidence.reset(), d.correspondence.reset();
  if (s <= Stage::Reconstructed) {
    d.solid.reset(), d.volume.reset(), d.recesses.reset();
    d.base_parts.reset(), d.edited_parts.reset();
    d.edits_applied = 0;
  }
  if (s <= Stage::Textured) d.texture.reset(), d.live.reset(), d.mask_conflicts.reset();
  if (s <= Stage::Validated) d.metrics.reset(), d.conflicts.reset(), d.gate.reset(), d.styled.reset();
}

const TexturedMesh& model_mesh(const Document& d) { return d.styled ? d.styled->texture.mesh : d.texture->mesh; }

std::optional<Message> style(Document& d, Notes&) {
  d.styled.reset();
  if (d.settings.style.preset == StylePreset::Crisp) return std::nullopt;
  const std::string key = style_input_hash(d);
  if (d.cached_styled && d.cached_styled->key == key) {
    d.styled = d.cached_styled;
    return std::nullopt;
  }
  StyleInputs in;
  in.parts = d.edited_parts.get();
  in.crisp = &d.solid->mesh;
  in.crisp_texture = d.texture.get();
  in.families = d.families.get();
  in.masks = masks_of(d);
  in.dims = dims_of(d);
  in.cleanup = d.cleanup_options;
  in.settings = d.settings.style;
  std::shared_ptr<const PartGeometry> rebuilt;
  if (!in.parts) {
    // The crisp model came from the project's mesh cache, which stores no parts:
    // rebuild them (same inputs, same parts) without touching the textured model.
    Document tmp = d;
    Notes ignored;
    if (auto e = build_parts(tmp, ignored)) return e;
    rebuilt = tmp.edited_parts;
    in.parts = rebuilt.get();
  }
  auto built = build_styled_model(in);
  if (!built) return built.error();
  built->key = key;
  d.styled = std::make_shared<const StyledModel>(std::move(*built));
  d.cached_styled = d.styled;
  return std::nullopt;
}

std::map<ViewRole, Mask2D> masks_of(const Document& d) {
  std::map<ViewRole, Mask2D> m;
  for (const SourceView& v : d.views)
    if (v.aligned) m.emplace(v.role, opaque_mask(*v.aligned, d.settings.alpha_threshold));
  return m;
}

std::optional<Message> align(Document& d, Notes& notes) {
  std::vector<SourceView> views = d.views;
  AlignOptions opt = d.align_options;
  opt.alpha_threshold = d.settings.alpha_threshold;
  // Real-world images become native pixel art first; clean art passes untouched.
  NormalizeOptions nopt;
  nopt.settings = d.settings.normalize;
  nopt.alpha_threshold = d.settings.alpha_threshold;
  nopt.extent_tolerance = opt.extent_tolerance;
  nopt.single_view = d.single_view.enabled || (d.settings.lowpoly.enabled && views.size() == 1);
  auto normalized = normalize_views(views, nopt);
  if (!normalized) return normalized.error();
  for (const Message& m : normalized->messages) notes.push_back(m);
  Expected<AlignResult> res = AlignResult{};
  if (d.settings.lowpoly.enabled) {
    // LOW-POLY STUDIO: Front with optional Side and Top, each view scaled and cropped alone.
    res = lowpoly::align_views(views, opt, d.settings.lowpoly.build.depth);
  } else if (d.single_view.enabled) {
    const double part_depth = max_user_part_depth(d);
    res = align_single_view(views, opt, [&](int w, int h) {
      return static_cast<int>(std::ceil(std::max(single_view_depth(d.single_view, w, h), part_depth)));
    });
  } else {
    res = auto_align(views, opt);
  }
  if (!res) {
    Message e = res.error();
    if (e.code == "view.too_large" && !d.settings.normalize.enabled)
      e.text += ". Input normalisation is off: turn it on (normalize) to remove backgrounds and shrink large art automatically";
    return e;
  }
  for (Message m : res->messages) {
    if (m.severity == Severity::Error) m.severity = Severity::Warning;
    notes.push_back(std::move(m));
  }
  d.views = std::move(views);
  d.align = std::make_shared<const AlignResult>(std::move(*res));
  return std::nullopt;
}

std::optional<Message> segment(Document& d, Notes&) {
  std::vector<const ImageRGBA*> imgs;
  for (const SourceView& v : d.views) imgs.push_back(v.aligned.get());
  auto fam = std::make_shared<const MaterialFamilies>(analyze_palette(imgs, d.settings.alpha_threshold));
  auto regions = std::make_shared<std::map<ViewRole, RegionMap>>();
  std::map<ViewRole, Mask2D> ym;
  for (const SourceView& v : d.views) {
    regions->emplace(v.role, compute_regions(*v.aligned, *fam, d.settings.alpha_threshold));
    if (v.role != ViewRole::Top && v.role != ViewRole::Bottom)
      ym.emplace(v.role, opaque_mask(*v.aligned, d.settings.alpha_threshold));
  }
  d.families = std::move(fam);
  d.regions = std::move(regions);
  d.bands = std::make_shared<const BandSet>(compute_bands(ym, dims_of(d).H));
  return std::nullopt;
}

std::optional<Message> match(Document& d, Notes& notes) {
  const Dims dims = dims_of(d);
  if (d.single_view.enabled) {
    // One drawing: no correspondence. The silhouette is part 0; its depth is assumed.
    d.candidates = std::make_shared<const CandidateSet>();
    d.evidence = std::make_shared<const std::vector<CandidateEvidence>>();
    CorrespondenceResult corr;
    corr.parts.push_back(Part3D{0, {}, CandStatus::InferredFill, "silhouette (single view)"});
    add_user_part_entries(corr, d);
    d.correspondence = std::make_shared<const CorrespondenceResult>(std::move(corr));
    return std::nullopt;
  }
  auto cands = std::make_shared<CandidateSet>(
      build_candidates(*d.bands, build_view_polygons(d.views, d.settings.simplify, d.settings.alpha_threshold), dims));
  if (cands->cands.empty()) return Message{"reconstruct.empty", "the views do not intersect in any volume", Severity::Error, {}};
  const VoxelGrid hull = visual_hull(masks_of(d), dims);
  auto ev = std::make_shared<const std::vector<CandidateEvidence>>(
      gather_evidence(*cands, *d.bands, hull, region_of(d, ViewRole::Top), region_of(d, ViewRole::Bottom), dims));
  // Candidates inside a "define" user part's masks are explained by that part.
  std::vector<UserDecision> decisions = d.decisions;
  for (int c : claimed_candidates(d.masks, *cands, dims)) decisions.push_back(UserDecision{c, false, true});
  CorrespondenceResult corr = solve_correspondence(*cands, *d.bands, *ev, *d.regions, decisions);
  add_user_part_entries(corr, d);
  auto shared = std::make_shared<const CorrespondenceResult>(std::move(corr));
  for (const Ambiguity& a : shared->ambiguities)
    notes.push_back(Message{a.kind, a.text, a.kind == "edit.uncovers_atom" ? Severity::Warning : Severity::Info,
                            {{"band", a.band}, {"candidates", a.candidates}}});
  d.candidates = std::move(cands);
  d.evidence = std::move(ev);
  d.correspondence = std::move(shared);
  return std::nullopt;
}

std::optional<Message> build_parts(Document& d, Notes& notes) {
  const Dims dims = dims_of(d);
  PartGeometry base;
  if (d.single_view.enabled) {
    const SourceView* v = single_source(d);
    if (!v) return Message{"view.single_view", "single view mode takes exactly one drawing", Severity::Error, {}};
    base = single_view_parts(*v, dims, single_depth(d), d.masks, d.settings.simplify, d.settings.alpha_threshold);
  } else if (d.mode == ReconstructMode::Voxel) {
    base = voxel_parts(*d.volume);  // the cells already respect the depth constraints
  } else {
    base = true_solid_parts(*d.candidates, {}, dims, d.correspondence->part_of_candidate);
    if (!d.masks.empty()) add_user_parts(base, d.masks, drawings_hull(d, dims), dims, d.settings.simplify, &notes);
  }
  if (d.mode != ReconstructMode::Voxel || d.single_view.enabled) {
    manifold::Manifold carve;
    for (const DepthConstraint& c : d.depth_constraints) carve = carve + carve_volume(c, dims);
    if (!carve.IsEmpty())
      for (auto& [id, m] : base.parts) m = m - carve;
    base.normalize();
  }
  PartGeometry edited = base;
  for (size_t i = 0; i < d.edits.size(); ++i)
    if (auto e = apply_edit(edited, d.edits[i], dims, &notes)) {
      Message m = *e;
      m.text = "edit #" + std::to_string(i + 1) + " (" + edit_name(d.edits[i]) + "): " + m.text;
      m.where["edit"] = i;
      return m;
    }
  d.base_parts = std::make_shared<const PartGeometry>(std::move(base));
  d.edits_applied = d.edits.size();
  d.solid = std::make_shared<const SolidResult>(assemble_parts(edited, d.cleanup_options));
  d.edited_parts = std::make_shared<const PartGeometry>(std::move(edited));
  return std::nullopt;
}

std::optional<Message> reconstruct(Document& d, Notes& notes) {
  const Dims dims = dims_of(d);
  std::shared_ptr<const VoxelGrid> kept;
  if (d.single_view.enabled) {
    const SourceView* v = single_source(d);
    if (!v) return Message{"view.single_view", "single view mode takes exactly one drawing", Severity::Error, {}};
    kept = std::make_shared<const VoxelGrid>(apply_depth_constraints(
        single_view_volume(*v, dims, single_depth(d), d.masks, d.settings.alpha_threshold), d.depth_constraints, dims));
  } else {
    kept = std::make_shared<const VoxelGrid>(apply_depth_constraints(
        voxelize_candidates(*d.candidates, masks_of(d), *d.bands, dims), d.depth_constraints, dims));
  }
  d.volume = kept;
  // A mesh cache from the project file is reused only for exactly these inputs.
  if (cache_usable(d)) {
    d.solid = d.cached_solid;
  } else if (auto e = build_parts(d, notes)) {
    return e;
  }
  if (d.solid->topology.triangle_count == 0)
    return Message{"reconstruct.empty", "reconstruction produced no geometry", Severity::Error, {}};
  std::vector<RecessFinding> recesses = analyze_recesses(*d.regions, *kept, dims);
  for (RecessFinding& f : recesses)
    if (f.kind == "possible_recess" && depth_decided(f, d.depth_constraints)) f.kind = "user_depth";
  d.recesses = std::make_shared<const std::vector<RecessFinding>>(std::move(recesses));
  return std::nullopt;
}

std::optional<Message> apply_last_edit(Document& d, Notes& notes) {
  if (d.edits.empty()) return Message{"edit.none", "no edit to apply", Severity::Error, {}};
  // Incremental when the cached geometry is exactly one edit behind.
  if (!d.edited_parts || d.edits_applied + 1 != d.edits.size() || stage_of(d) < Stage::Reconstructed) {
    clear_from(d, Stage::Reconstructed);
    return ensure(d, Stage::Reconstructed, {}, notes);
  }
  PartGeometry g = *d.edited_parts;
  if (auto e = apply_edit(g, d.edits.back(), dims_of(d), &notes)) return e;
  clear_from(d, Stage::Textured);
  d.solid = std::make_shared<const SolidResult>(assemble_parts(g, d.cleanup_options));
  d.edited_parts = std::make_shared<const PartGeometry>(std::move(g));
  d.edits_applied = d.edits.size();
  if (d.solid->topology.triangle_count == 0)
    return Message{"edit.empty_model", "the edit removed the whole model", Severity::Error, {}};
  return std::nullopt;
}

std::optional<Message> texture(Document& d, Notes& notes) {
  const Dims dims = dims_of(d);
  auto tex = std::make_shared<const TextureResult>(project_textures(d.solid->mesh, d.views, *d.families, dims,
                                                                    d.texture_options, live_assignments(d, notes),
                                                                    d.settings.alpha_threshold));
  for (const Message& m : tex->messages) notes.push_back(m);
  // Live checks of the current mesh: cheap enough to run after every edit.
  QualityMetrics live = compute_metrics(tex->mesh, d.solid->mesh, d.views, dims, *d.correspondence, *tex,
                                        d.settings.alpha_threshold, d.recesses.get(), false);
  if (d.single_view.enabled) live.inferred_volume_fraction = 1.0;
  d.live = std::make_shared<const QualityMetrics>(std::move(live));
  d.mask_conflicts = std::make_shared<const std::vector<MaskConflict>>(
      d.masks.empty() ? std::vector<MaskConflict>{} : mask_conflicts(d.masks, d.solid->mesh, dims));
  d.texture = std::move(tex);
  return std::nullopt;
}

std::vector<Conflict> document_conflicts(const Document& d, const QualityMetrics* metrics) {
  if (!d.correspondence || !d.align || !d.candidates) return {};
  std::map<ViewRole, int> mismatch;
  if (metrics)
    for (const auto& [r, v] : metrics->views) mismatch[r] = v.mismatch_px;
  const std::vector<RecessFinding> none;
  std::vector<Conflict> out = collect_conflicts(*d.align, *d.correspondence, *d.candidates, d.recesses ? *d.recesses : none,
                                                d.texture.get(), mismatch);
  if (d.single_view.enabled) {
    const double depth = single_depth(d);
    out.push_back(Conflict{"single_view_depth", d.single_view.user_depth ? Severity::Info : Severity::Warning,
                           !d.single_view.user_depth,
                           d.single_view.user_depth
                               ? "single view: the depth (" + std::to_string(depth) + " px) was chosen by the user"
                               : "single view: no drawing shows the depth; the default " + d.single_view.preset +
                                     " preset (" + std::to_string(depth) + " px) is assumed — choose a depth",
                           {{"view", to_string(d.single_view.role)}, {"depth", depth}, {"preset", d.single_view.preset}}});
    out.push_back(Conflict{"inferred_hidden_geometry", Severity::Info, false,
                           "single view: the back and sides are not drawn; they are inferred (mirrored / edge art)",
                           {{"view", to_string(d.single_view.role)}}});
  }
  if (!d.edits.empty()) {
    nlohmann::json ops = nlohmann::json::array();
    for (const EditOp& e : d.edits) ops.push_back(edit_name(e));
    out.push_back(Conflict{"user_edits", Severity::Info, false,
                           std::to_string(d.edits.size()) + " user edit(s) shape the geometry beyond the reconstruction",
                           {{"edits", ops}}});
  }
  if (d.styled)
    for (const Message& m : d.styled->notes)
      out.push_back(Conflict{m.code, m.severity, false, m.text, m.where});
  if (d.mask_conflicts)
    for (const MaskConflict& m : *d.mask_conflicts)
      out.push_back(Conflict{"mask.unmatched", Severity::Warning, false,
                             std::to_string(m.unmatched_px) + " masked pixel(s) of a user part are not explained by its "
                                                              "geometry (masks disagree between views)",
                             {{"part", m.part},
                              {"view", to_string(m.view)},
                              {"pixels", m.unmatched_px},
                              {"box", {m.box.x0, m.box.y0, m.box.x1, m.box.y1}}}});
  return out;
}

std::optional<Message> validate(Document& d, const ValidateOptions& opt, Notes& notes) {
  const Dims dims = dims_of(d);
  // A polished style is validated on the polished model: the model that is exported.
  if (auto e = style(d, notes)) return e;
  const TexturedMesh& mesh = model_mesh(d);
  const Mesh& solid = d.styled ? d.styled->solid.mesh : d.solid->mesh;
  const TextureResult& tex = d.styled ? d.styled->texture : *d.texture;
  QualityMetrics m = compute_metrics(mesh, solid, d.views, dims, *d.correspondence, tex, d.settings.alpha_threshold,
                                     d.recesses.get(), true);
  if (d.single_view.enabled) m.inferred_volume_fraction = 1.0;
  if (opt.glb_roundtrip) {
    ExportOptions eo;
    eo.pixels_per_meter = d.settings.pixels_per_meter;
    if (d.styled) apply_style_export(*d.styled, eo);
    const auto glb = write_glb(mesh, eo);
    m.glb = to_json(check_glb_roundtrip(glb, mesh, eo, dims));
    m.glb["bytes"] = glb.size();
  }
  GateConfig cfg;
  cfg.profile = opt.profile.value_or(d.settings.profile);
  cfg.single_view = d.single_view.enabled;
  cfg.edited = !d.edits.empty();
  std::vector<Conflict> conflicts = document_conflicts(d, &m);
  GateResult gate = run_quality_gate(m, conflicts, cfg);
  // Edited geometry that no longer matches a drawing is a deviation to review.
  if (cfg.edited) {
    nlohmann::json views = nlohmann::json::array();
    for (const Check& c : gate.checks)
      if (!c.passed && (c.name.rfind("projection_", 0) == 0 || c.name.rfind("rotation_", 0) == 0)) views.push_back(c.name);
    if (!views.empty()) {
      conflicts.push_back(Conflict{"edit.projection_deviation", Severity::Warning, true,
                                   "after user edits the model no longer matches its drawings in some views",
                                   {{"checks", views}}});
      gate = run_quality_gate(m, conflicts, cfg);
    }
  }
  d.gate = std::make_shared<const GateResult>(std::move(gate));
  d.conflicts = std::make_shared<const std::vector<Conflict>>(std::move(conflicts));
  d.metrics = std::make_shared<const QualityMetrics>(std::move(m));
  return std::nullopt;
}

std::optional<Message> ensure(Document& d, Stage target, const JobContext& job, Notes& notes, const ValidateOptions& vopt) {
  if (d.views.empty()) return Message{"view.missing_required", "import Front, Side and Top views first", Severity::Error, {}};
  if (d.settings.lowpoly.enabled && target > Stage::Segmented)
    return Message{"state.wrong_mode",
                   "this is a Low-Poly Studio project: its model is built by lowpoly_build (render with lowpoly_render, "
                   "export with lowpoly_export); the visual-hull stages do not run in this mode",
                   Severity::Error, {{"mode", "lowpoly_studio"}, {"stage", to_string(target)}}};
  const Stage order[] = {Stage::Aligned, Stage::Segmented, Stage::Matched, Stage::Reconstructed, Stage::Textured,
                         Stage::Validated};
  for (Stage s : order) {
    if (s > target) break;
    if (stage_of(d) >= s) continue;
    if (job.stop.stop_requested()) return Message{"job.cancelled", "cancelled", Severity::Error, {}};
    if (job.progress) job.progress(static_cast<float>(static_cast<int>(s)) / 7.0f, to_string(s));
    std::optional<Message> err;
    switch (s) {
      case Stage::Aligned: err = align(d, notes); break;
      case Stage::Segmented: err = segment(d, notes); break;
      case Stage::Matched: err = match(d, notes); break;
      case Stage::Reconstructed: err = reconstruct(d, notes); break;
      case Stage::Textured: err = texture(d, notes); break;
      case Stage::Validated: err = validate(d, vopt, notes); break;
      default: break;
    }
    if (err) return err;
  }
  return std::nullopt;
}

}  // namespace pixel3d::pipeline
