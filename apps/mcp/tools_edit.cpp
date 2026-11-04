// M4 editing tools: each decodes its arguments and makes exactly one Session
// call — the same call the desktop application makes for the same action.
#include <cmath>

#include "mcp/tools_detail.hpp"
#include "pixel3d/engine/inspect.hpp"

namespace pixel3d::mcp {

using nlohmann::json;
using namespace schema;

namespace {

ToolOutput out(OpResult r) { return ToolOutput{std::move(r), {}}; }

int need_int(const Args& a, const char* key, int lo, int hi) {
  const auto v = a.opt_int(key, lo, hi);
  if (!v) throw ArgError(key, "required");
  return *v;
}

int parse_axis(const Args& a, const char* key) {
  const json& v = a.raw(key);
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    if (s == "x" || s == "X") return 0;
    if (s == "y" || s == "Y") return 1;
    if (s == "z" || s == "Z") return 2;
  } else if (v.is_number_integer() && v.get<int>() >= 0 && v.get<int>() <= 2) {
    return v.get<int>();
  }
  throw ArgError(key, "must be x, y or z");
}

vec2d point(const std::string& field, const json& p) {
  if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
    throw ArgError(field, "points are [x, y] number pairs");
  const double x = p[0].get<double>(), y = p[1].get<double>();
  if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e6 || std::abs(y) > 1e6)
    throw ArgError(field, "coordinates must be finite and within ±1e6");
  return {x, y};
}

std::array<double, 3> vec3(const Args& a, const char* key, std::array<double, 3> def) {
  if (!a.has(key)) return def;
  const json& v = a.raw(key);
  if (!v.is_array() || v.size() != 3) throw ArgError(key, "must be [x, y, z]");
  std::array<double, 3> r{};
  for (int i = 0; i < 3; ++i) {
    if (!v[i].is_number()) throw ArgError(key, "must be [x, y, z] numbers");
    r[i] = v[i].get<double>();
    if (!std::isfinite(r[i]) || std::abs(r[i]) > 1e6) throw ArgError(key, "values must be finite and within ±1e6");
  }
  return r;
}

// A region of a view: rect [x0, y0, x1, y1], polygon [[x, y], …] or polygons [ring, …].
Polygons2D region(const Args& a) {
  int given = 0;
  for (const char* k : {"rect", "polygon", "polygons"}) given += a.has(k) ? 1 : 0;
  if (given != 1) throw ArgError("region", "give exactly one of rect, polygon or polygons");
  auto ring = [&](const std::string& f, const json& r) {
    if (!r.is_array() || r.size() < 3 || r.size() > 100000) throw ArgError(f, "a polygon needs 3 to 100000 points");
    Ring out;
    for (const auto& p : r) out.push_back(point(f, p));
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
  const json& ps = a.raw("polygons");
  if (!ps.is_array() || ps.empty() || ps.size() > 1000) throw ArgError("polygons", "must be a list of 1-1000 rings");
  Polygons2D o;
  for (const auto& r : ps) o.push_back(ring("polygons", r));
  return o;
}

std::vector<vec2d> stroke(const Args& a) {
  std::vector<vec2d> s;
  if (!a.has("stroke")) return s;
  const json& v = a.raw("stroke");
  if (!v.is_array() || v.empty() || v.size() > 100000) throw ArgError("stroke", "must be 1-100000 [x, y] points");
  for (const auto& p : v) s.push_back(point("stroke", p));
  return s;
}

BrushParams brush(const Args& a) {
  BrushParams b;
  b.radius = a.opt_num("radius", 0, 256).value_or(1.5);
  b.strength = a.opt_num("strength", 0, 1024).value_or(1.0);
  b.hardness = a.opt_num("hardness", 0, 1).value_or(0.5);
  b.quantum = a.opt_num("quantum", 0, 64).value_or(1.0);
  if (auto m = a.opt_str("mode")) {
    if (*m == "raise") b.raise = true;
    else if (*m == "lower") b.raise = false;
    else throw ArgError("mode", "must be 'raise' (toward the camera) or 'lower' (into the surface)");
  }
  return b;
}

const json kRegionProps = {
    {"rect", array(number("", -1e6, 1e6), "[x0, y0, x1, y1] in aligned view pixels.", 4, 4)},
    {"polygon", array(array(number("", -1e6, 1e6), "[x, y]", 2, 2), "One ring of [x, y] points.")},
    {"polygons", array(array(array(number("", -1e6, 1e6), "[x, y]", 2, 2), "ring"), "Rings (outer + holes).")}};

json with(json props, const json& extra) {
  for (const auto& [k, v] : extra.items()) props[k] = v;
  return props;
}

// ---- handlers -------------------------------------------------------------------

ToolOutput paint_depth(ToolContext& c, const Args& a) {
  a.allow_only({"view", "samples", "stroke", "radius", "strength", "hardness", "mode", "quantum", "part"});
  DepthPaintRequest r;
  r.view = a.role("view");
  r.part = a.opt_int("part", -1, 1 << 30).value_or(-1);
  if (a.has("samples")) {
    const json& s = a.raw("samples");
    if (!s.is_array() || s.empty() || s.size() > (1u << 20)) throw ArgError("samples", "must be 1-1048576 [u, v, offset] triples");
    for (const auto& t : s) {
      if (!t.is_array() || t.size() != 3 || !t[0].is_number_integer() || !t[1].is_number_integer() || !t[2].is_number())
        throw ArgError("samples", "each sample is [u, v, offset]");
      r.samples.push_back(DepthSample{t[0].get<int>(), t[1].get<int>(), t[2].get<double>()});
    }
  }
  r.stroke = stroke(a);
  if (r.samples.empty() == r.stroke.empty()) throw ArgError("stroke", "give either stroke (brush) or samples");
  r.brush = brush(a);
  return out(c.session.paint_depth(r));
}

ToolOutput paint_part_mask(ToolContext& c, const Args& a) {
  a.allow_only({"part", "view", "pixels", "stroke", "radius", "erase", "rect"});
  MaskPaintRequest r;
  r.part = need_int(a, "part", 0, 1 << 30);
  r.view = a.role("view");
  r.erase = a.opt_bool("erase").value_or(false);
  r.brush.radius = a.opt_num("radius", 0, 256).value_or(0.5);
  r.stroke = stroke(a);
  if (a.has("pixels")) {
    const json& p = a.raw("pixels");
    if (!p.is_array() || p.size() > (1u << 22)) throw ArgError("pixels", "must be a list of [u, v]");
    for (const auto& q : p) {
      if (!q.is_array() || q.size() != 2 || !q[0].is_number_integer() || !q[1].is_number_integer())
        throw ArgError("pixels", "each pixel is [u, v] integers");
      r.pixels.emplace_back(q[0].get<int>(), q[1].get<int>());
    }
  }
  if (a.has("rect")) {
    const json& b = a.raw("rect");
    if (!b.is_array() || b.size() != 4) throw ArgError("rect", "must be [x0, y0, x1, y1] (pixels, end exclusive)");
    const int x0 = b[0].get<int>(), y0 = b[1].get<int>(), x1 = b[2].get<int>(), y1 = b[3].get<int>();
    if (x1 <= x0 || y1 <= y0 || static_cast<int64_t>(x1 - x0) * (y1 - y0) > (1 << 22)) throw ArgError("rect", "invalid rect");
    for (int v = y0; v < y1; ++v)
      for (int u = x0; u < x1; ++u) r.pixels.emplace_back(u, v);
  }
  if (r.pixels.empty() && r.stroke.empty()) throw ArgError("pixels", "give pixels, rect or a stroke");
  return out(c.session.paint_part_mask(r));
}

ToolOutput create_part(ToolContext& c, const Args& a) {
  a.allow_only({"name", "define"});
  return out(c.session.create_part(a.str("name"), a.opt_bool("define").value_or(false)));
}

ToolOutput suggest_layer_parts(ToolContext& c, const Args& a) {
  a.allow_only({"apply", "replace", "accept"});
  LayerPartsRequest r;
  r.apply = a.opt_bool("apply").value_or(false);
  r.replace = a.opt_bool("replace").value_or(false);
  if (a.has("accept")) {
    const nlohmann::json& list = a.raw("accept");
    if (!list.is_array() || list.size() > 4096) throw ArgError("accept", "must be a list of layer names or paths");
    for (const auto& v : list) {
      if (!v.is_string() || v.get<std::string>().empty()) throw ArgError("accept", "each entry is a layer name or path");
      r.accept.push_back(v.get<std::string>());
    }
  }
  return out(c.session.suggest_layer_parts(r));
}

ToolOutput rename_part(ToolContext& c, const Args& a) {
  a.allow_only({"part", "name"});
  if (!a.has("part")) throw ArgError("part", "required");
  return out(c.session.rename_part(need_int(a, "part", -1, 1 << 30), a.str("name")));
}

ToolOutput delete_part(ToolContext& c, const Args& a) {
  a.allow_only({"part"});
  if (!a.has("part")) throw ArgError("part", "required");
  return out(c.session.delete_part(need_int(a, "part", -1, 1 << 30)));
}

ToolOutput split_part(ToolContext& c, const Args& a) {
  a.allow_only({"part", "by", "axis", "at"});
  if (!a.has("part")) throw ArgError("part", "required");
  const int part = need_int(a, "part", -1, 1 << 30);
  const std::string by = a.opt_str("by").value_or(a.has("axis") ? "plane" : "shells");
  if (by == "shells") return out(c.session.split_part_shells(part));
  if (by != "plane") throw ArgError("by", "must be 'plane' (axis + at) or 'shells' (disconnected pieces)");
  return out(c.session.split_part(part, parse_axis(a, "axis"), a.num("at", -1e6, 1e6)));
}

ToolOutput merge_parts(ToolContext& c, const Args& a) {
  a.allow_only({"parts"});
  const std::vector<int> parts = a.int_list("parts", -1, 1 << 30, 10000);
  if (parts.size() < 2) throw ArgError("parts", "give at least two part ids");
  return out(c.session.merge_parts(parts));
}

ToolOutput sync_part_masks(ToolContext& c, const Args& a) {
  a.allow_only({"part"});
  if (!a.has("part")) throw ArgError("part", "required");
  return out(c.session.sync_part_masks(need_int(a, "part", 0, 1 << 30)));
}

ToolOutput push_pull(ToolContext& c, const Args& a) {
  a.allow_only({"face", "view", "u", "v", "distance", "snap", "respect_bounds"});
  PushPullRequest r;
  if (a.has("face")) {
    r.face = need_int(a, "face", 0, 1 << 30);
  } else {
    r.view = a.role("view");
    r.u = need_int(a, "u", -1, 1 << 20);
    r.v = need_int(a, "v", -1, 1 << 20);
  }
  r.distance = a.num("distance", -1e4, 1e4);
  r.snap = a.opt_bool("snap").value_or(true);
  r.respect_bounds = a.opt_bool("respect_bounds").value_or(true);
  return out(c.session.push_pull(r));
}

ToolOutput add_volume(ToolContext& c, const Args& a) {
  a.allow_only({"view", "rect", "polygon", "polygons", "depth_from", "depth_to", "part"});
  return out(c.session.add_volume(a.role("view"), region(a), a.num("depth_from", -1e6, 1e6), a.num("depth_to", -1e6, 1e6),
                                  a.opt_int("part", -1, 1 << 30).value_or(-1)));
}

ToolOutput erase_volume(ToolContext& c, const Args& a) {
  a.allow_only({"view", "rect", "polygon", "polygons", "depth_from", "depth_to", "part"});
  return out(c.session.erase_volume(a.role("view"), region(a), a.num("depth_from", -1e6, 1e6),
                                    a.num("depth_to", -1e6, 1e6), a.opt_int("part", -1, 1 << 30).value_or(-1)));
}

ToolOutput extrude(ToolContext& c, const Args& a) {
  a.allow_only({"view", "rect", "polygon", "polygons", "distance", "part"});
  return out(c.session.extrude(a.role("view"), region(a), a.num("distance", -1e4, 1e4),
                               a.opt_int("part", -1, 1 << 30).value_or(-1)));
}

ToolOutput cut(ToolContext& c, const Args& a) {
  a.allow_only({"part", "axis", "at", "keep"});
  const std::string keep = a.str("keep");
  if (keep != "negative" && keep != "positive") throw ArgError("keep", "must be 'negative' or 'positive'");
  return out(c.session.cut(a.opt_int("part", -1, 1 << 30).value_or(-1), parse_axis(a, "axis"), a.num("at", -1e6, 1e6),
                           keep == "positive" ? 1 : -1));
}

ToolOutput mirror(ToolContext& c, const Args& a) {
  a.allow_only({"part", "axis", "at", "copy"});
  return out(c.session.mirror(a.opt_int("part", -1, 1 << 30).value_or(-1), parse_axis(a, "axis"), a.opt_num("at", -1e6, 1e6),
                              a.opt_bool("copy").value_or(false)));
}

ToolOutput transform_part(ToolContext& c, const Args& a) {
  a.allow_only({"part", "translate", "rotate_axis", "rotate_deg", "scale", "pivot", "snap"});
  TransformRequest r;
  if (!a.has("part")) throw ArgError("part", "required");
  r.part = need_int(a, "part", -1, 1 << 30);
  const auto t = vec3(a, "translate", {0, 0, 0});
  r.translate = {t[0], t[1], t[2]};
  r.rotate_axis = a.has("rotate_axis") ? parse_axis(a, "rotate_axis") : 1;
  r.rotate_deg = a.opt_num("rotate_deg", -36000, 36000).value_or(0);
  r.scale = a.opt_num("scale", 1e-3, 1e3).value_or(1);
  if (a.has("pivot")) {
    const auto p = vec3(a, "pivot", {0, 0, 0});
    r.pivot = vec3d{p[0], p[1], p[2]};
  }
  r.snap = a.opt_bool("snap").value_or(true);
  return out(c.session.transform_part(r));
}

ToolOutput duplicate_part(ToolContext& c, const Args& a) {
  a.allow_only({"part", "offset", "snap"});
  TransformRequest r;
  if (!a.has("part")) throw ArgError("part", "required");
  r.part = need_int(a, "part", -1, 1 << 30);
  const auto t = vec3(a, "offset", {0, 0, 0});
  r.translate = {t[0], t[1], t[2]};
  r.snap = a.opt_bool("snap").value_or(true);
  r.copy = true;
  return out(c.session.transform_part(r));
}

SelectMode parse_mode(const Args& a) {
  const auto s = a.opt_str("mode").value_or("part");
  const auto m = select_mode_from_string(s);
  if (!m) throw ArgError("mode", "must be object, part or face");
  return *m;
}

ToolOutput set_selection(ToolContext& c, const Args& a) {
  a.allow_only({"mode", "view", "u", "v", "origin", "direction", "parts", "clear"});
  if (a.opt_bool("clear").value_or(false)) return out(c.session.clear_selection());
  const SelectMode mode = parse_mode(a);
  if (a.has("parts")) return out(c.session.select_parts(a.int_list("parts", -1, 1 << 30, 10000), mode));
  if (a.has("origin")) {
    const auto o = vec3(a, "origin", {0, 0, 0}), d = vec3(a, "direction", {0, 0, 0});
    return out(c.session.select_at_ray({o[0], o[1], o[2]}, {d[0], d[1], d[2]}, mode));
  }
  if (a.has("view"))
    return out(c.session.select_at_pixel(a.role("view"), need_int(a, "u", -1, 1 << 20), need_int(a, "v", -1, 1 << 20), mode));
  throw ArgError("view", "give view + u + v, origin + direction, parts, or clear");
}

ToolOutput get_selection(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.get_selection());
}

ToolOutput preview_edit(ToolContext& c, const Args& a) {
  a.allow_only({"edit"});
  auto op = edit_from_json(a.raw("edit"));
  if (!op) throw ArgError("edit", op.error().text);
  return out(c.session.preview_edit(*op));
}

ToolOutput new_single_view_project(ToolContext& c, const Args& a) {
  a.allow_only({"path", "role", "preset", "depth", "name", "pixels_per_meter", "style"});
  ProjectSettings ps;
  if (auto v = a.opt_str("style")) ps.style.preset = parse_style("style", *v);
  if (auto n = a.opt_str("name")) ps.name = *n;
  if (auto p = a.opt_num("pixels_per_meter", 1e-3, 1e6)) ps.pixels_per_meter = *p;
  const ViewRole role = a.opt_role("role").value_or(ViewRole::Front);
  const auto path = c.paths.resolve("path", a.str("path"));
  SingleViewSettings sv;
  sv.enabled = true;
  sv.role = role;
  sv.preset = a.opt_str("preset").value_or("medium");
  if (!valid_depth_preset(sv.preset) || sv.preset == "custom") throw ArgError("preset", "must be flat, thin, medium or thick");
  if (auto d = a.opt_num("depth", 0.25, 4096)) sv.preset = "custom", sv.depth = *d;
  sv.user_depth = a.has("preset") || a.has("depth");
  OpResult r = c.session.new_project(ps);
  if (!r.success) return out(r);
  r = c.session.import_views({ViewImport{role, path, std::nullopt}});
  if (!r.success) return out(r);
  return out(c.session.set_single_view(sv));
}

ToolOutput set_single_view_depth(ToolContext& c, const Args& a) {
  a.allow_only({"preset", "depth"});
  const auto depth = a.opt_num("depth", 0.25, 4096);
  const auto preset = a.opt_str("preset");
  if (!depth && !preset) throw ArgError("preset", "give a preset (flat, thin, medium, thick) or a depth");
  return out(c.session.set_single_view_depth(preset.value_or("custom"), depth));
}

ToolOutput reload_views(ToolContext& c, const Args& a) {
  a.allow_only({});
  return out(c.session.reload_views());
}

}  // namespace

void append_edit_tools(std::vector<ToolDef>& t) {
  const json part = integer("Part id (see inspect parts / get_selection).", -1, 1 << 30);
  const json view = role_enum("View whose aligned pixels the region uses.");
  const json axis = enumeration("Axis.", {"x", "y", "z"});
  const json brush_props = {{"radius", number("Brush radius in pixels (< 1 = single pixels).", 0, 256)},
                            {"hardness", number("0 soft … 1 hard edge.", 0, 1)},
                            {"stroke", array(array(number("", -1e6, 1e6), "[x, y]", 2, 2), "Stroke points in view pixels.")}};
  t.push_back({"paint_depth", "Paint depth",
               "Depth Brush on a view: raise the surface toward the camera or lower it into the model under each brushed "
               "pixel. Give a stroke (+ radius, strength, hardness, mode) or explicit samples [[u, v, offset]]. One undo step; "
               "the result reports projection changes.",
               object(with(brush_props, {{"view", view},
                                         {"strength", number("Offset in pixels at full weight.", 0, 1024)},
                                         {"mode", enumeration("raise = toward the camera, lower = into the surface.", {"raise", "lower"})},
                                         {"quantum", number("Depth snapping step (0 = none, default 1 px).", 0, 64)},
                                         {"samples", array(array(number("", -1e6, 1e6), "[u, v, offset]", 3, 3), "Explicit samples.")},
                                         {"part", part}}),
                      {"view"}),
               false, false, false, paint_depth});
  t.push_back({"create_part", "Create part",
               "New user part (returns its id). Paint its pixels with paint_part_mask. define=true: its masks define its "
               "geometry and claim the automatic candidates inside them (resolves ambiguous parts); false: it labels "
               "existing geometry (split it off).",
               object({{"name", str("Part name.")}, {"define", boolean("Masks define the geometry (default false).")}}, {"name"}),
               false, false, false, create_part});
  t.push_back({"paint_part_mask", "Paint part mask",
               "Assign drawn pixels of a view to a user part (or erase them). Pixels, a rect, or a brush stroke. The artwork is "
               "never changed; transparent pixels are skipped.",
               object(with(brush_props, {{"part", part},
                                         {"view", view},
                                         {"pixels", array(array(integer("", -1, 1 << 20), "[u, v]", 2, 2), "Pixels.")},
                                         {"rect", array(integer("", -1, 1 << 20), "[x0, y0, x1, y1], end exclusive.", 4, 4)},
                                         {"erase", boolean("Remove the pixels from the part.")}}),
                      {"part", "view"}),
               false, false, false, paint_part_mask});
  t.push_back({"suggest_layer_parts", "Suggest parts from layers",
               "Views imported from layered files (.aseprite/.ase first frame, .psd composite): every visible raster "
               "layer is a SUGGESTED part mask named after the layer (in Low-Poly Studio with a type guessed from the "
               "name). Proposals only unless apply=true (one undo step; accept = the layers the user chose, default all "
               "suggested; replace=true replaces existing user parts). Hidden layers are listed, never created. Show the "
               "proposal to the user before applying.",
               object({{"apply", boolean("Create the accepted layers as user parts (default false).")},
                       {"replace", boolean("Replace existing user parts (default false).")},
                       {"accept", array(str("Layer name or group path."), "Layers to create (default: every suggested one).")}}),
               false, false, false, suggest_layer_parts});
  t.push_back({"sync_part_masks", "Sync part masks",
               "Project a user part's geometry into the views where it has no mask yet, so its masks agree everywhere.",
               object({{"part", part}}, {"part"}), false, false, true, sync_part_masks});
  t.push_back({"rename_part", "Rename part", "Give any part a display name.",
               object({{"part", part}, {"name", str("New name.")}}, {"part", "name"}), false, false, true, rename_part});
  t.push_back({"delete_part", "Delete part", "Remove a part's geometry (undoable).", object({{"part", part}}, {"part"}), false,
               true, false, delete_part});
  t.push_back({"split_part", "Split part",
               "Split a part by an axis plane (by=plane, axis, at) into two parts, or into its disconnected pieces (by=shells).",
               object({{"part", part}, {"by", enumeration("How to split.", {"plane", "shells"})}, {"axis", axis},
                       {"at", number("Plane coordinate in source pixels.", -1e6, 1e6)}},
                      {"part"}),
               false, false, false, split_part});
  t.push_back({"merge_parts", "Merge parts", "Merge parts into the first id.",
               object({{"parts", array(part, "Part ids (first is kept).", 2)}}, {"parts"}), false, false, false, merge_parts});
  t.push_back({"push_pull", "Push/Pull",
               "Move a planar face along its normal: distance > 0 pulls material out, < 0 pushes in. Face by id (get_selection) "
               "or by the pixel a view shows. respect_bounds (default) stops at the drawings' box so projections stay intact.",
               object({{"face", integer("Face id from get_selection (face mode).", 0, 1 << 30)},
                       {"view", role_enum("Pick the face shown at this view pixel …")},
                       {"u", integer("… pixel column.", -1, 1 << 20)},
                       {"v", integer("… pixel row.", -1, 1 << 20)},
                       {"distance", number("Source pixels.", -1e4, 1e4)},
                       {"snap", boolean("Whole pixels (default true).")},
                       {"respect_bounds", boolean("Clamp to the drawings' box (default true).")}},
                      {"distance"}),
               false, false, false, push_pull});
  t.push_back({"add_volume", "Add volume",
               "Add the region of a view swept between two depths (from that view's camera side) to a part, or as a new part.",
               object(with(kRegionProps, {{"view", view},
                                          {"depth_from", number("Start depth.", -1e6, 1e6)},
                                          {"depth_to", number("End depth.", -1e6, 1e6)},
                                          {"part", integer("Target part (omit = new part).", -1, 1 << 30)}}),
                      {"view", "depth_from", "depth_to"}),
               false, false, false, add_volume});
  t.push_back({"erase_volume", "Erase volume", "Carve the region of a view between two depths from a part (or every part).",
               object(with(kRegionProps, {{"view", view},
                                          {"depth_from", number("Start depth.", -1e6, 1e6)},
                                          {"depth_to", number("End depth.", -1e6, 1e6)},
                                          {"part", integer("Only this part (omit = all).", -1, 1 << 30)}}),
                      {"view", "depth_from", "depth_to"}),
               false, true, false, erase_volume});
  t.push_back({"extrude", "Extrude",
               "Move a region of a view out of (distance > 0) or into (< 0) the surface the view shows under it.",
               object(with(kRegionProps, {{"view", view}, {"distance", number("Source pixels.", -1e4, 1e4)}, {"part", part}}),
                      {"view", "distance"}),
               false, false, false, extrude});
  t.push_back({"cut", "Cut", "Cut away one side of an axis plane from a part (or the whole model).",
               object({{"part", integer("Part (omit = all).", -1, 1 << 30)}, {"axis", axis},
                       {"at", number("Plane coordinate.", -1e6, 1e6)},
                       {"keep", enumeration("Side to keep.", {"negative", "positive"})}},
                      {"axis", "at", "keep"}),
               false, true, false, cut});
  t.push_back({"mirror", "Mirror",
               "Mirror a part (or everything) across an axis plane (default: the box centre); copy=true adds a mirrored copy.",
               object({{"part", integer("Part (omit = all).", -1, 1 << 30)}, {"axis", axis},
                       {"at", number("Plane coordinate (default: centre).", -1e6, 1e6)},
                       {"copy", boolean("Add a mirrored copy as a new part.")}},
                      {"axis"}),
               false, false, false, mirror});
  t.push_back({"transform_part", "Transform part",
               "Scale, rotate and translate a part about its centre (or pivot). Translation snaps to pixels by default.",
               object({{"part", part},
                       {"translate", array(number("", -1e6, 1e6), "[x, y, z]", 3, 3)},
                       {"rotate_axis", axis},
                       {"rotate_deg", number("Degrees.", -36000, 36000)},
                       {"scale", number("Uniform scale.", 1e-3, 1e3)},
                       {"pivot", array(number("", -1e6, 1e6), "[x, y, z]", 3, 3)},
                       {"snap", boolean("Whole-pixel translation (default true).")}},
                      {"part"}),
               false, false, false, transform_part});
  t.push_back({"duplicate_part", "Duplicate part", "Copy a part as a new part, moved by offset.",
               object({{"part", part}, {"offset", array(number("", -1e6, 1e6), "[x, y, z]", 3, 3)},
                       {"snap", boolean("Whole pixels (default true).")}},
                      {"part"}),
               false, false, false, duplicate_part});
  t.push_back({"set_selection", "Set selection",
               "Select by a view pixel (view, u, v), by a 3D ray (origin, direction), by part ids, or clear. Mode object | "
               "part | face. Returns the selection with its projection box in every view.",
               object({{"mode", enumeration("Selection mode.", {"object", "part", "face"})},
                       {"view", role_enum("View of the pixel.")},
                       {"u", integer("Pixel column.", -1, 1 << 20)},
                       {"v", integer("Pixel row.", -1, 1 << 20)},
                       {"origin", array(number("", -1e6, 1e6), "[x, y, z]", 3, 3)},
                       {"direction", array(number("", -1e6, 1e6), "[x, y, z]", 3, 3)},
                       {"parts", array(part, "Part ids.")},
                       {"clear", boolean("Clear the selection.")}}),
               true, false, true, set_selection});
  t.push_back({"get_selection", "Get selection",
               "The current selection: parts, face (normal, area), the picked point and each view's projection box.",
               object({}), true, false, true, get_selection});
  t.push_back({"preview_edit", "Preview edit",
               "Result of an edit without committing it: triangles, watertightness and each view's mismatch. The edit uses the "
               "project edit format (see inspect what=edits).",
               object({{"edit", json{{"type", "object"}, {"description", "Edit object, e.g. {op: 'cut', part, axis, at, keep}."}}}},
                      {"edit"}),
               true, false, true, preview_edit});
  t.push_back({"new_single_view_project", "New single-view project",
               "Start a project from ONE drawing. Depth is not drawn: presets flat / thin / medium / thick or a number. Hidden "
               "geometry is reported as INFERRED; the default depth keeps the status NEEDS_REVIEW until a depth is chosen.",
               object({{"path", str("PNG path.")},
                       {"role", role_enum("Which view the drawing is (default front).")},
                       {"preset", enumeration("Depth preset.", {"flat", "thin", "medium", "thick"})},
                       {"depth", number("Depth in source pixels (overrides preset).", 0.25, 4096)},
                       {"name", str("Project name.")},
                       {"pixels_per_meter", number("Export scale.", 1e-3, 1e6)},
                       {"style", style_enum()}},
                      {"path"}),
               false, true, false, new_single_view_project});
  t.push_back({"set_single_view_depth", "Set single-view depth",
               "Choose the depth of a single-view project (the user's decision makes the depth determined).",
               object({{"preset", enumeration("Preset.", {"flat", "thin", "medium", "thick"})},
                       {"depth", number("Depth in source pixels.", 0.25, 4096)}}),
               false, false, true, set_single_view_depth});
  t.push_back({"reload_views", "Reload views",
               "Re-read every drawing from the file it was imported from (after re-exporting from Aseprite). Masks, depth "
               "constraints and edits are kept.",
               object({}), false, false, true, reload_views});
}

}  // namespace pixel3d::mcp
