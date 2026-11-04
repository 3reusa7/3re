#include "pixel3d/io/json_io.hpp"

#include "pixel3d/common/sha256.hpp"
#include "pixel3d/common/version.hpp"

namespace pixel3d {
namespace {

Expected<ViewRole> role_from(const nlohmann::json& j) {
  const auto r = view_role_from_string(j.get<std::string>());
  if (!r) return make_error("project.invalid", "unknown view role " + j.dump());
  return *r;
}

SurfaceMode surface_mode_from(const std::string& s) {
  for (SurfaceMode m : {SurfaceMode::Seen, SurfaceMode::OppositeProjection, SurfaceMode::EdgeExtend,
                        SurfaceMode::FlatPalette, SurfaceMode::Manual})
    if (to_string(m) == s) return m;
  return SurfaceMode::Manual;
}

nlohmann::json mesh_inputs(const Document& d) {
  nlohmann::json j = inputs_to_json(d);
  j.erase("texture");
  j.erase("texture_assignments");
  j.erase("part_names");
  j["settings"].erase("name");
  j["settings"].erase("pixels_per_meter");
  j["settings"].erase("profile");
  j["settings"].erase("style");  // the crisp model does not depend on the style
  j.erase("lowpoly");            // nor on LOW-POLY STUDIO (a separate output)
  j.erase("part_types");
  for (auto& v : j["views"]) v.erase("original_path");
  j["engine"] = std::string(version());  // geometry may change between engine versions
  return j;
}

// Everything that shapes or colours the polished model: the crisp inputs, the
// texture inputs and the style.
nlohmann::json style_inputs(const Document& d) {
  nlohmann::json j = inputs_to_json(d);
  j.erase("part_names");
  j.erase("lowpoly");
  j.erase("part_types");
  j["settings"].erase("name");
  j["settings"].erase("pixels_per_meter");
  j["settings"].erase("profile");
  for (auto& v : j["views"]) v.erase("original_path");
  j["engine"] = std::string(version());
  return j;
}

}  // namespace

nlohmann::json polygons_to_json(const Polygons2D& p) {
  nlohmann::json j = nlohmann::json::array();
  for (const Ring& r : p) {
    nlohmann::json ring = nlohmann::json::array();
    for (const vec2d& v : r) ring.push_back({v.x, v.y});
    j.push_back(ring);
  }
  return j;
}

Polygons2D polygons_from_json(const nlohmann::json& j) {
  Polygons2D p;
  for (const auto& ring : j) {
    Ring r;
    for (const auto& v : ring) r.push_back({v.at(0).get<double>(), v.at(1).get<double>()});
    p.push_back(std::move(r));
  }
  return p;
}

std::string source_image_entry(const SourceView& v) {
  const ImageFormat f = v.original_bytes ? sniff_image_format(*v.original_bytes, v.original_path) : ImageFormat::Png;
  return "views/" + v.sha256 + "." + std::string(file_extension(f == ImageFormat::Unknown ? ImageFormat::Png : f));
}

bool is_source_image_entry(const std::string& name, const std::string& sha) {
  for (ImageFormat f : {ImageFormat::Png, ImageFormat::Jpeg, ImageFormat::Bmp, ImageFormat::Gif, ImageFormat::Tga,
                        ImageFormat::Webp, ImageFormat::Aseprite, ImageFormat::Psd})
    if (name == "views/" + sha + "." + std::string(file_extension(f))) return true;
  return false;
}

nlohmann::json inputs_to_json(const Document& d) {
  nlohmann::json j;
  const ProjectSettings& s = d.settings;
  j["settings"] = {{"name", s.name},
                   {"alpha_threshold", s.alpha_threshold},
                   {"pixels_per_meter", s.pixels_per_meter},
                   {"profile", to_string(s.profile)},
                   {"simplify", {{"angle_snap", s.simplify.angle_snap}, {"min_unit_steps", s.simplify.min_unit_steps}}},
                   {"normalize", to_json(s.normalize)},
                   {"style", to_json(s.style)}};
  j["views"] = nlohmann::json::array();
  for (const SourceView& v : d.views)
    j["views"].push_back({{"role", to_string(v.role)},
                          {"sha256", v.sha256},
                          {"original_path", v.original_path},
                          {"image", source_image_entry(v)}});
  j["force_flip"] = nlohmann::json::object();
  for (const auto& [r, f] : d.align_options.force_flip) j["force_flip"][std::string(to_string(r))] = f;
  j["mode"] = to_string(d.mode);
  j["cleanup"] = {{"min_fragment_volume", d.cleanup_options.min_fragment_volume}};
  j["texture"] = {{"allow_opposite_projection", d.texture_options.allow_opposite_projection},
                  {"visibility_samples", d.texture_options.visibility_samples},
                  {"visibility_fraction", d.texture_options.visibility_fraction}};
  j["decisions"] = nlohmann::json::array();
  for (const UserDecision& u : d.decisions) j["decisions"].push_back({{"candidate", u.candidate}, {"keep", u.keep}});
  j["depth_constraints"] = nlohmann::json::array();
  for (const DepthConstraint& c : d.depth_constraints)
    j["depth_constraints"].push_back({{"view", to_string(c.view)},
                                      {"region", polygons_to_json(c.region)},
                                      {"from", c.depth_from},
                                      {"to", c.depth_to},
                                      {"label", c.label}});
  j["edits"] = nlohmann::json::array();
  for (const EditOp& e : d.edits) j["edits"].push_back(edit_to_json(e));
  j["part_masks"] = part_masks_to_json(d.masks);
  j["part_names"] = nlohmann::json::object();
  for (const auto& [id, name] : d.part_names) j["part_names"][std::to_string(id)] = name;
  j["next_part_id"] = d.next_part_id;
  j["single_view"] = single_view_to_json(d.single_view);
  j["lowpoly"] = to_json(s.lowpoly);
  j["part_types"] = part_types_to_json(d.part_types);
  j["texture_assignments"] = nlohmann::json::array();
  for (const TextureAssignment& a : d.texture_assignments)
    j["texture_assignments"].push_back(
        {{"tris", a.tris}, {"view", to_string(a.view)}, {"mode", to_string(a.mode)}, {"mesh_key", a.mesh_key}});
  return j;
}

Expected<Document> inputs_from_json(const nlohmann::json& j, const std::map<std::string, ImageBlob>& images) {
  Document d;
  try {
    const auto& s = j.at("settings");
    d.settings.name = s.value("name", "untitled");
    const int alpha = s.value("alpha_threshold", 128);
    if (alpha < 0 || alpha > 255) return make_error("project.invalid", "alpha_threshold must be in [0, 255]");
    d.settings.alpha_threshold = static_cast<uint8_t>(alpha);
    d.settings.pixels_per_meter = s.value("pixels_per_meter", 32.0);
    d.settings.profile = s.value("profile", "hand") == "strict" ? ToleranceProfile::Strict : ToleranceProfile::Hand;
    if (s.contains("simplify")) {
      d.settings.simplify.angle_snap = s["simplify"].value("angle_snap", true);
      d.settings.simplify.min_unit_steps = s["simplify"].value("min_unit_steps", 2);
    }
    if (s.contains("normalize")) {
      const auto& n = s["normalize"];
      NormalizeSettings& ns = d.settings.normalize;
      ns.enabled = n.value("enabled", ns.enabled);
      ns.target_size = n.value("target_size", ns.target_size);
      ns.bg_tolerance = n.value("bg_tolerance", ns.bg_tolerance);
      ns.axis_tolerance = n.value("axis_tolerance", ns.axis_tolerance);
    }
    if (s.contains("style")) {
      auto style = style_settings_from_json(s["style"]);
      if (!style) {
        Message e = style.error();
        e.code = "project.invalid";
        return Unexpected{e};
      }
      d.settings.style = std::move(*style);
    }
    for (const auto& v : j.at("views")) {
      auto role = role_from(v.at("role"));
      if (!role) return Unexpected{role.error()};
      const std::string sha = v.at("sha256").get<std::string>();
      const auto it = images.find(sha);
      if (it == images.end()) return make_error("project.invalid", "missing source image " + sha);
      SourceView sv;
      sv.role = *role;
      sv.sha256 = sha;
      sv.original_path = v.value("original_path", "");
      sv.original = it->second.image;
      sv.original_bytes = it->second.bytes;
      d.views.push_back(std::move(sv));
    }
    const nlohmann::json flips = j.value("force_flip", nlohmann::json::object());  // keep alive while iterating
    for (const auto& [k, f] : flips.items())
      if (auto r = view_role_from_string(k)) d.align_options.force_flip[*r] = f.get<bool>();
    d.mode = j.value("mode", "true_solid") == "voxel" ? ReconstructMode::Voxel : ReconstructMode::TrueSolid;
    if (j.contains("cleanup")) {
      // weld_vertices / weld_eps of older files never had an effect and are ignored.
      d.cleanup_options.min_fragment_volume = j["cleanup"].value("min_fragment_volume", 0.5);
    }
    if (j.contains("texture")) {
      d.texture_options.allow_opposite_projection = j["texture"].value("allow_opposite_projection", true);
      d.texture_options.visibility_samples = j["texture"].value("visibility_samples", 16);
      d.texture_options.visibility_fraction = j["texture"].value("visibility_fraction", 0.5);
    }
    for (const auto& u : j.value("decisions", nlohmann::json::array()))
      d.decisions.push_back({u.at("candidate").get<int>(), u.at("keep").get<bool>()});
    for (const auto& c : j.value("depth_constraints", nlohmann::json::array())) {
      auto role = role_from(c.at("view"));
      if (!role) return Unexpected{role.error()};
      d.depth_constraints.push_back(DepthConstraint{*role, polygons_from_json(c.at("region")), c.at("from").get<double>(),
                                                    c.at("to").get<double>(), c.value("label", "")});
    }
    for (const auto& a : j.value("texture_assignments", nlohmann::json::array())) {
      auto role = role_from(a.at("view"));
      if (!role) return Unexpected{role.error()};
      d.texture_assignments.push_back(
          TextureAssignment{a.at("tris").get<std::vector<int>>(), *role, surface_mode_from(a.at("mode").get<std::string>()),
                            a.value("mesh_key", "")});
    }
    if (j.contains("edits")) {
      if (!j["edits"].is_array()) return make_error("project.invalid", "edits must be a list");
      for (const auto& ej : j["edits"]) {
        auto op = edit_from_json(ej);
        if (!op) return Unexpected{op.error()};
        d.edits.push_back(std::move(*op));
      }
    }
    auto masks = part_masks_from_json(j.value("part_masks", nlohmann::json()));
    if (!masks) return Unexpected{masks.error()};
    d.masks = std::move(*masks);
    const nlohmann::json names = j.value("part_names", nlohmann::json::object());  // keep alive while iterating
    for (const auto& [k, v] : names.items()) d.part_names[std::stoi(k)] = v.get<std::string>();
    d.next_part_id = j.value("next_part_id", static_cast<int>(kFirstUserPart));
    auto sv = single_view_from_json(j.value("single_view", nlohmann::json()));
    if (!sv) return Unexpected{sv.error()};
    d.single_view = *sv;
    if (j.contains("lowpoly")) {
      auto lp = lowpoly_settings_from_json(j["lowpoly"]);
      if (!lp) {
        Message e = lp.error();
        e.code = "project.invalid";
        return Unexpected{e};
      }
      d.settings.lowpoly = std::move(*lp);
    }
    if (j.contains("part_types")) {
      auto pt = part_types_from_json(j["part_types"]);
      if (!pt) {
        Message e = pt.error();
        e.code = "project.invalid";
        return Unexpected{e};
      }
      d.part_types = std::move(*pt);
    }
  } catch (const std::exception& e) {
    return make_error("project.invalid", std::string("malformed project inputs: ") + e.what());
  }
  return d;
}

std::string mesh_input_hash(const Document& d) { return sha256_hex(mesh_inputs(d).dump()); }
std::string style_input_hash(const Document& d) { return sha256_hex(style_inputs(d).dump()); }

}  // namespace pixel3d
