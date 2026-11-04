#include "pixel3d/engine/doc_diff.hpp"

#include <map>
#include <set>

namespace pixel3d {
namespace {

std::map<ViewRole, std::string> view_ids(const Document& d) {
  std::map<ViewRole, std::string> m;
  for (const SourceView& v : d.views) m[v.role] = v.sha256;
  return m;
}

nlohmann::json diff_views(const Document& a, const Document& b) {
  const auto va = view_ids(a), vb = view_ids(b);
  nlohmann::json j;
  for (const auto& [r, sha] : vb) {
    const auto it = va.find(r);
    if (it == va.end()) j["added"].push_back(to_string(r));
    else if (it->second != sha) j["replaced"].push_back(to_string(r));
  }
  for (const auto& [r, sha] : va)
    if (!vb.contains(r)) j["removed"].push_back(to_string(r));
  return j;
}

nlohmann::json diff_settings(const ProjectSettings& a, const ProjectSettings& b) {
  nlohmann::json j = nlohmann::json::array();
  if (a.name != b.name) j.push_back("name");
  if (a.alpha_threshold != b.alpha_threshold) j.push_back("alpha_threshold");
  if (a.pixels_per_meter != b.pixels_per_meter) j.push_back("pixels_per_meter");
  if (a.profile != b.profile) j.push_back("profile");
  if (a.simplify.angle_snap != b.simplify.angle_snap || a.simplify.min_unit_steps != b.simplify.min_unit_steps)
    j.push_back("simplify");
  return j;
}

nlohmann::json diff_decisions(const Document& a, const Document& b) {
  std::map<int, bool> ma, mb;
  for (const UserDecision& u : a.decisions) ma[u.candidate] = u.keep;
  for (const UserDecision& u : b.decisions) mb[u.candidate] = u.keep;
  if (ma == mb) return nullptr;
  nlohmann::json j;
  for (const auto& [c, k] : mb)
    if (!ma.contains(c) || ma[c] != k) j["set"].push_back({{"candidate", c}, {"keep", k}});
  for (const auto& [c, k] : ma)
    if (!mb.contains(c)) j["cleared"].push_back(c);
  return j;
}

// Parts are identified by their member candidates (part ids are positional).
nlohmann::json diff_parts(const Document& a, const Document& b) {
  if (!a.correspondence && !b.correspondence) return nullptr;
  std::map<std::vector<int>, const Part3D*> pa, pb;
  if (a.correspondence)
    for (const Part3D& p : a.correspondence->parts) pa[p.candidates] = &p;
  if (b.correspondence)
    for (const Part3D& p : b.correspondence->parts) pb[p.candidates] = &p;
  nlohmann::json j;
  for (const auto& [key, p] : pb) {
    const auto it = pa.find(key);
    if (it == pa.end()) j["added"].push_back(p->id);
    else if (it->second->status != p->status)
      j["status_changed"].push_back({{"part", p->id}, {"from", to_string(it->second->status)}, {"to", to_string(p->status)}});
  }
  int removed = 0;
  for (const auto& [key, p] : pa)
    if (!pb.contains(key)) ++removed;
  if (removed) j["removed_count"] = removed;
  return j;
}

nlohmann::json diff_candidates(const Document& a, const Document& b) {
  if (!a.candidates && !b.candidates) return nullptr;
  if (!a.candidates || !b.candidates || a.candidates->cands.size() != b.candidates->cands.size())
    return b.candidates ? nlohmann::json("rebuilt") : nlohmann::json("cleared");
  nlohmann::json j = nlohmann::json::array();
  for (size_t i = 0; i < b.candidates->cands.size(); ++i) {
    const Candidate& ca = a.candidates->cands[i];
    const Candidate& cb = b.candidates->cands[i];
    if (ca.status != cb.status)
      j.push_back({{"candidate", cb.id}, {"from", to_string(ca.status)}, {"to", to_string(cb.status)}});
  }
  return j.empty() ? nlohmann::json() : j;
}

nlohmann::json mesh_summary(const Document& d) {
  if (!d.solid) return nullptr;
  return {{"triangles", d.solid->topology.triangle_count}, {"watertight", d.solid->topology.watertight}};
}

}  // namespace

nlohmann::json diff_documents(const Document& a, const Document& b) {
  nlohmann::json j = nlohmann::json::object();
  if (auto s = diff_settings(a.settings, b.settings); !s.empty()) j["settings"] = s;
  if (auto v = diff_views(a, b); !v.is_null()) j["views"] = v;
  if (auto dd = diff_decisions(a, b); !dd.is_null()) j["decisions"] = dd;
  if (a.depth_constraints.size() != b.depth_constraints.size()) {
    nlohmann::json ids = nlohmann::json::array();
    for (size_t i = a.depth_constraints.size(); i < b.depth_constraints.size(); ++i) ids.push_back(i);
    j["depth_constraints"] = {{"before", a.depth_constraints.size()}, {"after", b.depth_constraints.size()}, {"added", ids}};
  }
  if (a.texture_assignments.size() != b.texture_assignments.size())
    j["texture_assignments"] = {{"before", a.texture_assignments.size()}, {"after", b.texture_assignments.size()}};
  if (a.align != b.align && b.align) j["alignment"] = {{"dims", {b.align->dims.W, b.align->dims.H, b.align->dims.D}}};
  if (auto c = diff_candidates(a, b); !c.is_null()) j["candidates"] = c;
  if (auto p = diff_parts(a, b); !p.is_null() && !p.empty()) j["parts"] = p;
  if (a.solid != b.solid) j["mesh"] = {{"before", mesh_summary(a)}, {"after", mesh_summary(b)}};
  if (a.texture != b.texture) j["texture"] = {{"before", a.texture != nullptr}, {"after", b.texture != nullptr}};
  const auto status = [](const Document& d) { return std::string(to_string(d.gate ? d.gate->status : ValidationStatus::NotValidated)); };
  if (status(a) != status(b)) j["validation"] = {{"before", status(a)}, {"after", status(b)}};
  return j;
}

}  // namespace pixel3d
