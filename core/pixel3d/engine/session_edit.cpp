// M4 editing commands. Each builds one EditOp (or a mask/part change) from the
// current document and commits it as a single transaction / undo step.
#include <algorithm>
#include <cmath>
#include <set>

#include "pixel3d/common/limits.hpp"
#include "pixel3d/common/path_utf8.hpp"
#include "pixel3d/engine/inspect.hpp"
#include "pixel3d/engine/session.hpp"
#include "pixel3d/polygon/raster.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

using pipeline::Stage;
using nlohmann::json;

namespace {

constexpr size_t kMaxEdits = 100000;

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

Unexpected bad(const std::string& code, const std::string& text, json where = {}) {
  return make_error(code, text, std::move(where));
}

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

Dims dims_of(const Document& d) { return d.align ? d.align->dims : Dims{}; }

// Geometry to edit: reconstructed, with part solids (rebuilt when the mesh came from a project cache).
std::optional<Message> ensure_editable(Document& d, pipeline::Notes& notes) {
  if (auto e = pipeline::ensure(d, Stage::Textured, {}, notes)) return e;
  if (!d.edited_parts)
    if (auto e = pipeline::build_parts(d, notes)) return e;
  return std::nullopt;
}

int allocate_part(Document& d) { return d.next_part_id++; }

void allocate_ids(EditOp& op, Document& d) {
  std::visit(overloaded{[&](VolumeEdit& e) {
                          if (e.add && e.part < 0 && e.new_part < 0) e.new_part = allocate_part(d);
                        },
                        [&](CutEdit& e) {
                          if (e.keep == 0 && e.new_part < 0) e.new_part = allocate_part(d);
                        },
                        [&](MirrorEdit& e) {
                          if (e.copy && e.new_part < 0) e.new_part = allocate_part(d);
                        },
                        [&](TransformEdit& e) {
                          if (e.copy && e.new_part < 0) e.new_part = allocate_part(d);
                        },
                        [&](SplitShellsEdit& e) {
                          for (int& p : e.new_parts)
                            if (p < 0) p = allocate_part(d);
                        },
                        [](auto&) {}},
             op);
  for (int p : created_parts(op)) d.next_part_id = std::max(d.next_part_id, p + 1);
}

std::optional<Message> need_part(const Document& d, int part) {
  if (d.edited_parts && d.edited_parts->has(part)) return std::nullopt;
  json ids = json::array();
  if (d.edited_parts)
    for (const auto& [id, m] : d.edited_parts->parts) ids.push_back(id);
  return Message{"edit.unknown_part", "no part " + std::to_string(part) + " in the model", Severity::Error,
                 {{"part", part}, {"parts", ids}}};
}

// The surface a view shows (1 pixel = 1 sample): depth and triangle per pixel.
RenderTarget surface_of(const Document& d, ViewRole view) {
  return render_mesh_flat(d.solid->mesh, camera_for_view(view, dims_of(d), 1));
}

json mismatch_json(const std::map<ViewRole, Mask2D>& masks, const Mesh& mesh, Dims dims) {
  json j = json::object();
  for (const auto& [role, m] : masks) {
    const RenderTarget rt = render_mesh_flat(mesh, camera_for_view(role, dims, 1));
    j[std::string(to_string(role))] = {{"mismatch_px", mask_xor_count(rt.coverage, m)}, {"iou", mask_iou(rt.coverage, m)}};
  }
  return j;
}

}  // namespace

json parts_json(const Document& d);

OpResult Session::commit_edit(const std::string& name, json params, const std::function<Expected<EditOp>(Document&)>& build) {
  return apply(name, std::move(params), Commit::Push, [&](Document& d) {
    OpResult r;
    pipeline::Notes setup;
    if (auto e = ensure_editable(d, setup)) return OpResult::fail(*e);
    const std::shared_ptr<const QualityMetrics> before = d.live;
    if (d.edits.size() >= kMaxEdits) return OpResult::fail(Message{"input.invalid", "too many edits", Severity::Error, {}});
    auto op = build(d);
    if (!op) return OpResult::fail(op.error());
    allocate_ids(*op, d);
    if (auto e = check_edit(*op)) return OpResult::fail(*e);
    d.edits.push_back(*op);
    pipeline::Notes notes;
    if (auto e = pipeline::apply_last_edit(d, notes)) return OpResult::fail(*e);
    if (auto e = pipeline::ensure(d, Stage::Textured, {}, notes)) return OpResult::fail(*e);
    for (const Message& m : notes) r.add(m);
    // A view that now matches its drawing less well is reported right away.
    if (before && d.live)
      for (const auto& [role, vm] : d.live->views) {
        const auto it = before->views.find(role);
        if (it == before->views.end() || vm.mismatch_px <= it->second.mismatch_px) continue;
        r.add(Message{"edit.projection_changed",
                      "the " + std::string(to_string(role)) + " view no longer matches its drawing as well (" +
                          std::to_string(it->second.mismatch_px) + " → " + std::to_string(vm.mismatch_px) + " px)",
                      Severity::Warning,
                      {{"view", to_string(role)}, {"before_px", it->second.mismatch_px}, {"after_px", vm.mismatch_px}}});
      }
    r.data = {{"edit", edit_to_json(*op)},
              {"edit_index", d.edits.size() - 1},
              {"created_parts", created_parts(*op)},
              {"parts", parts_json(d)},
              {"live", inspect_live(d)}};
    r.success = true;
    r.changed = {"edits", "mesh"};
    return r;
  });
}

OpResult Session::apply_edit(EditOp op) {
  return commit_edit(edit_name(op), edit_to_json(op), [op](Document&) -> Expected<EditOp> { return op; });
}

namespace {

// Builders shared by the committing command and its preview, so a preview is
// exactly what the commit will do.
Expected<EditOp> build_depth_stroke(Document& d, const DepthPaintRequest& req) {
  const ImageSize sz = view_image_size(req.view, dims_of(d));
  DepthStrokeEdit e;
  e.view = req.view;
  e.part = req.part;
  e.samples = req.samples.empty() ? depth_brush_samples(req.stroke, req.brush, sz.w, sz.h) : req.samples;
  if (e.samples.empty()) return bad("input.invalid", "the stroke changes no pixel (check strength, quantum and position)");
  if (req.part >= 0)
    if (auto m = need_part(d, req.part)) return Unexpected{*m};
  return EditOp{e};
}

Expected<EditOp> build_push_pull(Document& d, const PushPullRequest& req, json* clamped) {
  int face = req.face;
  if (req.view) face = pick_view_pixel(d.solid->mesh, *req.view, dims_of(d), req.u, req.v).face;
  const auto fp = face_plane(d.solid->mesh, face);
  if (!fp) return bad("edit.no_face", "no face at that position (select a face first)", {{"face", face}});
  double dist = req.snap ? std::round(req.distance) : req.distance;
  if (!std::isfinite(dist) || std::abs(dist) < 1e-9) return bad("input.invalid", "distance is zero after snapping");
  if (req.respect_bounds && dist > 0) {
    const Dims dims = dims_of(d);
    const double ext[3] = {static_cast<double>(dims.W), static_cast<double>(dims.H), static_cast<double>(dims.D)};
    const double n[3] = {fp->normal.x, fp->normal.y, fp->normal.z};
    const double o[3] = {fp->origin.x, fp->origin.y, fp->origin.z};
    for (int k = 0; k < 3; ++k) {
      if (std::abs(n[k]) < 0.999) continue;  // only axis-aligned faces have a box limit
      const double room = n[k] > 0 ? ext[k] - o[k] : o[k];
      if (dist > room + 1e-9) {
        if (clamped) *clamped = {{"requested", dist}, {"applied", std::max(0.0, room)}};
        dist = std::max(0.0, room);
      }
    }
    if (dist < 1e-9)
      return bad("edit.at_bound", "the face is already at the drawings' bounding box (use respect_bounds=false to go beyond)");
  }
  SweepEdit e;
  e.part = fp->part;
  e.origin = {fp->origin.x, fp->origin.y, fp->origin.z};
  e.u = {fp->u.x, fp->u.y, fp->u.z};
  e.v = {fp->v.x, fp->v.y, fp->v.z};
  e.normal = {fp->normal.x, fp->normal.y, fp->normal.z};
  e.polygon = fp->polygon;
  e.distance = dist;
  return EditOp{e};
}

}  // namespace

OpResult Session::paint_depth(const DepthPaintRequest& req) {
  json params{{"view", to_string(req.view)}, {"samples", req.samples.size()}, {"stroke_points", req.stroke.size()},
              {"radius", req.brush.radius}, {"strength", req.brush.strength}, {"raise", req.brush.raise}};
  return commit_edit("paint_depth", params, [req](Document& d) { return build_depth_stroke(d, req); });
}

OpResult Session::add_volume(ViewRole view, const Polygons2D& region, double from, double to, int part) {
  return commit_edit("add_volume", {{"view", to_string(view)}, {"from", from}, {"to", to}, {"part", part}},
                     [=](Document& d) -> Expected<EditOp> {
                       if (part >= 0)
                         if (auto m = need_part(d, part)) return Unexpected{*m};
                       return EditOp{VolumeEdit{"add_volume", true, view, region, from, to, part, -1}};
                     });
}

OpResult Session::erase_volume(ViewRole view, const Polygons2D& region, double from, double to, int part) {
  return commit_edit("erase_volume", {{"view", to_string(view)}, {"from", from}, {"to", to}, {"part", part}},
                     [=](Document& d) -> Expected<EditOp> {
                       if (part >= 0)
                         if (auto m = need_part(d, part)) return Unexpected{*m};
                       return EditOp{VolumeEdit{"erase_volume", false, view, region, from, to, part, -1}};
                     });
}

OpResult Session::extrude(ViewRole view, const Polygons2D& region, double distance, int part) {
  return commit_edit(
      "extrude", {{"view", to_string(view)}, {"distance", distance}, {"part", part}}, [=](Document& d) -> Expected<EditOp> {
        if (!std::isfinite(distance) || std::abs(distance) < 1e-9 || std::abs(distance) > 1e4)
          return bad("input.invalid", "distance must be non-zero and within ±1e4");
        const Dims dims = dims_of(d);
        const ImageSize sz = view_image_size(view, dims);
        const Mask2D inside = rasterize_pixel_centres(region, sz.w, sz.h);
        const RenderTarget rt = surface_of(d, view);
        double near = 1e30;
        std::map<int, int> votes;
        for (int v = 0; v < sz.h; ++v)
          for (int u = 0; u < sz.w; ++u) {
            if (!inside.get(u, v)) continue;
            const size_t i = static_cast<size_t>(v) * sz.w + u;
            if (rt.tri_id[i] < 0) continue;
            near = std::min(near, static_cast<double>(rt.depth[i]));
            ++votes[d.solid->mesh.tri_part[rt.tri_id[i]]];
          }
        if (votes.empty()) return bad("edit.no_surface", "no surface under the region in this view");
        int target = part;
        if (target < 0)
          target = std::max_element(votes.begin(), votes.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        if (auto m = need_part(d, target)) return Unexpected{*m};
        if (distance > 0) return EditOp{VolumeEdit{"extrude", true, view, region, near - distance, near + 0.25, target, -1}};
        return EditOp{VolumeEdit{"extrude", false, view, region, near - 0.5, near - distance, part, -1}};
      });
}

OpResult Session::push_pull(const PushPullRequest& req) {
  json params{{"face", req.face}, {"distance", req.distance}, {"snap", req.snap}, {"respect_bounds", req.respect_bounds}};
  if (req.view) params["view"] = to_string(*req.view), params["u"] = req.u, params["v"] = req.v;
  auto clamped = std::make_shared<json>();
  OpResult r = commit_edit("push_pull", params,
                           [req, clamped](Document& d) { return build_push_pull(d, req, clamped.get()); });
  if (r.success && !clamped->is_null())
    r.add(Message{"edit.clamped", "the pull was limited to the drawings' bounding box so the projections stay intact",
                  Severity::Info, *clamped});
  return r;
}

OpResult Session::cut(int part, int axis, double at, int keep) {
  return commit_edit("cut", {{"part", part}, {"axis", axis}, {"at", at}, {"keep", keep}}, [=](Document& d) -> Expected<EditOp> {
    if (keep != -1 && keep != 1) return bad("input.invalid", "keep must be -1 (negative side) or 1 (positive side)");
    if (part >= 0)
      if (auto m = need_part(d, part)) return Unexpected{*m};
    return EditOp{CutEdit{part, axis, at, keep, -1}};
  });
}

OpResult Session::split_part(int part, int axis, double at) {
  return commit_edit("split_part", {{"part", part}, {"axis", axis}, {"at", at}}, [=](Document& d) -> Expected<EditOp> {
    if (auto m = need_part(d, part)) return Unexpected{*m};
    return EditOp{CutEdit{part, axis, at, 0, -1}};
  });
}

OpResult Session::split_part_shells(int part) {
  return commit_edit("split_part", {{"part", part}, {"by", "shells"}}, [=](Document& d) -> Expected<EditOp> {
    if (auto m = need_part(d, part)) return Unexpected{*m};
    const size_t shells = d.edited_parts->parts.at(part).Decompose().size();
    if (shells <= 1) return bad("edit.no_effect", "the part is a single piece");
    return EditOp{SplitShellsEdit{part, std::vector<int>(shells - 1, -1)}};
  });
}

OpResult Session::merge_parts(const std::vector<int>& parts) {
  return commit_edit("merge_parts", {{"parts", parts}}, [=](Document& d) -> Expected<EditOp> {
    if (parts.size() < 2) return bad("input.invalid", "merge needs at least two parts");
    for (int p : parts)
      if (auto m = need_part(d, p)) return Unexpected{*m};
    return EditOp{MergeEdit{parts[0], std::vector<int>(parts.begin() + 1, parts.end())}};
  });
}

OpResult Session::mirror(int part, int axis, std::optional<double> at, bool copy) {
  return commit_edit("mirror", {{"part", part}, {"axis", axis}, {"copy", copy}}, [=](Document& d) -> Expected<EditOp> {
    if (axis < 0 || axis > 2) return bad("input.invalid", "axis must be 0 (x), 1 (y) or 2 (z)");
    if (part >= 0)
      if (auto m = need_part(d, part)) return Unexpected{*m};
    const Dims dims = dims_of(d);
    const double centre = (axis == 0 ? dims.W : axis == 1 ? dims.H : dims.D) / 2.0;
    return EditOp{MirrorEdit{part, axis, at.value_or(centre), copy, -1}};
  });
}

OpResult Session::transform_part(const TransformRequest& req) {
  json params{{"part", req.part},       {"translate", {req.translate.x, req.translate.y, req.translate.z}},
              {"rotate_axis", req.rotate_axis}, {"rotate_deg", req.rotate_deg}, {"scale", req.scale},
              {"copy", req.copy}};
  return commit_edit(req.copy ? "duplicate_part" : "transform_part", params, [req](Document& d) -> Expected<EditOp> {
    if (auto m = need_part(d, req.part)) return Unexpected{*m};
    TransformEdit e;
    e.part = req.part;
    vec3d t = req.translate;
    if (req.snap) t = {std::round(t.x), std::round(t.y), std::round(t.z)};
    e.translate = {t.x, t.y, t.z};
    e.rotate_axis = req.rotate_axis;
    e.rotate_deg = req.rotate_deg;
    e.scale = req.scale;
    vec3d pivot;
    if (req.pivot) {
      pivot = *req.pivot;
    } else {
      const manifold::Box b = d.edited_parts->parts.at(req.part).BoundingBox();
      pivot = {(b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2, (b.min.z + b.max.z) / 2};
    }
    e.pivot = {pivot.x, pivot.y, pivot.z};
    e.copy = req.copy;
    return EditOp{e};
  });
}

OpResult Session::delete_part(int part) {
  // Low-Poly Studio: parts are the user's masks (no visual-hull edit list): deleting one
  // removes its masks, name, type and depth override; the model rebuilds from the rest.
  if (document().settings.lowpoly.enabled)
    return apply("delete_part", {{"part", part}}, Commit::Push, [&](Document& d) {
      if (!d.masks.parts.contains(part))
        return OpResult::fail(Message{"edit.unknown_part", "no part " + std::to_string(part), Severity::Error, {{"part", part}}});
      d.masks.parts.erase(part);
      d.part_names.erase(part);
      d.part_types.erase(part);
      d.settings.lowpoly.depth_ratio.erase(part);
      pipeline::clear_from(d, Stage::Matched);
      OpResult r;
      r.data = {{"part", part}, {"deleted", true}, {"parts_left", d.masks.parts.size()}};
      r.success = true;
      r.changed = {"parts"};
      return r;
    });
  return commit_edit("delete_part", {{"part", part}}, [=](Document& d) -> Expected<EditOp> {
    if (auto m = need_part(d, part)) return Unexpected{*m};
    if (d.edited_parts->parts.size() <= 1) return bad("edit.empty_model", "cannot delete the last part");
    return EditOp{DeletePartEdit{part}};
  });
}

// ---- parts and masks ------------------------------------------------------------

OpResult Session::create_part(const std::string& name, bool define) {
  return apply("create_part", {{"name", name}, {"define", define}}, Commit::Push, [&](Document& d) {
    if (name.empty() || name.size() > 256)
      return OpResult::fail(Message{"input.invalid", "part names are 1-256 bytes", Severity::Error, {}});
    if (d.masks.parts.size() >= 10000) return OpResult::fail(Message{"input.invalid", "too many user parts", Severity::Error, {}});
    OpResult r;
    const Stage reached = pipeline::stage_of(d);
    const int id = allocate_part(d);
    d.masks.parts[id] = UserPart{define, std::nullopt, {}};
    d.part_names[id] = name;
    if (reached >= Stage::Matched) {
      pipeline::clear_from(d, Stage::Matched);
      if (auto e = refresh(d, reached, Stage::Matched, r)) return OpResult::fail(*e);
    }
    r.data = {{"part", id}, {"name", name}, {"define", define},
              {"next", "paint its pixels with paint_part_mask (part " + std::to_string(id) + ")"}};
    r.success = true;
    r.changed = {"parts"};
    return r;
  });
}

OpResult Session::rename_part(int part, const std::string& name) {
  return apply("rename_part", {{"part", part}, {"name", name}}, Commit::Push, [&](Document& d) {
    if (name.empty() || name.size() > 256)
      return OpResult::fail(Message{"input.invalid", "part names are 1-256 bytes", Severity::Error, {}});
    bool known = d.masks.parts.contains(part) || (d.edited_parts && d.edited_parts->has(part));
    if (d.correspondence)
      for (const Part3D& p : d.correspondence->parts) known |= p.id == part;
    if (!known) return OpResult::fail(Message{"edit.unknown_part", "no part " + std::to_string(part), Severity::Error, {{"part", part}}});
    d.part_names[part] = name;
    OpResult r;
    r.data = {{"part", part}, {"name", name}};
    r.success = true;
    r.changed = {"parts"};
    return r;
  });
}

OpResult Session::paint_part_mask(const MaskPaintRequest& req) {
  json params{{"part", req.part}, {"view", to_string(req.view)}, {"pixels", req.pixels.size()},
              {"stroke_points", req.stroke.size()}, {"erase", req.erase}};
  return apply("paint_part_mask", params, Commit::Push, [&](Document& d) {
    OpResult r;
    const auto up = d.masks.parts.find(req.part);
    if (up == d.masks.parts.end())
      return OpResult::fail(Message{"edit.unknown_part", "part " + std::to_string(req.part) + " is not a user part (create_part first)",
                                    Severity::Error, {{"part", req.part}}});
    const Stage reached = pipeline::stage_of(d);
    pipeline::Notes notes;
    if (auto e = pipeline::ensure(d, Stage::Aligned, {}, notes)) return OpResult::fail(*e);
    const SourceView* src = nullptr;
    for (const SourceView& v : d.views)
      if (v.role == req.view) src = &v;
    if (!src || !src->aligned)
      return OpResult::fail(Message{"input.invalid", "no " + std::string(to_string(req.view)) + " drawing", Severity::Error, {}});
    const int w = src->aligned->width(), h = src->aligned->height();
    Mask2D& lm = up->second.masks[req.view];
    if (lm.width() != w || lm.height() != h) lm = Mask2D(w, h);
    const Mask2D opaque = opaque_mask(*src->aligned, d.settings.alpha_threshold);
    Mask2D touched(w, h);
    if (!req.stroke.empty()) touched = mask_brush(req.stroke, req.brush, w, h);
    for (const auto& [u, v] : req.pixels)
      if (u >= 0 && v >= 0 && u < w && v < h) touched.set(u, v, true);
    int changed = 0, skipped = 0;
    for (int v = 0; v < h; ++v)
      for (int u = 0; u < w; ++u) {
        if (!touched.get(u, v)) continue;
        if (req.erase) {
          if (lm.get(u, v)) lm.set(u, v, false), ++changed;
        } else if (!opaque.get(u, v)) {
          ++skipped;  // masks only cover drawn pixels; the artwork is never changed
        } else if (!lm.get(u, v)) {
          lm.set(u, v, true);
          ++changed;
        }
      }
    if (skipped > 0)
      r.add(Message{"mask.transparent_skipped", std::to_string(skipped) + " transparent pixel(s) were not masked",
                    Severity::Info, {{"pixels", skipped}}});
    if (changed == 0) r.add(Message{"mask.no_change", "the mask did not change", Severity::Info, {}});
    if (reached >= Stage::Matched) {
      pipeline::clear_from(d, up->second.define ? Stage::Matched : Stage::Reconstructed);
      if (auto e = refresh(d, reached, Stage::Aligned, r)) return OpResult::fail(*e);
    }
    const int mask_pixels = d.masks.parts.at(req.part).masks.at(req.view).count();
    r.data = {{"part", req.part}, {"view", to_string(req.view)}, {"changed_pixels", changed}, {"mask_pixels", mask_pixels},
              {"live", inspect_live(d)}};
    r.success = true;
    r.changed = {"masks"};
    return r;
  });
}

OpResult Session::sync_part_masks(int part) {
  return apply("sync_part_masks", {{"part", part}}, Commit::Push, [&](Document& d) {
    OpResult r;
    if (!d.masks.parts.contains(part))
      return OpResult::fail(Message{"edit.unknown_part", "part " + std::to_string(part) + " is not a user part", Severity::Error, {}});
    pipeline::Notes notes;
    if (auto e = ensure_editable(d, notes)) return OpResult::fail(*e);
    const std::vector<ViewRole> changed = pixel3d::sync_part_masks(d.masks, part, d.solid->mesh, dims_of(d), pipeline::masks_of(d));
    json views = json::array();
    for (ViewRole v : changed) views.push_back(to_string(v));
    if (changed.empty()) {
      r.add(Message{"mask.no_change", "every view already has a mask for this part, or it has no geometry", Severity::Info, {}});
    } else {
      pipeline::clear_from(d, d.masks.parts.at(part).define ? Stage::Matched : Stage::Reconstructed);
      if (auto e = refresh(d, Stage::Textured, Stage::Textured, r)) return OpResult::fail(*e);
    }
    r.data = {{"part", part}, {"views", views}, {"live", inspect_live(d)}};
    r.success = true;
    r.changed = {"masks"};
    return r;
  });
}

// ---- preview --------------------------------------------------------------------

OpResult Session::preview_edit(const EditOp& op, Mesh* mesh) const {
  return preview_built([&](Document&) -> Expected<EditOp> { return op; }, mesh);
}

OpResult Session::preview_paint_depth(const DepthPaintRequest& r, Mesh* mesh) const {
  return preview_built([&](Document& d) { return build_depth_stroke(d, r); }, mesh);
}

OpResult Session::preview_push_pull(const PushPullRequest& r, Mesh* mesh) const {
  return preview_built([&](Document& d) { return build_push_pull(d, r, nullptr); }, mesh);
}

OpResult Session::preview_built(const std::function<Expected<EditOp>(Document&)>& build, Mesh* mesh) const {
  return guarded([&] {
    Document d = history_.current();
    pipeline::Notes notes;
    if (auto e = ensure_editable(d, notes)) return OpResult::fail(*e);
    auto built = build(d);
    if (!built) return OpResult::fail(built.error());
    EditOp o = *built;
    allocate_ids(o, d);
    if (auto e = check_edit(o)) return OpResult::fail(*e);
    PartGeometry g = *d.edited_parts;
    std::vector<Message> edit_notes;
    if (auto e = pixel3d::apply_edit(g, o, dims_of(d), &edit_notes)) return OpResult::fail(*e);
    const SolidResult s = assemble_parts(g, d.cleanup_options);
    OpResult r;
    for (const Message& m : edit_notes) r.add(m);
    r.data = {{"edit", edit_to_json(o)},
              {"triangles", s.topology.triangle_count},
              {"volume", s.topology.signed_volume},
              {"watertight", s.topology.watertight},
              {"views", mismatch_json(pipeline::masks_of(d), s.mesh, dims_of(d))}};
    if (mesh) *mesh = s.mesh;
    r.success = true;
    finish(r);
    return r;
  });
}

// ---- selection -------------------------------------------------------------------

namespace {

std::vector<ViewRole> roles_of(const Document& d) {
  std::vector<ViewRole> r;
  for (const SourceView& v : d.views) r.push_back(v.role);
  return r;
}

std::vector<int> all_parts(const Mesh& m) {
  std::set<int> s(m.tri_part.begin(), m.tri_part.end());
  return {s.begin(), s.end()};
}

}  // namespace

OpResult Session::select_at_pixel(ViewRole view, int u, int v, SelectMode mode) {
  const Document& d = history_.current();
  if (!d.solid) return OpResult::fail(Message{"state.not_ready", "reconstruct first", Severity::Error, {}});
  const PickHit hit = pick_view_pixel(d.solid->mesh, view, dims_of(d), u, v);
  Selection s;
  s.mode = mode;
  s.revision = d.revision;
  if (hit.tri >= 0) {
    s.hit = hit;
    if (mode == SelectMode::Object) s.parts = all_parts(d.solid->mesh);
    else s.parts = {hit.part};
    if (mode == SelectMode::Face) s.face = hit.face;
  }
  selection_ = s;
  return get_selection();
}

OpResult Session::select_at_ray(vec3d origin, vec3d dir, SelectMode mode) {
  const Document& d = history_.current();
  if (!d.solid) return OpResult::fail(Message{"state.not_ready", "reconstruct first", Severity::Error, {}});
  if (length(dir) < 1e-12) return OpResult::fail(Message{"input.invalid", "the ray direction is zero", Severity::Error, {}});
  const PickHit hit = pick_ray(d.solid->mesh, origin, dir);
  Selection s;
  s.mode = mode;
  s.revision = d.revision;
  if (hit.tri >= 0) {
    s.hit = hit;
    if (mode == SelectMode::Object) s.parts = all_parts(d.solid->mesh);
    else s.parts = {hit.part};
    if (mode == SelectMode::Face) s.face = hit.face;
  }
  selection_ = s;
  return get_selection();
}

OpResult Session::select_parts(const std::vector<int>& parts, SelectMode mode) {
  const Document& d = history_.current();
  if (!d.solid) return OpResult::fail(Message{"state.not_ready", "reconstruct first", Severity::Error, {}});
  const std::vector<int> have = all_parts(d.solid->mesh);
  for (int p : parts)
    if (std::find(have.begin(), have.end(), p) == have.end())
      return OpResult::fail(Message{"edit.unknown_part", "no part " + std::to_string(p) + " in the model", Severity::Error,
                                    {{"part", p}, {"parts", have}}});
  Selection s;
  s.mode = mode == SelectMode::Face ? SelectMode::Part : mode;
  s.parts = mode == SelectMode::Object ? have : parts;
  s.revision = d.revision;
  selection_ = s;
  return get_selection();
}

OpResult Session::clear_selection() {
  selection_ = Selection{};
  return get_selection();
}

OpResult Session::get_selection() const {
  return guarded([&] {
    const Document& d = history_.current();
    OpResult r;
    if (d.solid && d.align) {
      r.data = selection_json(selection_, d.solid->mesh, dims_of(d), roles_of(d));
    } else {
      r.data = {{"mode", to_string(selection_.mode)}, {"parts", json::array()}, {"face", nullptr}};
    }
    // Faces are renumbered by every geometry change; parts keep their ids.
    r.data["stale"] = !selection_.empty() && selection_.revision != d.revision;
    json names = json::object();
    for (int p : selection_.parts)
      if (const auto it = d.part_names.find(p); it != d.part_names.end()) names[std::to_string(p)] = it->second;
    r.data["names"] = names;
    r.success = true;
    finish(r);
    return r;
  });
}

// ---- single view ------------------------------------------------------------------

OpResult Session::set_single_view(const SingleViewSettings& s) {
  return apply("set_single_view", single_view_to_json(s), Commit::Push, [&](Document& d) {
    if (!valid_depth_preset(s.preset) || !std::isfinite(s.depth) || s.depth < 0 || s.depth > 4096)
      return OpResult::fail(Message{"input.invalid", "depth preset must be flat, thin, medium, thick or custom (depth 0-4096)",
                                    Severity::Error, {}});
    if (s.enabled && d.views.size() != 1)
      return OpResult::fail(Message{"view.single_view", "single view mode needs exactly one drawing (the project has " +
                                                            std::to_string(d.views.size()) + ")",
                                    Severity::Error, {}});
    OpResult r;
    const Stage reached = pipeline::stage_of(d);
    SingleViewSettings sv = s;
    if (s.enabled && !d.views.empty()) sv.role = d.views.front().role;
    d.single_view = sv;
    pipeline::clear_from(d, Stage::Aligned);
    if (reached >= Stage::Aligned)
      if (auto e = refresh(d, reached, Stage::Aligned, r)) return OpResult::fail(*e);
    r.data = single_view_to_json(sv);
    r.success = true;
    r.changed = {"single_view"};
    return r;
  });
}

OpResult Session::set_single_view_depth(const std::string& preset, std::optional<double> depth) {
  SingleViewSettings s = history_.current().single_view;
  if (!s.enabled) return OpResult::fail(Message{"state.not_ready", "the project is not in single view mode", Severity::Error, {}});
  s.preset = depth ? "custom" : preset;
  if (depth) s.depth = *depth;
  s.user_depth = true;
  if (!valid_depth_preset(s.preset) || (depth && (!std::isfinite(*depth) || *depth <= 0 || *depth > 4096)))
    return OpResult::fail(Message{"input.invalid", "use flat, thin, medium or thick, or a depth in (0, 4096]", Severity::Error, {}});
  return set_single_view(s);
}

}  // namespace pixel3d
