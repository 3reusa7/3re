#include "pixel3d/edit/edit_ops.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "pixel3d/common/limits.hpp"
#include "pixel3d/io/json_io.hpp"
#include "pixel3d/reconstruct/prism.hpp"
#include "pixel3d/render/camera.hpp"
#include "pixel3d/render/rasterizer.hpp"

namespace pixel3d {

using nlohmann::json;
namespace mf = manifold;

namespace {

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

Message err(const std::string& code, const std::string& text, json where = {}) {
  return Message{code, text, Severity::Error, std::move(where)};
}

mf::vec3 axis_vec(int axis) { return {axis == 0 ? 1.0 : 0.0, axis == 1 ? 1.0 : 0.0, axis == 2 ? 1.0 : 0.0}; }

json vec(const std::array<double, 3>& a) { return json::array({a[0], a[1], a[2]}); }

std::array<double, 3> vec_from(const json& j) {
  if (!j.is_array() || j.size() != 3) throw std::runtime_error("expected [x, y, z]");
  return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

bool finite3(const std::array<double, 3>& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }

std::optional<Message> check_region(const Polygons2D& r) {
  if (r.empty()) return err("input.invalid", "region: empty");
  size_t n = 0;
  for (const Ring& ring : r) {
    if (ring.size() < 3) return err("input.invalid", "region: every ring needs at least 3 points");
    n += ring.size();
    for (const vec2d& p : ring)
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 1e6 || std::abs(p.y) > 1e6)
        return err("input.invalid", "region: coordinates must be finite and within ±1e6");
  }
  if (n > static_cast<size_t>(limits::kMaxPolygonPoints)) return err("input.invalid", "region: too many points");
  return std::nullopt;
}

mf::Polygons to_polys(const Polygons2D& p) {
  mf::Polygons out;
  for (const Ring& r : p) {
    mf::SimplePolygon sp;
    for (const vec2d& q : r) sp.push_back({q.x, q.y});
    out.push_back(std::move(sp));
  }
  return out;
}

std::optional<Message> need_part(const PartGeometry& g, int part) {
  if (!g.has(part)) return err("edit.unknown_part", "no part " + std::to_string(part), {{"part", part}});
  return std::nullopt;
}

void unite_into(PartGeometry& g, int part, const mf::Manifold& m) {
  auto it = g.parts.find(part);
  if (it == g.parts.end()) g.parts[part] = m;
  else it->second = it->second + m;
}

std::optional<Message> apply_volume(PartGeometry& g, const VolumeEdit& e, Dims d) {
  const mf::Manifold prism = make_prism_range(e.view, d, e.region, e.depth_from, e.depth_to);
  if (prism.IsEmpty()) return err("edit.empty", "the region and depth range enclose no volume");
  if (e.add) {
    if (e.part >= 0) {
      if (auto m = need_part(g, e.part)) return m;
      unite_into(g, e.part, prism);
    } else {
      unite_into(g, e.new_part, prism);
    }
    return std::nullopt;
  }
  if (e.part >= 0) {
    if (auto m = need_part(g, e.part)) return m;
    g.parts[e.part] = g.parts[e.part] - prism;
  } else {
    for (auto& [id, m] : g.parts) m = m - prism;
  }
  return std::nullopt;
}

std::optional<Message> apply_sweep(PartGeometry& g, const SweepEdit& e) {
  const double h = std::abs(e.distance);
  if (h < 1e-9) return err("edit.empty", "push/pull distance is zero");
  const mf::Polygons polys = to_polys(e.polygon);
  // Pull: from just inside the face outward; push: from just outside inward.
  constexpr double kOverlap = 0.01;
  mf::Manifold local = e.distance > 0 ? mf::Manifold::Extrude(polys, h + kOverlap).Translate({0, 0, -kOverlap})
                                      : mf::Manifold::Extrude(polys, h + kOverlap).Translate({0, 0, -h});
  const mf::mat3x4 xf({e.u[0], e.u[1], e.u[2]}, {e.v[0], e.v[1], e.v[2]}, {e.normal[0], e.normal[1], e.normal[2]},
                      {e.origin[0], e.origin[1], e.origin[2]});
  const mf::Manifold sweep = local.Transform(xf);
  if (sweep.IsEmpty()) return err("edit.empty", "the face polygon encloses no area");
  if (e.distance > 0) {
    if (auto m = need_part(g, e.part)) return m;
    unite_into(g, e.part, sweep);
  } else if (e.part >= 0) {
    if (auto m = need_part(g, e.part)) return m;
    g.parts[e.part] = g.parts[e.part] - sweep;
  } else {
    for (auto& [id, m] : g.parts) m = m - sweep;
  }
  return std::nullopt;
}

std::optional<Message> apply_cut(PartGeometry& g, const CutEdit& e) {
  const mf::vec3 n = axis_vec(e.axis);
  auto split = [&](const mf::Manifold& m) { return m.SplitByPlane(n, e.at); };  // first = positive side
  if (e.part < 0) {
    for (auto& [id, m] : g.parts) {
      auto [pos, neg] = split(m);
      m = e.keep > 0 ? pos : neg;
    }
    return std::nullopt;
  }
  if (auto m = need_part(g, e.part)) return m;
  auto [pos, neg] = split(g.parts[e.part]);
  if (e.keep > 0) g.parts[e.part] = pos;
  else if (e.keep < 0) g.parts[e.part] = neg;
  else {
    g.parts[e.part] = neg;
    if (!pos.IsEmpty()) g.parts[e.new_part] = pos;
  }
  return std::nullopt;
}

std::optional<Message> apply_merge(PartGeometry& g, const MergeEdit& e) {
  if (auto m = need_part(g, e.into)) return m;
  for (int p : e.parts) {
    if (p == e.into) continue;
    if (auto m = need_part(g, p)) return m;
  }
  for (int p : e.parts) {
    if (p == e.into) continue;
    g.parts[e.into] = g.parts[e.into] + g.parts[p];
    g.parts.erase(p);
  }
  return std::nullopt;
}

mf::Manifold mirrored(const mf::Manifold& m, int axis, double at) {
  const mf::vec3 n = axis_vec(axis);
  return m.Translate(-n * at).Mirror(n).Translate(n * at);
}

std::optional<Message> apply_mirror(PartGeometry& g, const MirrorEdit& e) {
  if (e.part < 0) {
    for (auto& [id, m] : g.parts) m = mirrored(m, e.axis, e.at);
    return std::nullopt;
  }
  if (auto m = need_part(g, e.part)) return m;
  const mf::Manifold r = mirrored(g.parts[e.part], e.axis, e.at);
  if (e.copy) g.parts[e.new_part] = r;
  else g.parts[e.part] = r;
  return std::nullopt;
}

std::optional<Message> apply_transform(PartGeometry& g, const TransformEdit& e) {
  if (auto m = need_part(g, e.part)) return m;
  const mf::vec3 pivot{e.pivot[0], e.pivot[1], e.pivot[2]};
  const mf::vec3 rot{e.rotate_axis == 0 ? e.rotate_deg : 0.0, e.rotate_axis == 1 ? e.rotate_deg : 0.0,
                     e.rotate_axis == 2 ? e.rotate_deg : 0.0};
  const mf::Manifold r = g.parts[e.part]
                             .Translate(-pivot)
                             .Scale({e.scale, e.scale, e.scale})
                             .Rotate(rot.x, rot.y, rot.z)
                             .Translate(pivot + mf::vec3{e.translate[0], e.translate[1], e.translate[2]});
  if (e.copy) g.parts[e.new_part] = r;
  else g.parts[e.part] = r;
  return std::nullopt;
}

std::optional<Message> apply_split_shells(PartGeometry& g, const SplitShellsEdit& e) {
  if (auto m = need_part(g, e.part)) return m;
  std::vector<mf::Manifold> shells = g.parts[e.part].Decompose();
  if (shells.size() <= 1) return std::nullopt;
  std::stable_sort(shells.begin(), shells.end(),
                   [](const mf::Manifold& a, const mf::Manifold& b) { return a.Volume() > b.Volume(); });
  if (e.new_parts.size() + 1 < shells.size())
    return err("edit.invalid", "not enough part ids for the shells", {{"shells", shells.size()}});
  g.parts[e.part] = shells[0];
  for (size_t i = 1; i < shells.size(); ++i) g.parts[e.new_parts[i - 1]] = shells[i];
  return std::nullopt;
}

std::optional<Message> apply_depth_stroke(PartGeometry& g, const DepthStrokeEdit& e, Dims d, std::vector<Message>* notes) {
  if (e.part >= 0)
    if (auto m = need_part(g, e.part)) return m;
  // The surface under each pixel, at this point of the edit list.
  const SolidResult cur = assemble_parts(g, CleanupOptions{});
  const Camera cam = camera_for_view(e.view, d, 1);
  const RenderTarget rt = render_mesh_flat(cur.mesh, cam);
  const ImageSize sz{cam.width, cam.height};
  // Pixels with the same (part, surface depth, offset) form one region: a few
  // prisms instead of one box per pixel.
  std::map<std::tuple<int, long long, long long>, Mask2D> groups;
  int skipped = 0;
  for (const DepthSample& s : e.samples) {
    if (s.u < 0 || s.v < 0 || s.u >= sz.w || s.v >= sz.h) {
      ++skipped;
      continue;
    }
    const size_t idx = static_cast<size_t>(s.v) * sz.w + s.u;
    const int tri = rt.tri_id[idx];
    if (tri < 0) {
      ++skipped;  // no surface under this pixel
      continue;
    }
    const int part = static_cast<size_t>(tri) < cur.mesh.tri_part.size() ? cur.mesh.tri_part[tri] : -1;
    if (e.part >= 0 && part != e.part) continue;
    const long long nq = std::llround(rt.depth[idx] * 4.0), oq = std::llround(s.offset * 4.0);
    if (oq == 0) continue;
    auto [it, ins] = groups.try_emplace({part, nq, oq}, Mask2D(sz.w, sz.h));
    it->second.set(s.u, s.v, true);
  }
  if (skipped > 0 && notes)
    notes->push_back(Message{"edit.depth_no_surface", std::to_string(skipped) + " brushed pixel(s) have no surface under them",
                             Severity::Info, {{"pixels", skipped}}});
  for (const auto& [key, mask] : groups) {
    const auto [part, nq, oq] = key;
    const double near = nq / 4.0, off = oq / 4.0;
    const Polygons2D region = mask_to_polygons(mask);
    if (off > 0) {
      unite_into(g, g.has(part) ? part : g.parts.begin()->first, make_prism_range(e.view, d, region, near - off, near + 0.25));
    } else {
      const mf::Manifold carve = make_prism_range(e.view, d, region, near - 0.5, near - off);
      if (e.part >= 0) g.parts[e.part] = g.parts[e.part] - carve;
      else
        for (auto& [id, m] : g.parts) m = m - carve;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string edit_name(const EditOp& op) {
  return std::visit(overloaded{[](const VolumeEdit& e) { return e.label; },
                               [](const SweepEdit& e) { return e.label; },
                               [](const CutEdit& e) { return std::string(e.keep == 0 ? "split_part" : "cut"); },
                               [](const MergeEdit&) { return std::string("merge_parts"); },
                               [](const MirrorEdit&) { return std::string("mirror"); },
                               [](const TransformEdit& e) { return std::string(e.copy ? "duplicate_part" : "transform_part"); },
                               [](const DeletePartEdit&) { return std::string("delete_part"); },
                               [](const SplitShellsEdit&) { return std::string("split_shells"); },
                               [](const DepthStrokeEdit&) { return std::string("paint_depth"); }},
                    op);
}

std::vector<int> created_parts(const EditOp& op) {
  return std::visit(overloaded{[](const VolumeEdit& e) { return e.add && e.part < 0 ? std::vector<int>{e.new_part} : std::vector<int>{}; },
                               [](const CutEdit& e) { return e.keep == 0 ? std::vector<int>{e.new_part} : std::vector<int>{}; },
                               [](const MirrorEdit& e) { return e.copy ? std::vector<int>{e.new_part} : std::vector<int>{}; },
                               [](const TransformEdit& e) { return e.copy ? std::vector<int>{e.new_part} : std::vector<int>{}; },
                               [](const SplitShellsEdit& e) { return e.new_parts; },
                               [](const auto&) { return std::vector<int>{}; }},
                    op);
}

json edit_to_json(const EditOp& op) {
  return std::visit(
      overloaded{
          [](const VolumeEdit& e) {
            return json{{"op", "volume"},       {"label", e.label},           {"add", e.add},
                        {"view", to_string(e.view)}, {"region", polygons_to_json(e.region)},
                        {"depth_from", e.depth_from}, {"depth_to", e.depth_to}, {"part", e.part},
                        {"new_part", e.new_part}};
          },
          [](const SweepEdit& e) {
            return json{{"op", "sweep"},          {"label", e.label},     {"part", e.part},
                        {"origin", vec(e.origin)}, {"u", vec(e.u)},        {"v", vec(e.v)},
                        {"normal", vec(e.normal)}, {"polygon", polygons_to_json(e.polygon)}, {"distance", e.distance}};
          },
          [](const CutEdit& e) {
            return json{{"op", "cut"}, {"part", e.part}, {"axis", e.axis}, {"at", e.at}, {"keep", e.keep}, {"new_part", e.new_part}};
          },
          [](const MergeEdit& e) { return json{{"op", "merge"}, {"into", e.into}, {"parts", e.parts}}; },
          [](const MirrorEdit& e) {
            return json{{"op", "mirror"}, {"part", e.part}, {"axis", e.axis}, {"at", e.at}, {"copy", e.copy}, {"new_part", e.new_part}};
          },
          [](const TransformEdit& e) {
            return json{{"op", "transform"},        {"part", e.part},         {"translate", vec(e.translate)},
                        {"rotate_axis", e.rotate_axis}, {"rotate_deg", e.rotate_deg}, {"scale", e.scale},
                        {"pivot", vec(e.pivot)},     {"copy", e.copy},         {"new_part", e.new_part}};
          },
          [](const DeletePartEdit& e) { return json{{"op", "delete_part"}, {"part", e.part}}; },
          [](const SplitShellsEdit& e) { return json{{"op", "split_shells"}, {"part", e.part}, {"new_parts", e.new_parts}}; },
          [](const DepthStrokeEdit& e) {
            json s = json::array();
            for (const DepthSample& d : e.samples) s.push_back({d.u, d.v, d.offset});
            return json{{"op", "depth_stroke"}, {"view", to_string(e.view)}, {"part", e.part}, {"samples", s}};
          }},
      op);
}

Expected<EditOp> edit_from_json(const json& j) {
  try {
    if (!j.is_object()) return make_error("project.invalid", "an edit must be an object");
    const std::string op = j.at("op").get<std::string>();
    auto role = [&](const char* k) {
      const auto r = view_role_from_string(j.at(k).get<std::string>());
      if (!r) throw std::runtime_error("unknown view role");
      return *r;
    };
    if (op == "volume")
      return EditOp{VolumeEdit{j.value("label", "volume"), j.at("add").get<bool>(), role("view"),
                               polygons_from_json(j.at("region")), j.at("depth_from").get<double>(),
                               j.at("depth_to").get<double>(), j.at("part").get<int>(), j.value("new_part", -1)}};
    if (op == "sweep")
      return EditOp{SweepEdit{j.value("label", "push_pull"), j.at("part").get<int>(), vec_from(j.at("origin")),
                              vec_from(j.at("u")), vec_from(j.at("v")), vec_from(j.at("normal")),
                              polygons_from_json(j.at("polygon")), j.at("distance").get<double>()}};
    if (op == "cut")
      return EditOp{CutEdit{j.at("part").get<int>(), j.at("axis").get<int>(), j.at("at").get<double>(),
                            j.at("keep").get<int>(), j.value("new_part", -1)}};
    if (op == "merge") return EditOp{MergeEdit{j.at("into").get<int>(), j.at("parts").get<std::vector<int>>()}};
    if (op == "mirror")
      return EditOp{MirrorEdit{j.at("part").get<int>(), j.at("axis").get<int>(), j.at("at").get<double>(),
                               j.at("copy").get<bool>(), j.value("new_part", -1)}};
    if (op == "transform")
      return EditOp{TransformEdit{j.at("part").get<int>(), vec_from(j.at("translate")), j.at("rotate_axis").get<int>(),
                                  j.at("rotate_deg").get<double>(), j.at("scale").get<double>(), vec_from(j.at("pivot")),
                                  j.at("copy").get<bool>(), j.value("new_part", -1)}};
    if (op == "delete_part") return EditOp{DeletePartEdit{j.at("part").get<int>()}};
    if (op == "split_shells")
      return EditOp{SplitShellsEdit{j.at("part").get<int>(), j.at("new_parts").get<std::vector<int>>()}};
    if (op == "depth_stroke") {
      DepthStrokeEdit e;
      e.view = role("view");
      e.part = j.value("part", -1);
      for (const auto& s : j.at("samples")) {
        if (!s.is_array() || s.size() != 3) throw std::runtime_error("depth sample must be [u, v, offset]");
        e.samples.push_back({s[0].get<int>(), s[1].get<int>(), s[2].get<double>()});
      }
      return EditOp{e};
    }
    return make_error("project.invalid", "unknown edit op '" + op + "'");
  } catch (const std::exception& e) {
    return make_error("project.invalid", std::string("malformed edit: ") + e.what());
  }
}

std::optional<Message> check_edit(const EditOp& op) {
  auto bad = [](const std::string& t) { return std::optional<Message>(err("input.invalid", t)); };
  auto part_ok = [](int p) { return p >= -1 && p < (1 << 30); };
  auto new_ok = [](int p) { return p >= kFirstUserPart && p < (1 << 30); };
  return std::visit(
      overloaded{
          [&](const VolumeEdit& e) -> std::optional<Message> {
            if (auto m = check_region(e.region)) return m;
            if (!std::isfinite(e.depth_from) || !std::isfinite(e.depth_to) || std::abs(e.depth_from) > 1e6 ||
                std::abs(e.depth_to) > 1e6 || e.depth_to <= e.depth_from)
              return bad("depth_from < depth_to, both finite within ±1e6");
            if (!part_ok(e.part) || (e.add && e.part < 0 && !new_ok(e.new_part))) return bad("invalid part id");
            return std::nullopt;
          },
          [&](const SweepEdit& e) -> std::optional<Message> {
            if (auto m = check_region(e.polygon)) return m;
            if (!finite3(e.origin) || !finite3(e.u) || !finite3(e.v) || !finite3(e.normal)) return bad("non-finite plane");
            if (!std::isfinite(e.distance) || std::abs(e.distance) > 1e4) return bad("distance must be within ±1e4");
            if (!part_ok(e.part) || (e.distance > 0 && e.part < 0)) return bad("pull needs a part");
            return std::nullopt;
          },
          [&](const CutEdit& e) -> std::optional<Message> {
            if (e.axis < 0 || e.axis > 2 || !std::isfinite(e.at) || e.keep < -1 || e.keep > 1) return bad("invalid cut");
            if (!part_ok(e.part) || (e.part < 0 && e.keep == 0) || (e.keep == 0 && !new_ok(e.new_part)))
              return bad("invalid part id for cut");
            return std::nullopt;
          },
          [&](const MergeEdit& e) -> std::optional<Message> {
            if (e.into < 0 || e.parts.empty() || e.parts.size() > 10000) return bad("merge needs a target and parts");
            for (int p : e.parts)
              if (p < -1) return bad("invalid part id");
            return std::nullopt;
          },
          [&](const MirrorEdit& e) -> std::optional<Message> {
            if (e.axis < 0 || e.axis > 2 || !std::isfinite(e.at) || !part_ok(e.part)) return bad("invalid mirror");
            if (e.copy && (e.part < 0 || !new_ok(e.new_part))) return bad("a mirrored copy needs a part and a new id");
            return std::nullopt;
          },
          [&](const TransformEdit& e) -> std::optional<Message> {
            if (e.part < -1 || !finite3(e.translate) || !finite3(e.pivot) || !std::isfinite(e.rotate_deg) ||
                e.rotate_axis < 0 || e.rotate_axis > 2 || !std::isfinite(e.scale) || e.scale < 1e-3 || e.scale > 1e3)
              return bad("invalid transform");
            if (e.copy && !new_ok(e.new_part)) return bad("a duplicate needs a new id");
            return std::nullopt;
          },
          [&](const DeletePartEdit& e) -> std::optional<Message> {
            return e.part < -1 ? bad("invalid part id") : std::optional<Message>();
          },
          [&](const SplitShellsEdit& e) -> std::optional<Message> {
            if (e.part < -1 || e.new_parts.size() > 10000) return bad("invalid split");
            for (int p : e.new_parts)
              if (!new_ok(p)) return bad("invalid new part id");
            return std::nullopt;
          },
          [&](const DepthStrokeEdit& e) -> std::optional<Message> {
            if (e.samples.empty() || e.samples.size() > (1u << 20)) return bad("a depth stroke needs 1 to 2^20 samples");
            for (const DepthSample& s : e.samples)
              if (!std::isfinite(s.offset) || std::abs(s.offset) > 1e4) return bad("depth offsets must be within ±1e4");
            return part_ok(e.part) ? std::optional<Message>() : bad("invalid part id");
          }},
      op);
}

std::optional<Message> apply_edit(PartGeometry& g, const EditOp& op, Dims d, std::vector<Message>* notes) {
  PartGeometry work = g;  // all-or-nothing
  std::optional<Message> e = std::visit(overloaded{[&](const VolumeEdit& x) { return apply_volume(work, x, d); },
                                                   [&](const SweepEdit& x) { return apply_sweep(work, x); },
                                                   [&](const CutEdit& x) { return apply_cut(work, x); },
                                                   [&](const MergeEdit& x) { return apply_merge(work, x); },
                                                   [&](const MirrorEdit& x) { return apply_mirror(work, x); },
                                                   [&](const TransformEdit& x) { return apply_transform(work, x); },
                                                   [&](const DeletePartEdit& x) -> std::optional<Message> {
                                                     if (auto m = need_part(work, x.part)) return m;
                                                     work.parts.erase(x.part);
                                                     return std::nullopt;
                                                   },
                                                   [&](const SplitShellsEdit& x) { return apply_split_shells(work, x); },
                                                   [&](const DepthStrokeEdit& x) { return apply_depth_stroke(work, x, d, notes); }},
                                        op);
  if (e) return e;
  work.normalize();
  if (work.parts.empty()) return err("edit.empty_model", "the edit would remove the whole model");
  g = std::move(work);
  return std::nullopt;
}

}  // namespace pixel3d
