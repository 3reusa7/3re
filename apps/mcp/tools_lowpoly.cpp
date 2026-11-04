// LOW-POLY STUDIO tools: one Session call each, no modelling logic here.
#include "mcp/tools_detail.hpp"

namespace pixel3d::mcp {
namespace {

using nlohmann::json;
using namespace schema;

ToolOutput out(OpResult r) { return ToolOutput{std::move(r), {}}; }

lowpoly::Style parse_lp_style(const char* field, const std::string& v) {
  const auto s = lowpoly::style_from_string(v);
  if (!s) throw ArgError(field, "must be flat, cel, outline or cel_outline");
  return *s;
}

// The low-poly settings shared by set_lowpoly_settings and lowpoly_build.
LowpolyUpdate update_args(const Args& a) {
  LowpolyUpdate u;
  u.enabled = a.opt_bool("enabled");
  u.tri_budget = a.opt_int("tri_budget", lowpoly::kMinBudget, lowpoly::kMaxBudget);
  u.joint_loops = a.opt_int("joint_loops", 1, 3);
  u.tones = a.opt_int("tones", 2, 3);
  if (auto v = a.opt_str("style")) u.style = parse_lp_style("style", *v);
  if (auto v = a.opt_str("template")) {
    const auto t = lowpoly::template_from_string(*v);
    if (!t) throw ArgError("template", "must be auto, cloaked_character, humanoid or generic_chain");
    u.templ = *t;
  }
  u.baked_cel = a.opt_bool("baked_cel");
  u.hull = a.opt_bool("hull");
  u.hull_width = a.opt_num("hull_width", 0.05, 8);
  u.accept_inferred_depth = a.opt_bool("accept_inferred_depth");
  u.depth = a.opt_num("depth", 0, 4096);
  return u;
}

const std::initializer_list<const char*> kUpdateKeys = {"enabled", "tri_budget", "joint_loops", "tones", "style", "template",
                                                        "baked_cel", "hull", "hull_width", "accept_inferred_depth", "depth"};

ToolOutput set_lowpoly_settings(ToolContext& c, const Args& a) {
  a.allow_only(kUpdateKeys);
  const LowpolyUpdate u = update_args(a);
  if (u.empty()) throw ArgError("arguments", "nothing to change");
  return out(c.session.set_lowpoly_settings(u));
}

ToolOutput set_part_type(ToolContext& c, const Args& a) {
  a.allow_only({"part", "type", "depth_ratio"});
  PartTypeUpdate u;
  u.part = a.opt_int("part", 0, 1 << 30).value_or(-1);
  if (u.part < 0) throw ArgError("part", "is required");
  if (auto v = a.opt_str("type")) {
    const auto t = lowpoly::part_type_from_string(*v);
    if (!t) throw ArgError("type", "must be head, torso, limb, accessory or cloth");
    u.type = *t;
  }
  u.depth_ratio = a.opt_num("depth_ratio", 0, 2);
  if (!u.type && !u.depth_ratio) throw ArgError("type", "give a type or a depth_ratio");
  return out(c.session.set_part_type(u));
}

ToolOutput lowpoly_suggest_parts(ToolContext& c, const Args& a) {
  a.allow_only({"apply", "replace"});
  LowpolySuggestRequest r;
  r.apply = a.opt_bool("apply").value_or(false);
  r.replace = a.opt_bool("replace").value_or(false);
  return out(c.session.lowpoly_suggest_parts(r));
}

ToolOutput lowpoly_build(ToolContext& c, const Args& a) {
  a.allow_only(kUpdateKeys);
  return out(c.session.lowpoly_build(update_args(a), c.job));
}

ToolOutput lowpoly_render(ToolContext& c, const Args& a) {
  a.allow_only({"yaws", "elevation", "size", "pose", "style", "wire", "contact_sheet", "output_dir"});
  LowpolyRenderRequest r;
  if (a.has("yaws")) {
    const json& list = a.raw("yaws");
    if (!list.is_array() || list.empty() || list.size() > 72) throw ArgError("yaws", "must be 1-72 angles in degrees");
    r.yaws.clear();
    for (const json& v : list) {
      if (!v.is_number()) throw ArgError("yaws", "must be numbers (degrees)");
      r.yaws.push_back(v.get<double>());
    }
  }
  r.elevation_deg = a.opt_num("elevation", -90, 90).value_or(10);
  r.size = a.opt_int("size", 16, 2048).value_or(384);
  r.pose = a.opt_bool("pose").value_or(false);
  if (auto v = a.opt_str("style")) r.style = parse_lp_style("style", *v);
  r.wire = a.opt_bool("wire").value_or(false);
  r.contact_sheet = a.opt_bool("contact_sheet").value_or(false);
  const auto dir = a.opt_str("output_dir");
  ToolOutput o;
  o.result = c.session.lowpoly_render(r, &o.images);
  if (o.result.success && dir) write_named_images(c.paths.resolve("output_dir", *dir), o.images, o.result);
  return o;
}

ToolOutput lowpoly_export(ToolContext& c, const Args& a) {
  a.allow_only({"path", "accept_status", "engine_files"});
  LowpolyExportRequest r;
  r.path = c.paths.resolve("path", a.str("path"));
  if (auto s = a.opt_str("accept_status")) {
    if (*s == "NEEDS_REVIEW") r.allow_needs_review = true;
    else if (*s != "PASS") throw ArgError("accept_status", "must be PASS or NEEDS_REVIEW (a FAIL model is never exported)");
  }
  r.engine_files = a.opt_bool("engine_files").value_or(false);
  return out(c.session.lowpoly_export(r));
}

json update_schema() {
  return {{"enabled", boolean("Project mode: true = Low-Poly Studio (views re-aligned as Front + optional Side/Top).")},
          {"tri_budget", integer("Hard cap on exported triangles (default 1000; 500 is the accepted minimum and warns).",
                                 lowpoly::kMinBudget, lowpoly::kMaxBudget)},
          {"joint_loops", integer("Edge loops per joint (default 2).", 1, 3)},
          {"tones", integer("Cel ramp tones (default 3).", 2, 3)},
          {"style", enumeration("Look: flat (unlit), cel, outline or cel_outline (default). The outline is engine-side "
                                "(shader), never triangles unless hull=true.",
                                {"flat", "cel", "outline", "cel_outline"})},
          {"template", enumeration("Body plan (default auto).", {"auto", "cloaked_character", "humanoid", "generic_chain"})},
          {"baked_cel", boolean("Bake the cel ramp into the texture (fixed standard light; for engines without shaders, "
                                "e.g. Roblox). Reported: lighting will not follow engine lights.")},
          {"hull", boolean("Inverted-hull outline shells in the file (count toward the budget; default false).")},
          {"hull_width", number("Inverted-hull width in source pixels.", 0.05, 8)},
          {"accept_inferred_depth", boolean("Front only: the USER accepts inferred depth for batch/export. The status "
                                            "stays NEEDS_REVIEW and the acceptance is written in the report. Only set it "
                                            "when the user asks.")},
          {"depth", number("Front only: total depth in source pixels (the user's decision); 0 = automatic by part type.",
                           0, 4096)}};
}

}  // namespace

void append_lowpoly_tools(std::vector<ToolDef>& t) {
  const json part_type = enumeration("Part type.", {"head", "torso", "limb", "accessory", "cloth"});
  t.push_back({"set_lowpoly_settings", "Set low-poly settings",
               "LOW-POLY STUDIO settings of the project (undoable): mode, triangle budget, joint loops, style, template, "
               "baked cel, hull, front-only depth and accept_inferred_depth. Building is lowpoly_build.",
               object(update_schema()), false, false, true, set_lowpoly_settings});
  t.push_back({"set_part_type", "Set part type",
               "Type of a user part for LOW-POLY STUDIO (head, torso, limb, accessory, cloth) and, front only, its "
               "minimum depth/width (depth_ratio; 0 = the type's default). Undoable.",
               object({{"part", integer("User part id (inspect parts).", 0, 1 << 30)},
                       {"type", part_type},
                       {"depth_ratio", number("Minimum depth/width of the part's cross-sections (0 = default).", 0, 2)}},
                      {"part"}),
               false, false, true, set_part_type});
  t.push_back({"lowpoly_suggest_parts", "Suggest low-poly parts",
               "Typed parts proposed from the Front drawing (colour regions split into cores and protrusions; Side masks "
               "from the same colours when a Side view exists). Proposals only unless apply=true (one undo step; "
               "replace=true replaces existing user parts). Show the proposal to the user before applying.",
               object({{"apply", boolean("Create the suggested parts (default false).")},
                       {"replace", boolean("Replace existing user parts (default false).")}}),
               false, false, false, lowpoly_suggest_parts});
  t.push_back({"lowpoly_build", "Build low-poly character",
               "Build the LOW-POLY STUDIO character from the typed parts: lofted part shells within the triangle budget "
               "(hard cap, never decimated), skeleton + skin weights, texture projected from the drawings (one view per "
               "texel, hidden sides INFERRED), skinned GLB written in memory and reloaded independently, and the "
               "low-poly gate (PASS / NEEDS_REVIEW / FAIL). Optional settings are applied first in the same undo step. "
               "Front-only depth is INFERRED and keeps NEEDS_REVIEW.",
               object(update_schema()), false, false, true, lowpoly_build});
  t.push_back({"lowpoly_render", "Render low-poly character",
               "Preview renders of the built low-poly character (orthographic, consistent framing): yaw list (default 0, "
               "90, 180, 270), rest pose or the template's test pose, style override, wireframe, contact sheet. "
               "Presentation only: never a check.",
               object({{"yaws", array(number("", -3600, 3600), "Yaw angles in degrees (0 = front, 90 = from the right).", 1, 72)},
                       {"elevation", number("Camera elevation in degrees (default 10).", -90, 90)},
                       {"size", integer("Image size in pixels (default 384).", 16, 2048)},
                       {"pose", boolean("Render the test pose (skinned) instead of the rest pose.")},
                       {"style", enumeration("Override the project's look.", {"flat", "cel", "outline", "cel_outline"})},
                       {"wire", boolean("Draw triangle edges.")},
                       {"contact_sheet", boolean("Also return one image with every frame in a row.")},
                       {"output_dir", str("Also write the PNGs into this directory.")}}),
               true, false, true, lowpoly_render});
  t.push_back({"lowpoly_export", "Export low-poly character",
               "Write the skinned low-poly GLB (exactly the bytes built and round-tripped at lowpoly_build). PASS is "
               "written; NEEDS_REVIEW only with accept_status NEEDS_REVIEW (the user's decision) or when the user "
               "accepted inferred depth; FAIL never. engine_files also writes a Godot 4 toon + outline shader pair and "
               "Unity (URP) setup notes next to it.",
               object({{"path", str("GLB path.")},
                       {"accept_status", enumeration("Explicitly accept this status.", {"PASS", "NEEDS_REVIEW"})},
                       {"engine_files", boolean("Also write <stem>_toon.gdshader, <stem>_outline.gdshader, <stem>_unity.md.")}},
                      {"path"}),
               false, false, true, lowpoly_export});
}

}  // namespace pixel3d::mcp
