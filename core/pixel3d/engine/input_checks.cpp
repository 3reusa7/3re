#include "pixel3d/engine/input_checks.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "pixel3d/common/limits.hpp"

namespace pixel3d {
namespace {

Message invalid(const std::string& field, const std::string& why, nlohmann::json value = {}) {
  nlohmann::json where{{"field", field}};
  if (!value.is_null()) where["value"] = std::move(value);
  return Message{"input.invalid", field + ": " + why, Severity::Error, std::move(where)};
}

bool finite_in(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

}  // namespace

std::optional<Message> check_settings(const ProjectSettings& s) {
  if (s.name.size() > 256) return invalid("settings.name", "longer than 256 bytes");
  if (!finite_in(s.pixels_per_meter, 1e-3, 1e6))
    return invalid("settings.pixels_per_meter", "must be a finite number in [0.001, 1000000]", s.pixels_per_meter);
  if (s.simplify.min_unit_steps < 1 || s.simplify.min_unit_steps > 64)
    return invalid("settings.simplify.min_unit_steps", "must be in [1, 64]", s.simplify.min_unit_steps);
  if (auto e = check_normalize_settings(s.normalize)) return e;
  if (auto e = check_style_settings(s.style)) return e;
  if (auto e = check_lowpoly_settings(s.lowpoly)) return e;
  return std::nullopt;
}

std::optional<Message> check_cleanup_options(const CleanupOptions& o) {
  if (!finite_in(o.min_fragment_volume, 0, 1e9))
    return invalid("cleanup.min_fragment_volume", "must be in [0, 1e9]", o.min_fragment_volume);
  return std::nullopt;
}

std::optional<Message> check_texture_options(const TextureOptions& o) {
  if (o.visibility_samples < 1 || o.visibility_samples > 1024)
    return invalid("texture.visibility_samples", "must be in [1, 1024]", o.visibility_samples);
  if (!finite_in(o.visibility_fraction, 0, 1))
    return invalid("texture.visibility_fraction", "must be in [0, 1]", o.visibility_fraction);
  return std::nullopt;
}

std::optional<Message> check_depth_constraint(const DepthConstraint& c) {
  if (!finite_in(c.depth_from, -1e9, 1e9) || !finite_in(c.depth_to, -1e9, 1e9))
    return invalid("depth", "depth_from and depth_to must be finite numbers within ±1e9");
  if (c.depth_from > c.depth_to) return invalid("depth", "depth_from must not exceed depth_to");
  if (c.label.size() > 256) return invalid("depth.label", "longer than 256 bytes");
  if (c.region.empty()) return invalid("depth.region", "the region is empty");
  size_t points = 0;
  for (const Ring& r : c.region) {
    if (r.size() < 3) return invalid("depth.region", "every ring needs at least 3 points");
    points += r.size();
    if (points > static_cast<size_t>(limits::kMaxPolygonPoints)) return invalid("depth.region", "too many points");
    for (const vec2d& p : r)
      if (!finite_in(p.x, -1e6, 1e6) || !finite_in(p.y, -1e6, 1e6))
        return invalid("depth.region", "coordinates must be finite and within ±1e6");
  }
  return std::nullopt;
}

std::optional<Message> check_texture_assignment(const TextureAssignment& a) {
  if (a.tris.size() > static_cast<size_t>(limits::kMaxAssignedTriangles))
    return invalid("texture_assignment.tris", "too many triangles");
  for (int t : a.tris)
    if (t < 0) return invalid("texture_assignment.tris", "triangle indices must be non-negative", t);
  return std::nullopt;
}

std::optional<Message> check_decision(const UserDecision& u) {
  if (u.candidate < 0) return invalid("decision.candidate", "candidate ids are non-negative", u.candidate);
  return std::nullopt;
}

std::optional<Message> check_document_inputs(const Document& d) {
  if (auto e = check_settings(d.settings)) return e;
  if (auto e = check_cleanup_options(d.cleanup_options)) return e;
  if (auto e = check_texture_options(d.texture_options)) return e;
  std::set<ViewRole> roles;
  for (const SourceView& v : d.views)
    if (!roles.insert(v.role).second) return invalid("views", "a view role appears twice", std::string(to_string(v.role)));
  if (d.decisions.size() > static_cast<size_t>(limits::kMaxDecisions)) return invalid("decisions", "too many decisions");
  for (const UserDecision& u : d.decisions)
    if (auto e = check_decision(u)) return e;
  if (d.depth_constraints.size() > static_cast<size_t>(limits::kMaxDepthConstraints))
    return invalid("depth_constraints", "too many depth constraints");
  for (const DepthConstraint& c : d.depth_constraints)
    if (auto e = check_depth_constraint(c)) return e;
  if (d.edits.size() > 100000) return invalid("edits", "too many edits");
  int highest = kFirstUserPart - 1;
  for (const EditOp& e : d.edits) {
    if (auto m = check_edit(e)) return m;
    for (int p : created_parts(e)) highest = std::max(highest, p);
  }
  for (const auto& [id, up] : d.masks.parts) highest = std::max(highest, id);
  if (d.next_part_id <= highest || d.next_part_id >= (1 << 30))
    return invalid("next_part_id", "must exceed every user part id", d.next_part_id);
  if (d.part_names.size() > 100000) return invalid("part_names", "too many names");
  for (const auto& [id, name] : d.part_names)
    if (name.empty() || name.size() > 256) return invalid("part_names", "names are 1-256 bytes", id);
  if (d.single_view.enabled && d.views.size() > 1) return invalid("single_view", "single view mode with more than one view");
  if (d.texture_assignments.size() > static_cast<size_t>(limits::kMaxTextureAssignments))
    return invalid("texture_assignments", "too many texture assignments");
  for (const TextureAssignment& a : d.texture_assignments) {
    if (auto e = check_texture_assignment(a)) return e;
    if (!roles.contains(a.view))
      return invalid("texture_assignment.view", "no source view with this role", std::string(to_string(a.view)));
  }
  return std::nullopt;
}

}  // namespace pixel3d
