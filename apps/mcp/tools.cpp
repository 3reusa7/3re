#include "mcp/tools.hpp"

#include "mcp/tools_detail.hpp"
#include "pixel3d/engine/inspect.hpp"

namespace pixel3d::mcp {

using nlohmann::json;
using namespace schema;

OpResult describe(const Session& s, json data) {
  OpResult r = s.current_status();
  r.data = std::move(data);
  return r;
}

namespace {

ToleranceProfile parse_profile(const std::string& field, const std::string& s) {
  if (s == "strict") return ToleranceProfile::Strict;
  if (s == "hand") return ToleranceProfile::Hand;
  throw ArgError(field, "must be 'strict' or 'hand'");
}

ToolOutput out(OpResult r) { return ToolOutput{std::move(r), {}}; }

}  // namespace

StylePreset parse_style(const char* field, const std::string& v) {
  const auto p = style_preset_from_string(v);
  if (!p) throw ArgError(field, "must be crisp, soft, lowpoly or clay");
  return *p;
}

namespace {

// --- project -------------------------------------------------------------

NormalizeUpdate normalize_args(const Args& a) {
  NormalizeUpdate u;
  u.enabled = a.opt_bool("normalize");
  u.target_size = a.opt_int("target_size", 16, 512);
  u.bg_tolerance = a.opt_int("bg_tolerance", 0, 255);
  u.axis_tolerance = a.opt_num("axis_tolerance", 0, 1);
  return u;
}

ToolOutput new_project(ToolContext& c, const Args& a) {
  a.allow_only({"name", "pixels_per_meter", "profile", "alpha_threshold", "normalize", "target_size", "bg_tolerance",
                "axis_tolerance", "style", "mode"});
  ProjectSettings ps;
  if (auto v = a.opt_str("mode")) {
    if (*v != "standard" && *v != "lowpoly_studio") throw ArgError("mode", "must be standard or lowpoly_studio");
    ps.lowpoly.enabled = *v == "lowpoly_studio";
  }
  if (auto v = a.opt_str("style")) ps.style.preset = parse_style("style", *v);
  if (auto v = a.opt_str("name")) ps.name = *v;
  if (auto v = a.opt_num("pixels_per_meter", 1e-3, 1e6)) ps.pixels_per_meter = *v;
  if (auto v = a.opt_str("profile")) ps.profile = parse_profile("profile", *v);
  if (auto v = a.opt_int("alpha_threshold", 1, 255)) ps.alpha_threshold = static_cast<uint8_t>(*v);
  ps.normalize = normalize_args(a).applied_to(ps.normalize);
  return out(c.session.new_project(ps));
}

ToolOutput open_project(ToolContext& c, const Args& a) {
  a.allow_only({"path"});
  return out(c.session.load(c.paths.resolve("path", a.str("path"))));
}

ToolOutput save_project(ToolContext& c, const Args& a) {
  a.allow_only({"path"});
  return out(c.session.save(c.paths.resolve("path", a.str("path"))));
}

ToolOutput update_settings(ToolContext& c, const Args& a) {
  a.allow_only({"name", "pixels_per_meter", "profile", "normalize", "target_size", "bg_tolerance", "axis_tolerance",
                "style"});
  SettingsUpdate u;
  u.name = a.opt_str("name");
  u.pixels_per_meter = a.opt_num("pixels_per_meter", 1e-3, 1e6);
  if (auto v = a.opt_str("profile")) u.profile = parse_profile("profile", *v);
  u.normalize = normalize_args(a);
  if (auto v = a.opt_str("style")) {
    // The preset changes; the project's tuning and materials stay (set_style changes them).
    StyleSettings st = c.session.document().settings.style;
    st.preset = parse_style("style", *v);
    u.style = st;
  }
  if (!u.name && !u.pixels_per_meter && !u.profile && u.normalize.empty() && !u.style)
    throw ArgError("arguments", "nothing to change");
  return out(c.session.update_settings(u));
}

ToolOutput set_style(ToolContext& c, const Args& a) {
  a.allow_only({"style", "radius", "exponent", "bevel", "smooth_angle", "shade_radius", "ao", "symmetry", "materials",
                "reset"});
  const StyleSettings current = c.session.document().settings.style;
  StyleSettings st = current;
  if (a.opt_bool("reset").value_or(false)) {
    st = StyleSettings{};
    st.preset = current.preset;
  }
  if (auto v = a.opt_str("style")) st.preset = parse_style("style", *v);
  if (auto v = a.opt_num("radius", 0, 8)) st.radius = *v;
  if (auto v = a.opt_num("exponent", 1, 8)) st.exponent = *v;
  if (auto v = a.opt_num("bevel", 0, 2)) st.bevel = *v;
  if (auto v = a.opt_num("smooth_angle", 0, 90)) st.smooth_angle = *v;
  if (auto v = a.opt_num("shade_radius", 0, 8)) st.shade_radius = *v;
  if (auto v = a.opt_bool("ao")) st.ao = *v;
  if (auto v = a.opt_bool("symmetry")) st.symmetry = *v;
  if (a.has("materials")) {
    const json& list = a.raw("materials");
    if (!list.is_array()) throw ArgError("materials", "must be an array of {family, roughness?, metallic?, emissive?}");
    for (size_t i = 0; i < list.size(); ++i) {
      const std::string field = "materials[" + std::to_string(i) + "]";
      if (!list[i].is_object()) throw ArgError(field, "must be an object {family, roughness?, metallic?, emissive?}");
      const Args m(list[i]);
      m.allow_only({"family", "roughness", "metallic", "emissive"});
      FamilyMaterial fm;
      fm.family = m.opt_int("family", 0, 4096).value_or(-1);
      if (fm.family < 0) throw ArgError(field + ".family", "is required (a material family index)");
      fm.roughness = m.opt_num("roughness", 0, 1);
      fm.metallic = m.opt_num("metallic", 0, 1);
      fm.emissive = m.opt_num("emissive", 0, 100);
      // A family named again replaces its earlier override.
      std::erase_if(st.materials, [&](const FamilyMaterial& o) { return o.family == fm.family; });
      st.materials.push_back(fm);
    }
  }
  SettingsUpdate u;
  u.style = st;
  return out(c.session.update_settings(u));
}

ToolOutput import_views(ToolContext& c, const Args& a) {
  a.allow_only({"views", "front", "side", "top", "back", "right", "bottom", "normalize", "target_size", "bg_tolerance",
                "axis_tolerance"});
  std::vector<ViewImport> views;
  if (a.has("views")) {
    const json& list = a.raw("views");
    if (!list.is_array() || list.empty() || list.size() > 6) throw ArgError("views", "must be an array of 1-6 views");
    for (size_t i = 0; i < list.size(); ++i) {
      const std::string field = "views[" + std::to_string(i) + "]";
      if (!list[i].is_object()) throw ArgError(field, "must be an object {role, path, flip_x?}");
      const Args v(list[i]);
      v.allow_only({"role", "path", "flip_x"});
      views.push_back({v.role("role"), c.paths.resolve(field + ".path", v.str("path")), v.opt_bool("flip_x")});
    }
  }
  for (const char* r : {"front", "side", "top", "back", "right", "bottom"})
    if (a.has(r)) views.push_back({parse_role(r, r), c.paths.resolve(r, a.str(r)), std::nullopt});
  if (views.empty()) throw ArgError("views", "give at least one view (front, side and top are required to reconstruct)");
  return out(c.session.import_views(views, normalize_args(a)));
}

// --- pipeline ------------------------------------------------------------

ToolOutput auto_align(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.auto_align());
}

ToolOutput analyze_views(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.analyze());
}

ToolOutput segment_parts(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.segment_parts());
}

ToolOutput match_parts(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.match_parts());
}

ToolOutput reconstruct(ToolContext& c, const Args& a) {
  a.allow_only({"mode"});
  ReconstructOptions ro;
  if (auto m = a.opt_str("mode")) {
    if (*m == "voxel") ro.mode = ReconstructMode::Voxel;
    else if (*m != "true_solid") throw ArgError("mode", "must be 'true_solid' or 'voxel'");
  }
  return out(c.session.reconstruct(ro, c.job));
}

ToolOutput get_conflicts(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.get_conflicts());
}

ToolOutput get_inferred_geometry(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.get_inferred_geometry());
}

Polygons2D region_from(const Args& a, ToolContext& c, ViewRole* view) {
  int given = 0;
  for (const char* k : {"rect", "polygon", "polygons", "conflict_index"}) given += a.has(k) ? 1 : 0;
  if (given != 1) throw ArgError("region", "give exactly one of rect, polygon, polygons or conflict_index");
  auto point = [](const std::string& field, const json& p) {
    if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
      throw ArgError(field, "points are [x, y] number pairs");
    return vec2d{p[0].get<double>(), p[1].get<double>()};
  };
  auto ring = [&](const std::string& field, const json& r) {
    if (!r.is_array() || r.size() < 3 || r.size() > 100000) throw ArgError(field, "a polygon needs 3 to 100000 points");
    Ring out;
    for (const auto& p : r) out.push_back(point(field, p));
    return out;
  };
  if (a.has("rect")) {
    const json& r = a.raw("rect");
    if (!r.is_array() || r.size() != 4) throw ArgError("rect", "must be [x0, y0, x1, y1]");
    for (const auto& v : r)
      if (!v.is_number()) throw ArgError("rect", "must be [x0, y0, x1, y1]");
    const double x0 = r[0].get<double>(), y0 = r[1].get<double>(), x1 = r[2].get<double>(), y1 = r[3].get<double>();
    if (!(x1 > x0 && y1 > y0)) throw ArgError("rect", "needs x1 > x0 and y1 > y0");
    return rect_polys(x0, y0, x1, y1);
  }
  if (a.has("polygon")) return {ring("polygon", a.raw("polygon"))};
  if (a.has("polygons")) {
    const json& ps = a.raw("polygons");
    if (!ps.is_array() || ps.empty() || ps.size() > 1000) throw ArgError("polygons", "must be a list of 1-1000 rings");
    Polygons2D out;
    for (const auto& r : ps) out.push_back(ring("polygons", r));
    return out;
  }
  // A conflict reported by get_conflicts that carries a view region (possible_recess).
  const int idx = *a.opt_int("conflict_index", 0, 1 << 20);
  const OpResult conflicts = c.session.get_conflicts();
  const json& list = conflicts.data["conflicts"];
  if (idx >= static_cast<int>(list.size())) throw ArgError("conflict_index", "no conflict with this index (see get_conflicts)");
  const json& where = list[static_cast<size_t>(idx)].value("where", json::object());
  if (!where.contains("box") || !where.contains("view"))
    throw ArgError("conflict_index", "this conflict has no view region (use rect or polygon)");
  const ViewRole cv = parse_role("conflict_index", where["view"].get<std::string>());
  if (a.has("view") && cv != *view) throw ArgError("view", "the conflict belongs to view " + std::string(to_string(cv)));
  *view = cv;
  const json& b = where["box"];
  return rect_polys(b[0].get<double>(), b[1].get<double>(), b[2].get<double>(), b[3].get<double>());
}

ToolOutput set_part_depth(ToolContext& c, const Args& a) {
  a.allow_only({"view", "rect", "polygon", "polygons", "conflict_index", "depth_from", "depth_to", "flush", "label"});
  DepthConstraint dc;
  if (auto v = a.opt_role("view")) dc.view = *v;
  else if (!a.has("conflict_index")) throw ArgError("view", "required (or give conflict_index)");
  dc.region = region_from(a, c, &dc.view);
  const bool flush = a.opt_bool("flush").value_or(false);
  if (flush) {
    if (a.has("depth_from") || a.has("depth_to")) throw ArgError("flush", "flush cannot be combined with depth_from/depth_to");
    dc.depth_from = 0;
    dc.depth_to = 1e9;
    dc.label = "flush";
  } else {
    dc.depth_from = a.num("depth_from", -1e6, 1e6);
    dc.depth_to = a.opt_num("depth_to", -1e6, 1e9).value_or(1e9);
  }
  if (auto l = a.opt_str("label")) dc.label = *l;
  return out(c.session.set_depth(dc));
}

ToolOutput edit_part(ToolContext& c, const Args& a) {
  a.allow_only({"keep", "remove", "decisions"});
  PartEdit e;
  for (int id : a.int_list("keep", 0, 1 << 30, 100000)) e.decisions.push_back({id, true});
  for (int id : a.int_list("remove", 0, 1 << 30, 100000)) e.decisions.push_back({id, false});
  if (a.has("decisions")) {
    const json& d = a.raw("decisions");
    if (!d.is_array() || d.size() > 100000) throw ArgError("decisions", "must be an array of {candidate, keep}");
    for (const auto& x : d) {
      if (!x.is_object()) throw ArgError("decisions", "must be an array of {candidate, keep}");
      const Args one(x);
      one.allow_only({"candidate", "keep"});
      const auto keep = one.opt_bool("keep");
      if (!keep) throw ArgError("decisions.keep", "required");
      e.decisions.push_back({*one.opt_int("candidate", 0, 1 << 30), *keep});
    }
  }
  if (e.decisions.empty()) throw ArgError("keep", "give candidate ids in keep, remove or decisions");
  return out(c.session.edit_part(e));
}

ToolOutput cleanup_mesh(ToolContext& c, const Args& a) {
  a.allow_only({"min_fragment_volume"});
  CleanupOptions o = c.session.document().cleanup_options;
  if (auto v = a.opt_num("min_fragment_volume", 0, 1e9)) o.min_fragment_volume = *v;
  return out(c.session.cleanup(o));
}

ToolOutput project_textures(ToolContext& c, const Args& a) {
  a.allow_only({"allow_opposite_projection", "visibility_fraction", "visibility_samples"});
  TextureOptions o = c.session.document().texture_options;
  if (auto v = a.opt_bool("allow_opposite_projection")) o.allow_opposite_projection = *v;
  if (auto v = a.opt_num("visibility_fraction", 0, 1)) o.visibility_fraction = *v;
  if (auto v = a.opt_int("visibility_samples", 1, 1024)) o.visibility_samples = *v;
  return out(c.session.project_textures(o));
}

ToolOutput assign_texture(ToolContext& c, const Args& a) {
  a.allow_only({"triangles", "view", "mode"});
  TextureAssignment t;
  t.tris = a.int_list("triangles", 0, 1 << 30, 1 << 20);
  if (t.tris.empty()) throw ArgError("triangles", "required");
  t.view = a.role("view");
  const std::string mode = a.opt_str("mode").value_or("manual");
  if (mode == "flat_palette") t.mode = SurfaceMode::FlatPalette;
  else if (mode == "manual") t.mode = SurfaceMode::Manual;
  else throw ArgError("mode", "must be 'manual' (project from view) or 'flat_palette'");
  return out(c.session.assign_texture(t));
}

ToolOutput validate(ToolContext& c, const Args& a) {
  a.allow_only({"profile", "glb_roundtrip"});
  ValidateOptions o;
  if (auto p = a.opt_str("profile")) o.profile = parse_profile("profile", *p);
  if (auto g = a.opt_bool("glb_roundtrip")) o.glb_roundtrip = *g;
  return out(c.session.validate(o, c.job));
}

// --- history, export, inspection -----------------------------------------

ToolOutput undo(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.undo());
}

ToolOutput redo(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.redo());
}

ToolOutput checkpoint(ToolContext& c, const Args& a) {
  a.allow_only({"name"});
  return out(c.session.create_checkpoint(a.str("name")));
}

ToolOutput restore_checkpoint(ToolContext& c, const Args& a) {
  a.allow_only({"name"});
  return out(c.session.restore_checkpoint(a.str("name")));
}

ToolOutput export_glb(ToolContext& c, const Args& a) {
  a.allow_only({"path", "accept_status"});
  GlbExportRequest r;
  r.path = c.paths.resolve("path", a.str("path"));
  if (auto s = a.opt_str("accept_status")) {
    if (*s == "NEEDS_REVIEW") r.allow_needs_review = true;
    else if (*s == "FAIL") r.allow_needs_review = r.allow_fail = true;
    else if (*s != "PASS") throw ArgError("accept_status", "must be PASS, NEEDS_REVIEW or FAIL");
  }
  return out(c.session.export_glb(r));
}

ToolOutput export_model(ToolContext& c, const Args& a) {
  a.allow_only({"path", "format", "preset", "units", "front", "unlit", "accept_status"});
  ModelExportRequest r;
  r.path = c.paths.resolve("path", a.str("path"));
  if (auto f = a.opt_str("format")) {
    r.format = export_format_from_string(*f);
    if (!r.format) throw ArgError("format", "must be glb, gltf or obj");
  }
  if (auto p = a.opt_str("preset")) {
    const auto preset = engine_preset_from_string(*p);
    if (!preset) throw ArgError("preset", "must be generic, godot, unity or roblox");
    r.preset = *preset;
  }
  if (auto u = a.opt_str("units")) {
    r.units = export_units_from_string(*u);
    if (!r.units) throw ArgError("units", "must be metres, centimetres, studs or pixels");
  }
  if (auto f = a.opt_str("front")) {
    if (*f != "+z" && *f != "-z") throw ArgError("front", "must be +z or -z");
    r.front_negative_z = *f == "-z";
  }
  r.unlit = a.opt_bool("unlit").value_or(false);
  if (auto s = a.opt_str("accept_status")) {
    if (*s == "NEEDS_REVIEW") r.allow_needs_review = true;
    else if (*s == "FAIL") r.allow_needs_review = r.allow_fail = true;
    else if (*s != "PASS") throw ArgError("accept_status", "must be PASS, NEEDS_REVIEW or FAIL");
  }
  return out(c.session.export_model(r));
}

ToolOutput inspect(ToolContext& c, const Args& a) {
  a.allow_only({"what"});
  const std::string what = a.str("what");
  const Session& s = c.session;
  json data;
  if (what == "project") data = inspect_project(s);
  else if (what == "views") data = inspect_views(s.document());
  else if (what == "parts") data = inspect_parts(s.document());
  else if (what == "mesh") data = inspect_mesh(s.document());
  else if (what == "conflicts") data = inspect_conflicts(s);
  else if (what == "diagnostics") data = inspect_diagnostics(s);
  else if (what == "validation") data = inspect_validation(s.document());
  else if (what == "edits") data = inspect_edits(s.document());
  else if (what == "live") data = inspect_live(s.document());
  else if (what == "lowpoly") data = inspect_lowpoly(s.document());
  else throw ArgError("what", "must be project, views, parts, mesh, conflicts, diagnostics, validation, edits, live or lowpoly");
  return out(describe(s, std::move(data)));
}

}  // namespace

const std::vector<ToolDef>& tool_catalog() {
  static const std::vector<ToolDef> tools = [] {
    std::vector<ToolDef> t;
    const json path_arg = str("File path (absolute, or relative to the server's working directory).");
    const json profile = enumeration("Tolerance profile: strict (synthetic art) or hand (hand-drawn art, default).",
                                     {"strict", "hand"});
    t.push_back({"new_project", "New project",
                 "Start a new, empty project (and a fresh history). Settings: name, pixels_per_meter (export scale, "
                 "default 32), profile, alpha_threshold (default 128), and input normalisation (normalize, "
                 "target_size, bg_tolerance, axis_tolerance).",
                 object(with_normalize({{"name", str("Project name.")},
                                        {"pixels_per_meter", number("Source pixels per metre in the exported GLB.", 0.001, 1e6)},
                                        {"profile", profile},
                                        {"alpha_threshold", integer("Pixels with alpha >= this are opaque.", 1, 255)},
                                        {"style", style_enum()},
                                        {"mode", enumeration("standard (default: the visual-hull model from Front, Side and "
                                                             "Top) or lowpoly_studio (a rigged low-poly character from "
                                                             "Front with optional Side/Top; build with lowpoly_build).",
                                                             {"standard", "lowpoly_studio"})}})),
                 false, true, false, new_project});
    t.push_back({"open_project", "Open project", "Open a .p3d project (starts a fresh history; the mesh cache is reused when valid).",
                 object({{"path", path_arg}}, {"path"}), false, true, false, open_project});
    t.push_back({"save_project", "Save project", "Save the project (.p3d: sources, decisions, checkpoints, mesh cache, log). Atomic.",
                 object({{"path", path_arg}}, {"path"}), true, false, true, save_project});
    t.push_back({"update_settings", "Update settings",
                 "Change the project name, export scale, tolerance profile, input normalisation or model style "
                 "(undoable; normalisation changes re-align the views; a style change keeps the crisp model and "
                 "re-validates the polished one).",
                 object(with_normalize({{"name", str("Project name.")},
                                        {"pixels_per_meter", number("Source pixels per metre in the exported GLB.", 0.001, 1e6)},
                                        {"profile", profile},
                                        {"style", style_enum()}})),
                 false, false, true, update_settings});
    t.push_back({"set_style", "Set style",
                 "Model style and its tuning (undoable). Unset values keep the project's; reset=true returns every "
                 "tuning value and material to the style's defaults. Rounding is always clamped so every drawn "
                 "pixel stays covered (never loosened). Materials: PBR per colour family (validate's report lists "
                 "them); metal and glow are never guessed, only set here. The polished model is built at validate "
                 "and is what is validated and exported (GLB carries the maps and materials).",
                 object({{"style", style_enum()},
                         {"radius", number("Corner radius in source px (soft/clay 1.5, lowpoly 1.0).", 0, 8)},
                         {"exponent", number("Corner shape: 2 round, 1 chamfer, >2 squarer.", 1, 8)},
                         {"bevel", number("Small bevel of corners too thin to round, px (default 0.2).", 0, 2)},
                         {"smooth_angle", number("Smooth shading below this angle in degrees (0 = flat).", 0, 90)},
                         {"shade_radius", number("Shading-only bevel radius in px (normal map; 0 = off).", 0, 8)},
                         {"ao", boolean("Bake ambient occlusion.")},
                         {"symmetry", boolean("Mirror drawings that are pixel-exact mirror images (default true).")},
                         {"materials", array(object({{"family", integer("Material family index.", 0, 4096)},
                                                     {"roughness", number("0 glossy .. 1 matte.", 0, 1)},
                                                     {"metallic", number("0 .. 1.", 0, 1)},
                                                     {"emissive", number("Glow strength (0 = none).", 0, 100)}},
                                                    {"family"}),
                                             "Per-family material overrides.")},
                         {"reset", boolean("Start from the style's defaults.")}}),
                 false, false, true, set_style});
    const json role_path = str("Image path of this view (PNG, JPEG, BMP, GIF first frame, TGA).");
    t.push_back({"import_views", "Import views",
                 "Import orthographic pixel-art views. Front, Side (seen from the LEFT: the object's front points to the "
                 "image right) and Top (front edge at the image bottom) are required; Back, Right, Bottom are optional "
                 "and resolve ambiguities. Either role keys or a views array. Real-world images (opaque or white "
                 "backgrounds, anti-aliasing, JPEG noise, AI pixel-style art, oversize art) are normalised at "
                 "auto_align and every change is reported (input.* messages); the normalisation arguments set the "
                 "project's settings in the same undo step.",
                 object(with_normalize({{"front", role_path},
                         {"side", role_path},
                         {"top", role_path},
                         {"back", role_path},
                         {"right", role_path},
                         {"bottom", role_path},
                         {"views", array(object({{"role", role_enum("View role.")},
                                                 {"path", role_path},
                                                 {"flip_x", boolean("Force (true) or forbid (false) mirroring.")}},
                                                {"role", "path"}),
                                         "Alternative form with per-view options.")}})),
                 false, false, false, import_views});
    t.push_back({"auto_align", "Auto-align", "Normalise pixel scale, crop, detect mirrored views, reconcile extents (W, H, D).",
                 object({}), false, false, true, auto_align});
    t.push_back({"analyze_views", "Analyze views",
                 "Report scale, flips, unexplained pixels, material families and Y-bands (computes alignment and "
                 "segmentation if missing; no undo step).",
                 object({}), true, false, true, analyze_views});
    t.push_back({"segment_parts", "Segment parts", "Compute material regions, Y-bands and strands per view.", object({}),
                 false, false, true, segment_parts});
    t.push_back({"match_parts", "Match parts",
                 "Cross-view part correspondence: candidate parts with evidence; lists ambiguities and alternatives.",
                 object({}), false, false, true, match_parts});
    t.push_back({"reconstruct", "Reconstruct",
                 "Build the TRUE SOLID (or intentional VOXEL) model; runs missing earlier stages; creates an automatic "
                 "checkpoint of the previous state.",
                 object({{"mode", enumeration("Geometry mode.", {"true_solid", "voxel"})}}), false, false, true, reconstruct});
    t.push_back({"get_conflicts", "Get conflicts",
                 "Everything the drawings do not determine (AMBIGUOUS, blocks PASS) or that was assumed (INFERRED, info), "
                 "plus ambiguous candidates with Top footprints [x0, z0, x1, z1].",
                 object({}), true, false, true, get_conflicts});
    t.push_back({"get_inferred_geometry", "Get inferred geometry",
                 "Candidates kept by assumption (grid prior, continuation, solid fill) or kept provisionally (ambiguous), "
                 "recess findings, and surfaces textured by inference.",
                 object({}), true, false, true, get_inferred_geometry});
    t.push_back({"set_part_depth", "Set part depth",
                 "Depth constraint for a region of one view: material may exist only between depth_from and depth_to, "
                 "measured from that view's camera side of the bounding box. Use it to carve a recess/cavity (e.g. a "
                 "possible_recess via conflict_index) or flush=true to declare the region flush. Only apply depths the "
                 "user or the drawings determine.",
                 object({{"view", role_enum("View whose image coordinates the region uses.")},
                         {"rect", array(number("", -1e6, 1e6), "[x0, y0, x1, y1] in aligned view pixels.", 4, 4)},
                         {"polygon", array(array(number("", -1e6, 1e6), "[x, y]", 2, 2), "One ring of [x, y] points.")},
                         {"polygons", array(array(array(number("", -1e6, 1e6), "[x, y]", 2, 2), "ring"),
                                            "Rings (outer + holes).")},
                         {"conflict_index", integer("Index of a conflict from get_conflicts whose region to use.", 0, 1 << 20)},
                         {"depth_from", number("Start of the allowed material depth.", -1e6, 1e6)},
                         {"depth_to", number("End of the allowed material depth (default: unbounded).", -1e6, 1e9)},
                         {"flush", boolean("Declare the region flush (decides it without carving).")},
                         {"label", str("Short label.")}}),
                 false, false, false, set_part_depth});
    t.push_back({"edit_part", "Edit parts",
                 "Keep or remove candidate parts by id (from get_conflicts / inspect parts). Decisions the drawings "
                 "cannot honour are refused and reported, never silently applied.",
                 object({{"keep", array(integer("Candidate id.", 0, 1 << 30), "Candidate ids to keep.")},
                         {"remove", array(integer("Candidate id.", 0, 1 << 30), "Candidate ids to remove.")},
                         {"decisions", array(object({{"candidate", integer("Candidate id.", 0, 1 << 30)},
                                                     {"keep", boolean("Keep (true) or remove (false).")}},
                                                    {"candidate", "keep"}),
                                             "Explicit decisions.")}}),
                 false, false, true, edit_part});
    t.push_back({"cleanup_mesh", "Clean up mesh",
                 "Re-run mesh cleanup (duplicate/zero-area/internal faces, winding, fragments smaller than "
                 "min_fragment_volume voxels).",
                 object({{"min_fragment_volume", number("Shells smaller than this (voxels) are removed.", 0, 1e9)}}),
                 false, false, true, cleanup_mesh});
    t.push_back({"project_textures", "Project textures",
                 "Pixel-exact texture projection from the source views; unseen faces are INFERRED (opposite projection, "
                 "edge extend or flat palette), never repainted.",
                 object({{"allow_opposite_projection", boolean("Allow Opposite Projection for unseen faces.")},
                         {"visibility_fraction", number("Share of a face a view must show to texture it.", 0, 1)},
                         {"visibility_samples", integer("Samples per face for material checks.", 1, 1024)}}),
                 false, false, true, project_textures});
    t.push_back({"assign_texture", "Assign texture",
                 "Manually texture triangles from a supplied view (manual) or with the part's palette swatch "
                 "(flat_palette). Bound to the current mesh.",
                 object({{"triangles", array(integer("Triangle index.", 0, 1 << 30), "Triangle indices.")},
                         {"view", role_enum("Source view.")},
                         {"mode", enumeration("Texturing mode.", {"manual", "flat_palette"})}},
                        {"triangles", "view"}),
                 false, false, false, assign_texture});
    t.push_back({"validate", "Validate",
                 "Run every check (re-render IoU per view and at 90/180/270 degrees, thickness, turntable, watertight, "
                 "manifold, winding, GLB round trip) and the quality gate. Status PASS / NEEDS_REVIEW / FAIL with reasons.",
                 object({{"profile", profile}, {"glb_roundtrip", boolean("Include the GLB round trip (default true).")}}),
                 false, false, true, validate});
    t.push_back({"render_view", "Render view",
                 "Render the textured model as PNG images to inspect it visually. Source-view cameras (views, "
                 "pixels_per_unit, lit, compare = source vs render diff) or any orbit angle (yaw, elevation, size).",
                 object({{"views", array(role_enum("View role."), "Source-view cameras (default: all supplied).")},
                         {"pixels_per_unit", integer("Zoom for view cameras.", 1, 64)},
                         {"lit", boolean("Simple lighting (default false for views, true for orbit).")},
                         {"compare", boolean("Also return '<view>_compare' (grey match, red extra, blue missing, yellow "
                                             "colour differs).")},
                         {"yaw", number("Orbit yaw in degrees (0 = front, 90 = right side).", -3600, 3600)},
                         {"elevation", number("Orbit elevation in degrees.", -90, 90)},
                         {"size", integer("Orbit image size in pixels.", 16, 4096)},
                         {"output_dir", str("Also write the PNGs into this directory.")},
                         {"style", enumeration("validation (default): the exact check renderer. beauty: presentation "
                                               "render (perspective, studio lights, soft shadows) for looking at the "
                                               "model; never used by any check.",
                                               {"validation", "beauty"})}}),
                 true, false, true, render_view});
    t.push_back({"render_turntable", "Render turntable",
                 "360-degree turntable: contact sheet inline (frames inline with include_frames); optional output_dir.",
                 object({{"frames", integer("Number of frames.", 1, 360)},
                         {"size", integer("Frame size in pixels.", 16, 1024)},
                         {"elevation", number("Camera elevation in degrees.", -90, 90)},
                         {"include_frames", boolean("Return every frame inline too.")},
                         {"output_dir", str("Also write frames + contact sheet into this directory.")},
                         {"style", enumeration("validation (default) or beauty (presentation render, never checked).",
                                               {"validation", "beauty"})}}),
                 true, false, true, render_turntable});
    t.push_back({"undo", "Undo", "Undo the last document change.", object({}), false, true, false, undo});
    t.push_back({"redo", "Redo", "Redo the last undone change.", object({}), false, false, false, redo});
    t.push_back({"checkpoint", "Create checkpoint", "Store the current state under a name (saved with the project).",
                 object({{"name", str("Checkpoint name.")}}, {"name"}), false, false, true, checkpoint});
    t.push_back({"restore_checkpoint", "Restore checkpoint", "Restore a named checkpoint (undoable).",
                 object({{"name", str("Checkpoint name.")}}, {"name"}), false, true, false, restore_checkpoint});
    t.push_back({"export_glb", "Export GLB",
                 "Validate and write the GLB (scale from the project). A NEEDS_REVIEW or FAIL model is refused unless "
                 "accept_status names that status explicitly — only do that when the user accepts it.",
                 object({{"path", path_arg},
                         {"accept_status", enumeration("Explicitly accept exporting a model with this status.",
                                                       {"PASS", "NEEDS_REVIEW", "FAIL"})}},
                        {"path"}),
                 false, false, true, export_glb});
    t.push_back({"export_model", "Export model",
                 "Validate and write the model as GLB, glTF (.gltf + .bin + .png) or OBJ (.obj + .mtl + .png), for an "
                 "engine preset: generic (GLB, metres, front +Z), godot (GLB, metres, +Z), unity (GLB, metres, +Z) or "
                 "roblox (OBJ, studs, front -Z). units / front / unlit override the preset; scale always comes from the "
                 "project. The format comes from `format`, else the path's extension, else the preset. Every file is "
                 "reloaded and re-rendered before it is written. Same accept_status rule as export_glb.",
                 object({{"path", path_arg},
                         {"format", enumeration("File format.", {"glb", "gltf", "obj"})},
                         {"preset", enumeration("Engine preset.", {"generic", "godot", "unity", "roblox"})},
                         {"units", enumeration("Unit of the exported coordinates.", {"metres", "centimetres", "studs", "pixels"})},
                         {"front", enumeration("Direction the drawing's Front faces in the file.", {"+z", "-z"})},
                         {"unlit", boolean("glTF: KHR_materials_unlit (exact pixel colours, no shading).")},
                         {"accept_status", enumeration("Explicitly accept exporting a model with this status.",
                                                       {"PASS", "NEEDS_REVIEW", "FAIL"})}},
                        {"path"}),
                 false, false, true, export_model});
    t.push_back({"inspect", "Inspect",
                 "Read-only JSON about the project (same as the pixel3d:// resources): project, views, parts, mesh, "
                 "conflicts, diagnostics or validation.",
                 object({{"what", enumeration("What to inspect.", {"project", "views", "parts", "mesh", "conflicts",
                                                                    "diagnostics", "validation", "edits", "live",
                                                                    "lowpoly"})}},
                        {"what"}),
                 true, false, true, inspect});
    append_edit_tools(t);
    append_lowpoly_tools(t);
    return t;
  }();
  return tools;
}

const ToolDef* find_tool(const std::string& name) {
  for (const ToolDef& t : tool_catalog())
    if (t.name == name) return &t;
  return nullptr;
}

}  // namespace pixel3d::mcp
