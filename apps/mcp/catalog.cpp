#include "mcp/catalog.hpp"

#include "pixel3d/engine/inspect.hpp"

namespace pixel3d::mcp {

using nlohmann::json;

namespace {

struct StaticResource {
  const char* uri;
  const char* name;
  const char* description;
  json (*read)(const Session&);
};

const StaticResource kResources[] = {
    {"pixel3d://project", "project", "Settings, pipeline stage, revision, undo/redo, checkpoints, status",
     [](const Session& s) { return inspect_project(s); }},
    {"pixel3d://views", "views", "Source views, alignment (scale, crop, flips, padding), palette and families",
     [](const Session& s) { return inspect_views(s.document()); }},
    {"pixel3d://parts", "parts", "Y-bands, candidate parts with status and evidence, 3D parts, user/edited parts with names, decisions, depth constraints",
     [](const Session& s) { return inspect_parts(s.document()); }},
    {"pixel3d://mesh/stats", "mesh statistics", "Topology, cleanup report, per-part triangles, texture statistics",
     [](const Session& s) { return inspect_mesh(s.document()); }},
    {"pixel3d://conflicts", "conflicts", "Ambiguities, inferred assumptions and drawing conflicts",
     [](const Session& s) { return inspect_conflicts(s); }},
    {"pixel3d://diagnostics", "diagnostics", "Engine version, resource limits, recent operation log, messages",
     [](const Session& s) { return inspect_diagnostics(s); }},
    {"pixel3d://validation", "validation results", "Last validation report (status, checks, metrics) or NOT_VALIDATED",
     [](const Session& s) { return inspect_validation(s.document()); }},
    {"pixel3d://edits", "edits", "Geometry edits, user parts and their masks, single view settings",
     [](const Session& s) { return inspect_edits(s.document()); }},
    {"pixel3d://live", "live checks", "Projection, topology and texture checks of the current mesh (after every edit)",
     [](const Session& s) { return inspect_live(s.document()); }},
    {"pixel3d://selection", "selection", "The current selection and its projection in every view",
     [](const Session& s) { return s.get_selection().data; }},
};

constexpr char kViewPrefix[] = "pixel3d://views/";

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

// The SKILL.md body without its YAML front matter.
std::string_view skill_body() {
  std::string_view s = skill_markdown();
  if (s.substr(0, 3) == "---") {
    const size_t end = s.find("\n---", 3);
    if (end != std::string_view::npos) {
      s.remove_prefix(end + 4);
      while (!s.empty() && (s.front() == '\n' || s.front() == '\r')) s.remove_prefix(1);
    }
  }
  return s;
}

}  // namespace

std::string base64(std::string_view bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t v = (uint32_t(uint8_t(bytes[i])) << 16) | (uint32_t(uint8_t(bytes[i + 1])) << 8) | uint8_t(bytes[i + 2]);
    for (int k = 18; k >= 0; k -= 6) out.push_back(kAlphabet[(v >> k) & 63]);
  }
  if (i < bytes.size()) {
    uint32_t v = uint32_t(uint8_t(bytes[i])) << 16;
    if (i + 1 < bytes.size()) v |= uint32_t(uint8_t(bytes[i + 1])) << 8;
    out.push_back(kAlphabet[(v >> 18) & 63]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

json resource_list() {
  json list = json::array();
  for (const StaticResource& r : kResources)
    list.push_back({{"uri", r.uri}, {"name", r.name}, {"description", r.description}, {"mimeType", "application/json"}});
  return list;
}

json resource_templates() {
  return json::array({{{"uriTemplate", "pixel3d://views/{role}/aligned.png"},
                       {"name", "aligned view image"},
                       {"description", "A source view after alignment (scale-normalised, cropped, flipped): the pixel "
                                       "coordinates that regions and footprints use. role: front, back, side, right, top, bottom"},
                       {"mimeType", "image/png"}},
                      {{"uriTemplate", "pixel3d://views/{role}/original.png"},
                       {"name", "original view image"},
                       {"description", "A source view exactly as imported"},
                       {"mimeType", "image/png"}}});
}

std::optional<json> read_resource(const Session& s, const std::string& uri) {
  for (const StaticResource& r : kResources)
    if (uri == r.uri) return json::array({{{"uri", uri}, {"mimeType", "application/json"}, {"text", dump(r.read(s))}}});
  if (uri.rfind(kViewPrefix, 0) != 0) return std::nullopt;
  const std::string rest = uri.substr(sizeof(kViewPrefix) - 1);
  const size_t slash = rest.find('/');
  if (slash == std::string::npos) return std::nullopt;
  const auto role = view_role_from_string(rest.substr(0, slash));
  const std::string kind = rest.substr(slash + 1);
  if (!role || (kind != "aligned.png" && kind != "original.png")) return std::nullopt;
  for (const SourceView& v : s.document().views) {
    if (v.role != *role) continue;
    std::string bytes;
    if (kind == "original.png" && v.original_bytes) {
      bytes.assign(v.original_bytes->begin(), v.original_bytes->end());
    } else {
      const auto& img = kind == "aligned.png" ? v.aligned : v.original;
      if (!img) return std::nullopt;  // not aligned yet
      const std::vector<uint8_t> png = encode_png(*img);
      bytes.assign(png.begin(), png.end());
    }
    return json::array({{{"uri", uri}, {"mimeType", "image/png"}, {"blob", base64(bytes)}}});
  }
  return std::nullopt;
}

json prompt_list() {
  return json::array({{{"name", "reconstruct_from_views"},
                       {"title", "Reconstruct a 3D model from pixel-art views"},
                       {"description", "The Pixel3D agent workflow: inspect, align, analyze, reconstruct, inspect "
                                       "ambiguity, repair only when determinable, texture, validate, 360° inspect, "
                                       "clean up, revalidate, export."},
                       {"arguments", json::array({{{"name", "front"}, {"description", "Front view PNG path"}, {"required", false}},
                                                  {{"name", "side"}, {"description", "Side (left) view PNG path"}, {"required", false}},
                                                  {{"name", "top"}, {"description", "Top view PNG path"}, {"required", false}},
                                                  {{"name", "output"}, {"description", "GLB path to export"}, {"required", false}},
                                                  {{"name", "notes"},
                                                   {"description", "What the user knows about the object (parts, depths)"},
                                                   {"required", false}}})}}});
}

std::optional<json> get_prompt(const std::string& name, const json& arguments) {
  if (name != "reconstruct_from_views") return std::nullopt;
  std::string task = "Reconstruct a 3D model with the Pixel3D tools, following the workflow below.\n";
  const json args = arguments.is_object() ? arguments : json::object();
  for (const char* k : {"front", "side", "top", "output", "notes"})
    if (args.contains(k) && args[k].is_string()) task += std::string("- ") + k + ": " + args[k].get<std::string>() + "\n";
  task += "\n";
  task += skill_body();
  return json{{"description", "Pixel3D reconstruction workflow"},
              {"messages", json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", task}}}}})}};
}

std::string server_instructions() {
  return "Pixel3D turns orthographic pixel-art views (Front, Side = seen from the left, Top = front edge at the "
         "bottom; optional Back/Right/Bottom) into a validated watertight textured GLB. Workflow: inspect -> "
         "import_views -> auto_align -> analyze_views -> reconstruct -> get_conflicts / get_inferred_geometry -> "
         "repair only what the drawings or the user determine (edit_part, set_part_depth, extra views) -> "
         "project_textures -> validate -> render_turntable / render_view (look at the images) -> cleanup_mesh -> "
         "validate -> export_glb -> save_project. Editing tools (paint_depth, push_pull, mirror, part masks, ...) "
         "are for changes the user asks for; an edit that moves a silhouette away from a drawing blocks PASS. "
         "Every result has status PASS / NEEDS_REVIEW / FAIL / NOT_VALIDATED; NEEDS_REVIEW is not a pass and export_glb refuses it unless accept_status is given with "
         "the user's consent. Full workflow: prompt 'reconstruct_from_views'.";
}

}  // namespace pixel3d::mcp
