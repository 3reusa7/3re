#include "pixel3d/engine/session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>

#include "pixel3d/common/limits.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/common/sha256.hpp"
#include "pixel3d/engine/doc_diff.hpp"
#include "pixel3d/engine/input_checks.hpp"
#include "pixel3d/engine/lowpoly_stage.hpp"
#include "pixel3d/io/json_io.hpp"
#include "pixel3d/io/project_file.hpp"
#include "pixel3d/validate/report.hpp"

namespace pixel3d {

using pipeline::Stage;

std::string_view to_string(Source s) {
  switch (s) {
    case Source::Gui: return "gui";
    case Source::Mcp: return "mcp";
    case Source::Cli: return "cli";
  }
  return "cli";
}

std::string_view to_string(ReconstructMode m) { return m == ReconstructMode::Voxel ? "voxel" : "true_solid"; }

namespace {

constexpr char kAutoCheckpoint[] = "auto:before-reconstruct-";
constexpr int kMaxAutoCheckpoints = 20;

int auto_number(const std::string& name) {
  int n = 0;
  for (char ch : name.substr(sizeof(kAutoCheckpoint) - 1)) {
    if (ch < '0' || ch > '9' || n > 100000000) break;
    n = n * 10 + (ch - '0');
  }
  return n;
}

std::string now_iso() {
  using namespace std::chrono;
  const auto now = floor<seconds>(system_clock::now());
  const auto day = floor<days>(now);
  const year_month_day ymd{day};
  const hh_mm_ss hms{now - day};
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02uT%02d:%02d:%02dZ", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), static_cast<int>(hms.hours().count()),
                static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count()));
  return buf;
}

}  // namespace

nlohmann::json parts_json(const Document& d) {
  nlohmann::json parts = nlohmann::json::array();
  std::map<int, nlohmann::json> by_id;
  if (d.correspondence)
    for (const Part3D& p : d.correspondence->parts)
      by_id[p.id] = {{"id", p.id}, {"label", p.label}, {"status", to_string(p.status)}, {"candidates", p.candidates}};
  // Parts with geometry: the edited part set, or the model mesh when it came from the project cache.
  std::set<int> with_geometry;
  if (d.edited_parts)
    for (const auto& [id, m] : d.edited_parts->parts) with_geometry.insert(id);
  else if (const auto& solid = d.solid ? d.solid : d.cached_solid)
    with_geometry.insert(solid->mesh.tri_part.begin(), solid->mesh.tri_part.end());
  for (const int id : with_geometry)
    if (!by_id.contains(id)) by_id[id] = {{"label", "created by an edit"}, {"status", "user"}};
  for (const auto& [id, up] : d.masks.parts) by_id[id]["define"] = up.define;
  for (const auto& [id, t] : d.part_types)
    if (d.masks.parts.contains(id)) by_id[id]["type"] = lowpoly::to_string(t);
  for (const auto& [id, name] : d.part_names) by_id[id];
  for (auto& [id, j] : by_id) {
    j["id"] = id;
    j["user"] = id >= kFirstUserPart;
    if (const auto it = d.part_names.find(id); it != d.part_names.end()) j["name"] = it->second;
    j["has_geometry"] = with_geometry.contains(id);
    parts.push_back(j);
  }
  return parts;
}

namespace {

nlohmann::json conflicts_now(const Document& d) {
  if (d.conflicts) return *d.conflicts;
  return pipeline::document_conflicts(d, nullptr);
}

// Runs fn and turns any exception into an OpResult error: nothing escapes the API.
template <class F>
OpResult guarded(F&& fn) {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return OpResult::fail(Message{"internal.out_of_memory", "not enough memory for this operation", Severity::Error, {}});
  } catch (const std::exception& e) {
    return OpResult::fail(Message{"internal.exception", e.what(), Severity::Error, {}});
  } catch (...) {
    return OpResult::fail(Message{"internal.exception", "unknown error", Severity::Error, {}});
  }
}

std::optional<Message> check_checkpoint_name(const std::string& name) {
  if (name.empty() || name.size() > 256)
    return Message{"input.invalid", "checkpoint names must be 1-256 bytes", Severity::Error, {{"field", "name"}}};
  return std::nullopt;
}

Message not_matched() {
  return Message{"state.not_ready", "import views first (Front, Side and Top)", Severity::Error, {}};
}

}  // namespace

Session::Session(Source source) : source_(source) {}

void Session::log_op(const std::string& name, nlohmann::json params, const OpResult& r) {
  log_.push_back(LogEntry{now_iso(), source_, name, std::move(params),
                          r.success ? "ok" : (r.errors.empty() ? "failed" : r.errors.front().code + ": " + r.errors.front().text),
                          r.success});
}

OpResult Session::apply(const std::string& name, nlohmann::json params, Commit commit,
                        const std::function<OpResult(Document&)>& fn) {
  OpResult r = guarded([&] {
    Document d = history_.current();
    OpResult res = fn(d);
    if (res.success) {
      // A low-poly build is derived from the inputs: any input change drops it.
      if (d.lowpoly && d.lowpoly->key != lowpoly_input_hash(d)) d.lowpoly.reset();
      res.changed_ids = diff_documents(history_.current(), d);
      d.revision = history_.current().revision + 1;
      if (commit == Commit::Push) history_.push(std::move(d), name);
      else history_.replace_current(std::move(d));
    }
    return res;
  });
  log_op(name, std::move(params), r);
  finish(r);
  return r;
}

std::optional<Message> Session::refresh(Document& d, Stage reached, Stage at_least, OpResult& r) const {
  const Stage target = std::max(at_least, std::min(reached, Stage::Textured));
  pipeline::Notes notes;
  auto err = pipeline::ensure(d, target, {}, notes);
  for (const Message& m : notes) r.add(m);
  return err;
}

void Session::finish(OpResult& r) const {
  const Document& d = history_.current();
  r.revision = d.revision;
  if (d.solid) {
    const TopologyReport& t = d.solid->topology;
    std::set<int> parts;
    for (int p : d.solid->mesh.tri_part)
      if (p >= 0) parts.insert(p);
    r.stats = MeshStats{t.triangle_count, t.vertex_count, static_cast<int>(parts.size()), t.signed_volume, t.watertight};
  }
  r.metrics = d.metrics ? metrics_json(*d.metrics) : nlohmann::json();
  r.status = d.gate ? d.gate->status : ValidationStatus::NotValidated;
  r.validation = nullptr;
  if (d.gate) {
    nlohmann::json v{{"status", to_string(d.gate->status)},
                     {"profile", to_string(d.settings.profile)},
                     {"failed_critical_checks", nlohmann::json::array()},
                     {"failed_checks", nlohmann::json::array()},
                     {"blocking_conflicts", nlohmann::json::array()}};
    for (const Check& c : d.gate->checks)
      if (!c.passed) v[c.critical ? "failed_critical_checks" : "failed_checks"].push_back(c.name);
    if (d.conflicts)
      for (const Conflict& c : *d.conflicts)
        if (c.blocks_pass) v["blocking_conflicts"].push_back({{"kind", c.kind}, {"text", c.text}});
    r.validation = std::move(v);
  }
  // A Low-Poly Studio project's model is the low-poly character: its gate is the status.
  if (d.settings.lowpoly.enabled) {
    if (const LowpolyBuild* b = current_lowpoly(d)) {
      r.status = b->validation.status;
      r.validation = lowpoly_summary(*b);
      r.stats.triangles = b->result.triangles;
      r.stats.vertices = static_cast<int>(b->result.mesh().positions.size());
      r.stats.parts = static_cast<int>(b->result.parts.size());
    } else {
      r.status = ValidationStatus::NotValidated;
    }
  }
}

OpResult Session::new_project(const ProjectSettings& settings) {
  // A new project starts a new session: history, checkpoints and the log belong
  // to the previous project and must not leak into this one (or its file).
  OpResult r = guarded([&] {
    if (auto e = check_settings(settings)) return OpResult::fail(*e);
    Document d;
    d.settings = settings;
    d.revision = history_.current().revision + 1;
    OpResult res;
    res.changed_ids = diff_documents(history_.current(), d);
    History fresh;
    fresh.replace_current(std::move(d));
    history_ = std::move(fresh);
    log_.clear();
    reconstruct_count_ = 0;
    res.success = true;
    res.changed = {"project"};
    return res;
  });
  log_op("new_project", {{"name", settings.name}}, r);
  finish(r);
  return r;
}

OpResult Session::update_settings(const SettingsUpdate& u) {
  nlohmann::json params = nlohmann::json::object();
  if (u.name) params["name"] = *u.name;
  if (u.pixels_per_meter) params["pixels_per_meter"] = *u.pixels_per_meter;
  if (u.profile) params["profile"] = to_string(*u.profile);
  if (u.normalize.enabled) params["normalize"] = *u.normalize.enabled;
  if (u.normalize.target_size) params["target_size"] = *u.normalize.target_size;
  if (u.normalize.bg_tolerance) params["bg_tolerance"] = *u.normalize.bg_tolerance;
  if (u.normalize.axis_tolerance) params["axis_tolerance"] = *u.normalize.axis_tolerance;
  if (u.style) params["style"] = to_json(*u.style);
  return apply("update_settings", params, Commit::Push, [&](Document& d) {
    ProjectSettings s = d.settings;
    if (u.name) s.name = *u.name;
    if (u.pixels_per_meter) s.pixels_per_meter = *u.pixels_per_meter;
    if (u.profile) s.profile = *u.profile;
    s.normalize = u.normalize.applied_to(s.normalize);
    if (u.style) s.style = *u.style;
    if (auto e = check_settings(s)) return OpResult::fail(*e);
    // Normalisation decides the aligned images: everything is derived again.
    if (!(s.normalize == d.settings.normalize)) pipeline::clear_from(d, Stage::Aligned);
    // Scale and profile change the GLB check and the quality gate; the style changes the
    // validated model (the crisp model stays: it does not depend on the style).
    if (s.pixels_per_meter != d.settings.pixels_per_meter || s.profile != d.settings.profile ||
        !(s.style == d.settings.style))
      pipeline::clear_from(d, Stage::Validated);
    d.settings = s;
    OpResult r;
    r.success = true;
    r.changed = {"settings"};
    return r;
  });
}

std::optional<Message> import_views_into(Document& d, const std::vector<ViewImport>& views, OpResult& r) {
  if (views.empty()) return Message{"input.invalid", "no views given", Severity::Error, {}};
  std::set<ViewRole> roles;
  for (const ViewImport& v : views)
    if (!roles.insert(v.role).second)
      return Message{"input.invalid", "view role given twice: " + std::string(to_string(v.role)), Severity::Error,
                     {{"view", to_string(v.role)}}};
  for (const ViewImport& v : views) {
    auto bytes = read_file(v.path, limits::kMaxImageFileBytes);
    if (!bytes) return bytes.error();
    auto img = decode_image(*bytes, to_utf8(v.path.filename()));
    if (!img) {
      Message m = img.error();
      m.where["path"] = to_utf8(v.path);
      m.where["view"] = to_string(v.role);
      return m;
    }
    if (opaque_mask(*img, d.settings.alpha_threshold).count() == 0)
      return Message{"view.empty", "the image has no opaque pixels: " + to_utf8(v.path), Severity::Error,
                     {{"view", to_string(v.role)}, {"path", to_utf8(v.path)}}};
    SourceView sv;
    sv.role = v.role;
    sv.original_path = to_utf8(v.path);
    sv.original = std::make_shared<const ImageRGBA>(std::move(*img));
    sv.sha256 = sha256_hex(*bytes);
    sv.original_bytes = std::make_shared<const std::vector<uint8_t>>(std::move(*bytes));
    std::erase_if(d.views, [&](const SourceView& o) { return o.role == v.role; });
    d.views.push_back(std::move(sv));
    if (v.flip_x) d.align_options.force_flip[v.role] = *v.flip_x;
    else d.align_options.force_flip.erase(v.role);
    r.data["views"].push_back({{"role", to_string(v.role)},
                               {"path", to_utf8(v.path)},
                               {"width", d.views.back().original->width()},
                               {"height", d.views.back().original->height()}});
  }
  // Candidate ids are positions in a candidate set derived from the drawings:
  // new drawings make old keep/remove decisions refer to different solids.
  if (!d.decisions.empty()) {
    r.add(Message{"edit.decisions_cleared",
                  std::to_string(d.decisions.size()) + " part decision(s) were cleared because the views changed",
                  Severity::Warning, {{"count", d.decisions.size()}}});
    d.decisions.clear();
  }
  if (!d.depth_constraints.empty() || !d.masks.empty())
    r.add(Message{"edit.constraints_kept",
                  "depth constraints and part masks are kept in view pixel coordinates; check them against the new drawings",
                  Severity::Warning, {{"constraints", d.depth_constraints.size()}, {"user_parts", d.masks.parts.size()}}});
  if (!d.edits.empty())
    r.add(Message{"edit.edits_kept", std::to_string(d.edits.size()) + " geometry edit(s) are replayed on the new drawings",
                  Severity::Info, {{"count", d.edits.size()}}});
  pipeline::clear_from(d, Stage::Aligned);
  return std::nullopt;
}

OpResult Session::import_views(const std::vector<ViewImport>& views, const NormalizeUpdate& normalize) {
  nlohmann::json params = nlohmann::json::array();
  for (const ViewImport& v : views) params.push_back({{"role", to_string(v.role)}, {"path", to_utf8(v.path)}});
  if (!normalize.empty()) params = {{"views", params}, {"normalize", to_json(normalize.applied_to(NormalizeSettings{}))}};
  return apply("import_views", params, Commit::Push, [&](Document& d) {
    OpResult r;
    if (!normalize.empty()) {
      ProjectSettings s = d.settings;
      s.normalize = normalize.applied_to(s.normalize);
      if (auto e = check_settings(s)) return OpResult::fail(*e);
      d.settings = s;
    }
    if (auto e = import_views_into(d, views, r)) return OpResult::fail(*e);
    r.success = true;
    r.changed = {"views"};
    return r;
  });
}

OpResult Session::reload_views() {
  return apply("reload_views", {}, Commit::Push, [&](Document& d) {
    OpResult r;
    if (d.views.empty()) return OpResult::fail(Message{"state.not_ready", "no views to reload", Severity::Error, {}});
    std::vector<ViewImport> changed;
    nlohmann::json unchanged = nlohmann::json::array();
    for (const SourceView& v : d.views) {
      const auto path = path_from_utf8(v.original_path);
      auto bytes = read_file(path, limits::kMaxImageFileBytes);
      if (!bytes) {
        r.add(Message{"view.reload_failed", "cannot re-read " + v.original_path + ": " + bytes.error().text, Severity::Warning,
                      {{"view", to_string(v.role)}, {"path", v.original_path}}});
        continue;
      }
      if (sha256_hex(*bytes) == v.sha256) {
        unchanged.push_back(to_string(v.role));
        continue;
      }
      std::optional<bool> flip;
      if (const auto f = d.align_options.force_flip.find(v.role); f != d.align_options.force_flip.end()) flip = f->second;
      changed.push_back(ViewImport{v.role, path, flip});
    }
    r.data["unchanged"] = unchanged;
    if (!changed.empty())
      if (auto e = import_views_into(d, changed, r)) return OpResult::fail(*e);
    if (changed.empty()) r.add(Message{"view.unchanged", "every drawing is unchanged on disk", Severity::Info, {}});
    r.success = true;
    r.changed = changed.empty() ? std::vector<std::string>{} : std::vector<std::string>{"views"};
    return r;
  });
}

OpResult Session::analyze() {
  return apply("analyze", {}, Commit::Replace, [&](Document& d) {
    OpResult r;
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Segmented, {}, notes)) return OpResult::fail(*e);
    for (const Message& m : notes) r.add(m);
    r.data["dims"] = {d.align->dims.W, d.align->dims.H, d.align->dims.D};
    for (const SourceView& v : d.views)
      r.data["views"][std::string(to_string(v.role))] = {{"scale", v.scale.k},
                                                         {"flipped", v.flipped_x},
                                                         {"unexplained_px", d.align->profile_conflicts.at(v.role)},
                                                         {"normalization", normalization_json(v)}};
    r.data["material_families"] = d.families->family_count;
    r.data["outline_detected"] = d.families->outline_family >= 0;
    r.data["bands"] = d.bands->bands.size();
    r.success = true;
    return r;
  });
}

OpResult Session::auto_align() {
  return apply("auto_align", {}, Commit::Push, [&](Document& d) {
    OpResult r;
    pipeline::clear_from(d, Stage::Aligned);
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Aligned, {}, notes)) return OpResult::fail(*e);
    for (const Message& m : notes) r.add(m);
    r.data["dims"] = {d.align->dims.W, d.align->dims.H, d.align->dims.D};
    for (const SourceView& v : d.views)
      r.data["views"][std::string(to_string(v.role))] = {{"scale", v.scale.k},
                                                         {"flipped", v.flipped_x},
                                                         {"flip_confidence", d.align->flip_confidence.contains(v.role)
                                                                                 ? d.align->flip_confidence.at(v.role)
                                                                                 : 1.0},
                                                         {"unexplained_px", d.align->profile_conflicts.at(v.role)},
                                                         {"normalization", normalization_json(v)}};
    r.success = true;
    r.changed = {"alignment"};
    return r;
  });
}

OpResult Session::segment_parts() {
  return apply("segment_parts", {}, Commit::Push, [&](Document& d) {
    OpResult r;
    pipeline::clear_from(d, Stage::Segmented);
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Segmented, {}, notes)) return OpResult::fail(*e);
    for (const Message& m : notes) r.add(m);
    r.data["bands"] = nlohmann::json::array();
    for (const Band& b : d.bands->bands) {
      nlohmann::json bj{{"id", b.id}, {"y0", b.y0}, {"y1", b.y1}};
      for (const BandView& bv : b.views) bj["strands"][std::string(to_string(bv.role))] = bv.strands.size();
      r.data["bands"].push_back(bj);
    }
    r.data["material_families"] = d.families->family_count;
    r.success = true;
    r.changed = {"segmentation"};
    return r;
  });
}

OpResult Session::match_parts() {
  return apply("match_parts", {}, Commit::Push, [&](Document& d) {
    OpResult r;
    pipeline::clear_from(d, Stage::Matched);
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Matched, {}, notes)) return OpResult::fail(*e);
    for (const Message& m : notes) r.add(m);
    r.data["candidates"] = d.candidates->cands.size();
    r.data["parts"] = parts_json(d);
    r.data["conflicts"] = conflicts_now(d);
    r.success = true;
    r.changed = {"parts"};
    return r;
  });
}

OpResult Session::reconstruct(const ReconstructOptions& opt, const JobContext& job) {
  return guarded([&] {
    // The automatic checkpoint holds the state before the call; it is added only
    // if the reconstruction succeeds (a failed call changes nothing).
    const Document before = history_.current();
    const std::string cp = kAutoCheckpoint + std::to_string(reconstruct_count_ + 1);
    OpResult res = apply("reconstruct", {{"mode", to_string(opt.mode)}}, Commit::Push, [&](Document& d) {
      OpResult r;
      d.mode = opt.mode;
      pipeline::clear_from(d, Stage::Reconstructed);
      pipeline::Notes notes;
      if (auto e = pipeline::ensure(d, Stage::Reconstructed, job, notes)) return OpResult::fail(*e);
      for (const Message& m : notes) r.add(m);
      r.data = {{"mode", to_string(opt.mode)},
                {"checkpoint", cp},
                {"parts", parts_json(d)},
                {"cache_hit", d.solid != nullptr && d.solid == d.cached_solid}};
      r.success = true;
      r.changed = {"mesh", "parts"};
      return r;
    });
    if (res.success) {
      // Retention: keep the newest kMaxAutoCheckpoints automatic checkpoints (named
      // ones are never dropped), and the map within what a project file may hold.
      std::vector<std::pair<int, std::string>> autos;
      for (const std::string& n : history_.checkpoint_names())
        if (n.rfind(kAutoCheckpoint, 0) == 0) autos.emplace_back(auto_number(n), n);
      std::sort(autos.begin(), autos.end());  // oldest first
      for (size_t i = 0; i < autos.size(); ++i) {
        const bool too_many_autos = autos.size() - i >= static_cast<size_t>(kMaxAutoCheckpoints);
        const bool map_full = history_.checkpoints().size() >= static_cast<size_t>(limits::kMaxCheckpoints);
        if (!too_many_autos && !map_full) break;
        history_.erase_checkpoint(autos[i].second);
      }
      ++reconstruct_count_;
      history_.set_checkpoint(cp, before);
    }
    return res;
  });
}

OpResult Session::get_conflicts() const {
  return guarded([&] {
    OpResult r;
    r.success = true;
    const Document& d = history_.current();
    r.data["conflicts"] = conflicts_now(d);
    r.data["validated"] = d.conflicts != nullptr;
    if (d.candidates) {
      nlohmann::json amb = nlohmann::json::array();
      for (const Candidate& c : d.candidates->cands)
        if (c.status == CandStatus::Ambiguous)
          amb.push_back({{"candidate", c.id},
                         {"band", c.band},
                         {"footprint", {c.footprint.x0, c.footprint.y0, c.footprint.x1, c.footprint.y1}},
                         {"volume", c.volume}});
      r.data["ambiguous_candidates"] = amb;
    }
    finish(r);
    return r;
  });
}

OpResult Session::current_status() const {
  OpResult r;
  r.success = true;
  finish(r);
  return r;
}

OpResult Session::get_inferred_geometry() const {
  return guarded([&] {
    const Document& d = history_.current();
    if (!d.candidates || !d.correspondence) return OpResult::fail(Message{
        "state.not_ready", "run match_parts or reconstruct first", Severity::Error, {}});
    OpResult r;
    nlohmann::json cands = nlohmann::json::array();
    double total = 0, inferred = 0, ambiguous = 0;
    for (const Candidate& c : d.candidates->cands) {
      if (!is_kept(c.status)) continue;
      total += c.volume;
      const bool inf = c.status == CandStatus::InferredGrid || c.status == CandStatus::InferredContinuation ||
                       c.status == CandStatus::InferredFill;
      if (c.status == CandStatus::Ambiguous) ambiguous += c.volume;
      if (!inf && c.status != CandStatus::Ambiguous) continue;
      if (inf) inferred += c.volume;
      const int part = c.id < static_cast<int>(d.correspondence->part_of_candidate.size())
                           ? d.correspondence->part_of_candidate[c.id]
                           : -1;
      cands.push_back({{"candidate", c.id},
                       {"part", part},
                       {"status", to_string(c.status)},
                       {"reason", c.reason},
                       {"band", c.band},
                       {"footprint", {c.footprint.x0, c.footprint.y0, c.footprint.x1, c.footprint.y1}},
                       {"volume", c.volume}});
    }
    r.data["candidates"] = cands;
    r.data["kept_volume"] = total;
    r.data["inferred_candidate_volume"] = inferred;
    r.data["ambiguous_candidate_volume"] = ambiguous;
    r.data["inferred_volume"] = d.correspondence->inferred_volume;
    r.data["ambiguous_volume"] = d.correspondence->ambiguous_volume;
    r.data["recesses"] = nlohmann::json::array();
    if (d.recesses)
      for (const RecessFinding& f : *d.recesses)
        r.data["recesses"].push_back({{"view", to_string(f.view)},
                                      {"kind", f.kind},
                                      {"region", f.region},
                                      {"box", {f.box.x0, f.box.y0, f.box.x1, f.box.y1}},
                                      {"area_px", f.area}});
    if (d.texture) {
      for (const auto& [mode, area] : d.texture->area_by_mode) r.data["surface_area_by_mode"][std::string(to_string(mode))] = area;
      size_t inferred_faces = 0;
      for (const FaceTexture& f : d.texture->faces) inferred_faces += f.inferred ? 1 : 0;
      r.data["inferred_surface_faces"] = inferred_faces;
    }
    r.success = true;
    finish(r);
    return r;
  });
}

OpResult Session::set_depth(const DepthConstraint& c) {
  if (auto e = check_depth_constraint(c)) return OpResult::fail(*e);
  return apply("set_depth", {{"view", to_string(c.view)}, {"from", c.depth_from}, {"to", c.depth_to}, {"label", c.label}},
               Commit::Push, [&](Document& d) {
                 OpResult r;
                 if (d.views.empty()) return OpResult::fail(not_matched());
                 if (d.depth_constraints.size() >= static_cast<size_t>(limits::kMaxDepthConstraints))
                   return OpResult::fail(Message{"input.invalid", "too many depth constraints", Severity::Error, {}});
                 const Stage reached = pipeline::stage_of(d);
                 d.depth_constraints.push_back(c);
                 pipeline::clear_from(d, Stage::Reconstructed);
                 if (auto e = refresh(d, reached, Stage::Reconstructed, r)) return OpResult::fail(*e);
                 r.data["constraint"] = d.depth_constraints.size() - 1;
                 r.success = true;
                 r.changed = {"constraints", "mesh"};
                 return r;
               });
}

OpResult Session::edit_part(const PartEdit& e) {
  nlohmann::json params = nlohmann::json::array();
  for (const UserDecision& u : e.decisions) params.push_back({{"candidate", u.candidate}, {"keep", u.keep}});
  return apply("edit_part", params, Commit::Push, [&](Document& d) {
    OpResult r;
    if (e.decisions.empty()) return OpResult::fail(Message{"input.invalid", "no decisions given", Severity::Error, {}});
    const Stage reached = pipeline::stage_of(d);
    // Candidate ids only mean something once the candidates exist.
    pipeline::Notes notes;
    if (auto err = pipeline::ensure(d, Stage::Matched, {}, notes)) return OpResult::fail(*err);
    if (d.decisions.size() + e.decisions.size() > static_cast<size_t>(limits::kMaxDecisions))
      return OpResult::fail(Message{"input.invalid", "too many decisions", Severity::Error, {}});
    for (const UserDecision& u : e.decisions) {
      if (auto err = check_decision(u)) return OpResult::fail(*err);
      if (u.candidate >= static_cast<int>(d.candidates->cands.size()))
        return OpResult::fail(Message{"edit.unknown_candidate", "no candidate " + std::to_string(u.candidate),
                                      Severity::Error, {{"candidate", u.candidate}}});
      std::erase_if(d.decisions, [&](const UserDecision& o) { return o.candidate == u.candidate; });
      d.decisions.push_back(u);
    }
    pipeline::clear_from(d, Stage::Matched);
    if (auto err = refresh(d, reached, Stage::Matched, r)) return OpResult::fail(*err);
    // A decision the solver could not honour (e.g. removing the only solid that
    // explains a drawn strand) is reported, never silently dropped.
    nlohmann::json refused = nlohmann::json::array();
    for (const UserDecision& u : e.decisions) {
      const CandStatus st = d.candidates->cands[u.candidate].status;
      if (u.keep != is_kept(st) || (u.keep && st == CandStatus::Ambiguous))
        refused.push_back({{"candidate", u.candidate}, {"keep", u.keep}, {"status", to_string(st)}});
    }
    if (!refused.empty())
      r.add(Message{"edit.decision_refused",
                    std::to_string(refused.size()) + " decision(s) could not be applied; the candidates keep their status",
                    Severity::Warning, {{"decisions", refused}}});
    r.data["refused_decisions"] = refused;
    r.data["parts"] = parts_json(d);
    r.data["conflicts"] = conflicts_now(d);
    r.success = true;
    r.changed = {"parts", "mesh"};
    return r;
  });
}

OpResult Session::cleanup(const CleanupOptions& opt) {
  if (auto e = check_cleanup_options(opt)) return OpResult::fail(*e);
  return apply("cleanup", {{"min_fragment_volume", opt.min_fragment_volume}}, Commit::Push, [&](Document& d) {
    OpResult r;
    const Stage reached = pipeline::stage_of(d);
    d.cleanup_options = opt;
    pipeline::clear_from(d, Stage::Reconstructed);
    if (auto e = refresh(d, reached, Stage::Reconstructed, r)) return OpResult::fail(*e);
    const CleanupReport& c = d.solid->cleanup;
    r.data = {{"welded", c.welded},
              {"removed_zero_area", c.removed_zero_area},
              {"removed_internal", c.removed_internal},
              {"removed_fragments", c.removed_fragments},
              {"reoriented_faces", c.reoriented_faces}};
    r.success = true;
    r.changed = {"mesh"};
    return r;
  });
}

OpResult Session::project_textures(const TextureOptions& opt) {
  if (auto e = check_texture_options(opt)) return OpResult::fail(*e);
  return apply("project_textures", {{"allow_opposite_projection", opt.allow_opposite_projection}}, Commit::Push,
               [&](Document& d) {
                 OpResult r;
                 d.texture_options = opt;
                 pipeline::clear_from(d, Stage::Textured);
                 pipeline::Notes notes;
                 if (auto e = pipeline::ensure(d, Stage::Textured, {}, notes)) return OpResult::fail(*e);
                 for (const Message& m : notes) r.add(m);
                 for (const auto& [mode, area] : d.texture->area_by_mode) r.data["area_by_mode"][std::string(to_string(mode))] = area;
                 r.success = true;
                 r.changed = {"texture"};
                 return r;
               });
}

OpResult Session::assign_texture(const TextureAssignment& a) {
  if (auto e = check_texture_assignment(a)) return OpResult::fail(*e);
  return apply("assign_texture", {{"tris", a.tris}, {"view", to_string(a.view)}, {"mode", to_string(a.mode)}},
               Commit::Push, [&](Document& d) {
                 OpResult r;
                 bool supplied = false;
                 for (const SourceView& v : d.views) supplied |= v.role == a.view;
                 if (!supplied)
                   return OpResult::fail(Message{"input.invalid", "no source view " + std::string(to_string(a.view)),
                                                 Severity::Error, {{"view", to_string(a.view)}}});
                 if (d.texture_assignments.size() >= static_cast<size_t>(limits::kMaxTextureAssignments))
                   return OpResult::fail(Message{"input.invalid", "too many texture assignments", Severity::Error, {}});
                 const Stage reached = pipeline::stage_of(d);
                 pipeline::Notes notes;
                 if (auto e = pipeline::ensure(d, Stage::Reconstructed, {}, notes)) return OpResult::fail(*e);
                 const size_t tris = d.solid->mesh.tris.size();
                 for (int t : a.tris)
                   if (static_cast<size_t>(t) >= tris)
                     return OpResult::fail(Message{"input.invalid", "no triangle " + std::to_string(t), Severity::Error,
                                                   {{"triangle", t}, {"triangles", tris}}});
                 TextureAssignment bound = a;
                 bound.mesh_key = mesh_input_hash(d);
                 d.texture_assignments.push_back(std::move(bound));
                 pipeline::clear_from(d, Stage::Textured);
                 if (auto e = refresh(d, reached, Stage::Textured, r)) return OpResult::fail(*e);
                 r.success = true;
                 r.changed = {"texture"};
                 return r;
               });
}

OpResult Session::validate(const ValidateOptions& opt, const JobContext& job) {
  return apply("validate", {}, Commit::Replace, [&](Document& d) {
    OpResult r;
    pipeline::clear_from(d, Stage::Validated);
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Validated, job, notes, opt)) return OpResult::fail(*e);
    for (const Message& m : notes) r.add(m);
    const ToleranceProfile profile = opt.profile.value_or(d.settings.profile);
    r.data["report"] = make_report(*d.gate, *d.metrics, *d.conflicts, *d.correspondence,
                                   {{"project", d.settings.name},
                                    {"mode", to_string(d.mode)},
                                    {"pixels_per_meter", d.settings.pixels_per_meter},
                                    {"revision", history_.current().revision + 1}},
                                   profile);
    if (d.styled) r.data["report"]["style"] = to_json(*d.styled);
    r.data["conflicts"] = *d.conflicts;
    r.success = true;
    r.changed = {"validation"};
    return r;
  });
}

OpResult Session::undo() {
  OpResult r = guarded([&] {
    const Document before = history_.current();
    if (!history_.undo()) return OpResult::fail(Message{"history.nothing_to_undo", "nothing to undo", Severity::Error, {}});
    OpResult res;
    res.success = true;
    res.changed = {"document"};
    res.changed_ids = diff_documents(before, history_.current());
    return res;
  });
  log_op("undo", {}, r);
  finish(r);
  return r;
}

OpResult Session::redo() {
  OpResult r = guarded([&] {
    const Document before = history_.current();
    if (!history_.redo()) return OpResult::fail(Message{"history.nothing_to_redo", "nothing to redo", Severity::Error, {}});
    OpResult res;
    res.success = true;
    res.changed = {"document"};
    res.changed_ids = diff_documents(before, history_.current());
    return res;
  });
  log_op("redo", {}, r);
  finish(r);
  return r;
}

OpResult Session::create_checkpoint(const std::string& name) {
  OpResult r = guarded([&] {
    if (auto e = check_checkpoint_name(name)) return OpResult::fail(*e);
    const bool exists = history_.checkpoint_state(name).has_value();
    if (!exists && history_.checkpoints().size() >= static_cast<size_t>(limits::kMaxCheckpoints))
      return OpResult::fail(Message{"checkpoint.limit", "too many checkpoints (limit " +
                                                            std::to_string(limits::kMaxCheckpoints) + ")",
                                    Severity::Error, {}});
    history_.checkpoint(name);
    OpResult res;
    res.success = true;
    res.data["name"] = name;
    res.data["replaced"] = exists;
    res.data["checkpoints"] = history_.checkpoint_names();
    return res;
  });
  log_op("create_checkpoint", {{"name", name}}, r);
  finish(r);
  return r;
}

OpResult Session::restore_checkpoint(const std::string& name) {
  const auto state = history_.checkpoint_state(name);
  if (!state) {
    OpResult r = OpResult::fail(Message{"checkpoint.not_found", "no checkpoint named " + name, Severity::Error,
                                        {{"name", name}, {"checkpoints", history_.checkpoint_names()}}});
    finish(r);
    return r;
  }
  return apply("restore_checkpoint", {{"name", name}}, Commit::Push, [&](Document& d) {
    d = *state;
    OpResult r;
    r.success = true;
    r.changed = {"document"};
    return r;
  });
}

OpResult Session::save(const std::filesystem::path& path) {
  OpResult r = guarded([&] {
    // The saved log includes this save.
    log_.push_back(LogEntry{now_iso(), source_, "save", {{"path", to_utf8(path)}}, "ok", true});
    const Message m = save_project(history_.current(), log_, history_, path);
    log_.pop_back();
    if (m.code != "ok") return OpResult::fail(m);
    OpResult res;
    res.success = true;
    res.data["path"] = to_utf8(path);
    return res;
  });
  log_op("save", {{"path", to_utf8(path)}}, r);
  finish(r);
  return r;
}

OpResult Session::save_copy(const std::filesystem::path& path) const {
  return guarded([&] {
    const Message m = save_project(history_.current(), log_, history_, path);
    if (m.code != "ok") return OpResult::fail(m);
    OpResult res;
    res.success = true;
    res.data["path"] = to_utf8(path);
    return res;
  });
}

OpResult Session::load(const std::filesystem::path& path) {
  OpResult r = guarded([&] {
    auto lp = load_project(path);
    if (!lp) return OpResult::fail(lp.error());
    History h;
    lp->doc.revision = 1;
    h.replace_current(std::move(lp->doc));
    // Continue numbering after the highest automatic checkpoint, so none is overwritten.
    int autos = 0;
    for (const auto& [name, cp] : lp->checkpoints)
      if (name.rfind(kAutoCheckpoint, 0) == 0) autos = std::max(autos, auto_number(name));
    h.set_checkpoints(std::move(lp->checkpoints));
    // Commit only after everything loaded: a failed load leaves the session as it was.
    history_ = std::move(h);
    reconstruct_count_ = autos;
    log_ = std::move(lp->log);
    OpResult res;
    res.success = true;
    res.changed = {"document"};
    res.data["path"] = to_utf8(path);
    res.data["mesh_cache"] = history_.current().cached_solid != nullptr;
    res.data["checkpoints"] = history_.checkpoint_names();
    return res;
  });
  log_op("load", {{"path", to_utf8(path)}}, r);
  finish(r);
  return r;
}

}  // namespace pixel3d
