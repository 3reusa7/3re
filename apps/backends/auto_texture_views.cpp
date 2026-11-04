// Multi-angle pictures (spec 2026-10-21-multi-angle-design): registration of extra pictures around their
// labelled angle, the checks that reject a picture (outline fit, label, colour agreement), colour/exposure
// harmonisation to the front picture, and the per-point blend of every picture that sees the surface.
#include <algorithm>
#include <cmath>
#include <numbers>
#include "auto_texture_internal.hpp"

namespace p3dasset::autotex {
using namespace pixel3d;
using namespace detail;

bool label_yaw(const std::string& label, double& yaw) {
  static const std::pair<const char*, double> table[] = {{"front", 0},  {"front_left", 45}, {"left", 90},  {"back_left", 135},
                                                         {"back", 180}, {"back_right", 225}, {"right", 270}, {"front_right", 315}};
  for (const auto& [name, y] : table)
    if (label == name) { yaw = y; return true; }
  return false;
}

namespace {
double angular_distance(double y0, double e0, double y1, double e1) {
  const Pose a = Pose::make(y0, e0), b = Pose::make(y1, e1);
  return std::acos(std::clamp(dot(a.forward, b.forward), -1.0, 1.0)) * 180 / std::numbers::pi;
}
// Inner misalignment of a registered extra picture (px): median distance from the mesh's own depth edges in
// this view (parts overlapping: an arm in front of the body) to the nearest strong colour edge of the picture.
// The front picture made the geometry, so there it is a pixel or two; a side or back picture drawn of the same
// subject lands its inner edges where the real subject has them, which TRELLIS only guessed.
double inner_misalignment(const Mesh& m, const Source& s, const Pose& pose, nlohmann::json& ev) {
  const View v = View::build(m, s, pose);
  const int W = v.w, H = v.h;
  const double jump = 8 * v.pixel_world + 0.002 * v.diag;
  std::vector<float> pic(size_t(W) * H, 64.f);
  std::vector<std::array<float, 3>> lab(size_t(W) * H);
  for (size_t i = 0; i < lab.size(); ++i) lab[i] = oklab(s.px[i]);
  std::vector<float> g(size_t(W) * H, 0.f);
  std::vector<float> inside;
  for (int y = 1; y + 1 < H; ++y)
    for (int x = 1; x + 1 < W; ++x) {
      const size_t i = size_t(y) * W + x;
      if (s.px[i][3] < 0.5f) continue;
      double s2 = 0;
      for (int d = 0; d < 3; ++d) {
        const double gx = lab[i + 1][size_t(d)] - lab[i - 1][size_t(d)], gy = lab[i + W][size_t(d)] - lab[i - W][size_t(d)];
        s2 += gx * gx + gy * gy;
      }
      g[i] = float(std::sqrt(s2));
      inside.push_back(g[i]);
    }
  if (inside.size() < 400) return 0;
  std::nth_element(inside.begin(), inside.begin() + std::ptrdiff_t(inside.size() * 85 / 100), inside.end());
  const float strong = std::max(0.04f, inside[inside.size() * 85 / 100]);
  for (size_t i = 0; i < g.size(); ++i)
    if (g[i] >= strong) pic[i] = 0;
  chamfer(pic, W, H);
  std::vector<float> d;
  for (int y = 1; y + 1 < H; ++y)
    for (int x = 1; x + 1 < W; ++x) {
      const size_t i = size_t(y) * W + x;
      const float z = v.depth[i];
      if (!std::isfinite(z) || s.px[i][3] < 0.5f) continue;
      bool edge = false;
      for (size_t j : {i + 1, i - 1, i + size_t(W), i - size_t(W)}) {
        const float n = v.depth[j];
        if (std::isfinite(n) && n - z > jump) { edge = true; break; }  // z is the near side of an inner depth edge
      }
      if (edge) d.push_back(pic[i]);
    }
  ev = {{"inner_depth_edge_pixels", d.size()}};
  if (d.size() < 50) return 0;
  std::nth_element(d.begin(), d.begin() + std::ptrdiff_t(d.size() / 2), d.end());
  ev["median_px"] = d[d.size() / 2];
  return d[d.size() / 2];
}

// Relative sharpness of a picture: mean |Laplacian| over mean |gradient| of Oklab L inside the subject. A
// blurred (upscaled, out of focus) picture has little curvature for its edges.
double sharpness(const Source& s) {
  double lap = 0, grad = 0;
  size_t n = 0;
  auto L = [&](int x, int y) { return double(oklab(s.px[size_t(y) * s.width + x])[0]); };
  for (int y = 2; y + 2 < s.height; y += 2)
    for (int x = 2; x + 2 < s.width; x += 2) {
      if (s.alpha(x, y) < 0.9f || s.alpha(x - 2, y) < 0.9f || s.alpha(x + 2, y) < 0.9f || s.alpha(x, y - 2) < 0.9f || s.alpha(x, y + 2) < 0.9f) continue;
      const double c = L(x, y);
      lap += std::abs(4 * c - L(x - 1, y) - L(x + 1, y) - L(x, y - 1) - L(x, y + 1));
      grad += std::abs(L(x + 1, y) - L(x - 1, y)) + std::abs(L(x, y + 1) - L(x, y - 1));
      ++n;
    }
  return n > 100 && grad > 1e-9 ? lap / grad : 0;
}
}  // namespace

std::array<float, 4> Pictures::harmonise(size_t k, const std::array<float, 4>& c) const {
  if (k == 0) return c;
  const auto& q = p[k];
  auto lab = oklab(c);
  lab[0] = float(std::clamp(q.gain[0] * lab[0] + q.offset[0], 0.0, 1.0));
  lab[1] = float(q.gain[1] * lab[1] + q.offset[1]);
  lab[2] = float(q.gain[2] * lab[2] + q.offset[2]);
  auto out = linear_from_oklab(lab);
  out[3] = c[3];
  return out;
}

Pictures Pictures::make(const Mesh& m, const Source& front, const Registration& reg, const std::vector<ExtraView>* extra) {
  Pictures out;
  out.p.push_back({&front, reg.pose, View::build(m, front, reg.pose), {1, 1, 1}, {0, 0, 0}, 1, "front", 0});
  if (extra)
    for (const auto& e : *extra)
      out.p.push_back({e.source, e.reg.pose, View::build(m, *e.source, e.reg.pose), e.gain, e.offset, e.weight, e.label, e.edge_px});
  return out;
}

Pictures::Combined Pictures::observe(vec3d pos, vec3d n, vec3d fn, double c0, double c1, const double* graze, bool sharp) const {
  Combined c;
  if (p.size() == 1) {  // the front alone: exactly its own observation
    c.o = p[0].view.observe(*p[0].src, p[0].pose, pos, n, fn, c0, c1);
    if (graze && c.o.conf > 0) c.o.conf *= float(smoothstep(graze[0], graze[1], std::min(double(c.o.facing), dot(n, -p[0].pose.forward))));
    if (c.o.conf > 0) c.dominant = 0;
    if (sharp && c.o.conf > 0) {
      const vec2d q = p[0].pose.to_image(pos);
      c.sharp = p[0].src->sample_sharp(q.x, q.y);
    } else c.sharp = c.o.rgb;
    return c;
  }
  double acc[3]{}, acs[3]{}, wsum = 0, wbest = 0;
  const double ref_scale = std::max(p[0].pose.scale, 1e-12);
  for (size_t k = 0; k < p.size(); ++k) {
    auto o = p[k].view.observe(*p[k].src, p[k].pose, pos, n, fn, c0, c1);
    if (o.stage > c.o.stage) c.o.stage = o.stage;
    if (o.conf <= 0) continue;
    if (graze) o.conf *= float(smoothstep(graze[0], graze[1], std::min(double(o.facing), dot(n, -p[k].pose.forward))));
    const vec2d q = p[k].pose.to_image(pos);
    const int ix = std::clamp(int(q.x), 0, p[k].view.w - 1), iy = std::clamp(int(q.y), 0, p[k].view.h - 1);
    const double edge = p[k].view.edge[size_t(iy) * p[k].view.w + ix];
    if (p[k].edge_px > 0) o.conf *= float(smoothstep(0.5 * p[k].edge_px, p[k].edge_px, edge));
    if (o.conf <= 0) continue;
    // Resolution on the surface (picture pixels per unit along the slope), sharpness, and distance from the
    // outline: a picture that sees the point squarely and sharply, away from its edges, dominates.
    const double res = std::clamp(p[k].pose.scale * std::max(0.05, double(o.facing)) / ref_scale, 0.05, 4.0);
    const double w = double(o.conf) * o.conf * res * p[k].weight * (0.15 + 0.85 * smoothstep(0.5, 8.0, edge));
    const auto rgb = harmonise(k, o.rgb);
    for (int d = 0; d < 3; ++d) acc[d] += rgb[size_t(d)] * w;
    if (sharp) {
      const auto s = harmonise(k, p[k].src->sample_sharp(q.x, q.y));
      for (int d = 0; d < 3; ++d) acs[d] += s[size_t(d)] * w;
    }
    wsum += w;
    if (k > 0) c.best_other = std::max(c.best_other, o.conf);
    if (w > wbest) {
      wbest = w;
      c.dominant = int(k);
      const float keep_conf = std::max(c.o.conf, o.conf);
      const uint8_t st = c.o.stage;
      c.o = o;
      c.o.conf = keep_conf;
      c.o.stage = std::max(st, o.stage);
    } else c.o.conf = std::max(c.o.conf, o.conf);
  }
  if (wsum <= 0) {
    c.o.conf = 0;
    c.dominant = -1;
    return c;
  }
  for (int d = 0; d < 3; ++d) {
    c.o.rgb[size_t(d)] = float(acc[d] / wsum);
    c.sharp[size_t(d)] = float((sharp ? acs[d] : acc[d]) / wsum);
  }
  c.sharp[3] = c.o.rgb[3];
  return c;
}

std::vector<ExtraView> register_extra_views(const Mesh& mesh, const Source& front, const Registration& front_reg,
                                            const std::vector<ExtraInput>& inputs, const Deadline& deadline, nlohmann::json& report) {
  report = nlohmann::json::array();
  struct Cand { ExtraView v; double yaw_rel = 0; nlohmann::json ev; };
  std::vector<Cand> cands;
  const double front_sharp = sharpness(front);
  for (const auto& in : inputs) {
    nlohmann::json ev{{"name", in.name}, {"label", in.label}};
    double rel = in.yaw;
    const bool automatic = in.label == "auto" && !in.yaw_given;
    if (!in.yaw_given && !automatic && !label_yaw(in.label, rel)) {
      ev["status"] = "rejected";
      ev["reason"] = "unknown angle label '" + in.label + "'";
      report.push_back(ev);
      continue;
    }
    if (!in.source) {
      ev["status"] = "rejected";
      ev["reason"] = "the picture could not be read";
      report.push_back(ev);
      continue;
    }
    RegisterPrior prior;
    prior.yaw = std::fmod(front_reg.pose.yaw + rel + 720, 360);
    prior.elevation = front_reg.pose.elevation;
    prior.max_drift = 30;
    prior.keep_prior = true;
    Registration reg;
    try {
      if (automatic) {  // "auto": the best outline fit anywhere, then refined there like a labelled picture
        const auto any = register_source(mesh, *in.source, deadline);
        prior.yaw = any.evidence["global_best"].value("yaw", any.pose.yaw);
        prior.elevation = any.evidence["global_best"].value("elevation", any.pose.elevation);
        prior.max_drift = 8;
      }
      reg = register_source(mesh, *in.source, deadline, &prior);
    } catch (const std::exception& e) {
      ev["status"] = "rejected";
      ev["reason"] = std::string("its outline could not be matched to the model (") + e.what() + ")";
      report.push_back(ev);
      continue;
    }
    if (automatic) rel = std::fmod(reg.pose.yaw - front_reg.pose.yaw + 720, 360);
    const auto& gb = reg.evidence["global_best"];
    const double gy = gb.value("yaw", reg.pose.yaw), ge = gb.value("elevation", reg.pose.elevation), giou = gb.value("iou", reg.iou);
    ev["fit"] = reg.iou;
    ev["yaw_from_front"] = std::fmod(reg.pose.yaw - front_reg.pose.yaw + 720, 360);
    ev["elevation"] = reg.pose.elevation;
    ev["labelled_yaw_from_front"] = rel;
    ev["best_fit_anywhere"] = {{"yaw_from_front", std::fmod(gy - front_reg.pose.yaw + 720, 360)}, {"elevation", ge}, {"fit", giou}};
    ev["registration"] = reg.evidence;
    std::string reason;
    if (!reg.silhouette_usable) reason = "it has no transparent background, so its outline cannot be matched to the model";
    else if (reg.iou < 0.55)
      reason = "its outline does not match the model at the labelled angle (fit " + std::to_string(int(std::lround(reg.iou * 100))) + "%)";
    else if (giou > reg.iou + 0.06 && angular_distance(gy, ge, reg.pose.yaw, reg.pose.elevation) > 40)
      reason = "it fits the model much better at " + std::to_string(int(std::lround(std::fmod(gy - front_reg.pose.yaw + 720, 360)))) +
               " degrees from the front (fit " + std::to_string(int(std::lround(giou * 100))) + "%) than at its label (" +
               std::to_string(int(std::lround(reg.iou * 100))) + "%): wrong angle label?";
    if (!reason.empty()) {
      ev["status"] = "rejected";
      ev["reason"] = reason;
      report.push_back(ev);
      continue;
    }
    Cand c;
    c.v.name = in.name;
    c.v.label = in.label;
    if (automatic || in.label == "custom") {  // name the angle it was found at (nearest of the eight)
      static const char* names[8] = {"front", "front_left", "left", "back_left", "back", "back_right", "right", "front_right"};
      c.v.label = names[int(std::lround(std::fmod(rel + 720, 360) / 45)) % 8];
      ev["resolved_label"] = c.v.label;
    }
    c.v.source = in.source;
    c.v.reg = reg;
    const double s = sharpness(*in.source);
    c.v.weight = front_sharp > 0 && s > 0 ? std::clamp(s / front_sharp, 0.5, 1.5) : 1.0;
    ev["sharpness_weight"] = c.v.weight;
    {
      // Keep-away distance from depth edges: three times the measured inner misalignment above the front's own
      // (the front made the geometry), at least 6 px, at most 6% of the subject's height in the picture.
      nlohmann::json me, mf;
      const double mis = inner_misalignment(mesh, *in.source, reg.pose, me);
      const double base = inner_misalignment(mesh, front, front_reg.pose, mf);
      int y0 = in.source->height, y1 = -1;
      for (int y = 0; y < in.source->height; ++y)
        for (int x = 0; x < in.source->width; ++x)
          if (in.source->alpha(x, y) >= 0.5f) { y0 = std::min(y0, y); y1 = std::max(y1, y); break; }
      const double cap = std::max(6.0, 0.06 * std::max(1, y1 - y0 + 1));
      c.v.edge_px = std::clamp(6.0 + 3.0 * std::max(0.0, mis - base), 6.0, cap);
      ev["inner_misalignment"] = {{"picture", me}, {"front_reference", mf}, {"keep_away_px", c.v.edge_px}};
    }
    c.yaw_rel = rel;
    c.ev = ev;
    cands.push_back(std::move(c));
    deadline.check("extra picture registration");
  }
  // Harmonisation: each picture is fitted to the pictures already accepted (the front first), in order of how
  // much surface it shares with them, then its residual on the shared surface decides whether it agrees.
  std::vector<ExtraView> accepted;
  const auto normals = vertex_normals(mesh);
  const size_t nv = mesh.positions.size();
  const size_t step = std::max<size_t>(1, nv / 60000);
  std::vector<uint8_t> done(cands.size(), 0);
  for (size_t round = 0; round < cands.size(); ++round) {
    const Pictures ref = Pictures::make(mesh, front, front_reg, &accepted);
    const double g[2] = {0.4, 0.75};
    // Reference colour per sampled vertex (only where the accepted pictures see it confidently).
    std::vector<std::array<float, 3>> ref_lab(nv);
    std::vector<uint8_t> ref_ok(nv, 0);
    for (size_t i = 0; i < nv; i += step) {
      const auto o = ref.observe(mesh.positions[i], normals[i], normals[i], 0.12, 0.5, g, false);
      if (o.o.conf >= 0.5f) { ref_lab[i] = oklab(o.o.rgb); ref_ok[i] = 1; }
    }
    size_t pick = cands.size(), best_overlap = 0;
    std::vector<std::vector<std::pair<std::array<float, 3>, std::array<float, 3>>>> pairs(cands.size());
    for (size_t c = 0; c < cands.size(); ++c) {
      if (done[c]) continue;
      const View v = View::build(mesh, *cands[c].v.source, cands[c].v.reg.pose);
      for (size_t i = 0; i < nv; i += step) {
        if (!ref_ok[i]) continue;
        auto o = v.observe(*cands[c].v.source, cands[c].v.reg.pose, mesh.positions[i], normals[i], normals[i], 0.12, 0.5);
        if (o.conf <= 0) continue;
        o.conf *= float(smoothstep(g[0], g[1], std::min(double(o.facing), dot(normals[i], -cands[c].v.reg.pose.forward))));
        const vec2d q = cands[c].v.reg.pose.to_image(mesh.positions[i]);
        const double e = v.edge[size_t(std::clamp(int(q.y), 0, v.h - 1)) * v.w + size_t(std::clamp(int(q.x), 0, v.w - 1))];
        o.conf *= float(smoothstep(0.5 * cands[c].v.edge_px, cands[c].v.edge_px, e));
        if (o.conf >= 0.5f) pairs[c].push_back({oklab(o.rgb), ref_lab[i]});
      }
      if (pick == cands.size() || pairs[c].size() > best_overlap) { pick = c; best_overlap = pairs[c].size(); }
    }
    if (pick == cands.size()) break;
    done[pick] = 1;
    auto& cand = cands[pick];
    auto& pr = pairs[pick];
    nlohmann::json h{{"shared_surface_samples", pr.size()}};
    if (pr.size() >= 64) {
      double mp[3]{}, mr[3]{}, vp = 0, vr = 0;
      for (const auto& [a, b] : pr)
        for (int d = 0; d < 3; ++d) { mp[d] += a[size_t(d)]; mr[d] += b[size_t(d)]; }
      for (int d = 0; d < 3; ++d) { mp[d] /= double(pr.size()); mr[d] /= double(pr.size()); }
      for (const auto& [a, b] : pr) { vp += (a[0] - mp[0]) * (a[0] - mp[0]); vr += (b[0] - mr[0]) * (b[0] - mr[0]); }
      const double gl = vp > 1e-9 ? std::clamp(std::sqrt(vr / vp), 0.8, 1.25) : 1.0;
      cand.v.gain = {gl, 1, 1};
      cand.v.offset = {std::clamp(mr[0] - gl * mp[0], -0.12, 0.12), std::clamp(mr[1] - mp[1], -0.05, 0.05), std::clamp(mr[2] - mp[2], -0.05, 0.05)};
      std::vector<double> before, after;
      for (const auto& [a, b] : pr) {
        auto de = [](const std::array<float, 3>& x, const std::array<float, 3>& y) {
          return std::sqrt(double(x[0] - y[0]) * (x[0] - y[0]) + double(x[1] - y[1]) * (x[1] - y[1]) + double(x[2] - y[2]) * (x[2] - y[2]));
        };
        before.push_back(de(a, b));
        const std::array<float, 3> c{float(gl * a[0] + cand.v.offset[0]), float(a[1] + cand.v.offset[1]), float(a[2] + cand.v.offset[2])};
        after.push_back(de(c, b));
      }
      auto median = [](std::vector<double> v) {
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        return v[v.size() / 2];
      };
      const double mb = median(before), ma = median(after);
      h["method"] = "Oklab L gain/offset + a/b offset, fitted on the shared surface";
      h["gain_l"] = gl;
      h["offset"] = {cand.v.offset[0], cand.v.offset[1], cand.v.offset[2]};
      h["median_delta_e_before"] = mb;
      h["median_delta_e_after"] = ma;
      if (pr.size() >= 200 && ma > 0.12) {
        cand.ev["status"] = "rejected";
        cand.ev["harmonisation"] = h;
        cand.ev["reason"] = "its colours disagree with the other pictures where they show the same surface (median difference " +
                            std::to_string(int(std::lround(ma * 100))) + " after exposure matching): wrong angle or a different object?";
        report.push_back(cand.ev);
        continue;
      }
    } else {
      h["method"] = "none: too little surface shared with the other pictures to match colours";
    }
    cand.ev["harmonisation"] = h;
    cand.ev["status"] = "accepted";
    report.push_back(cand.ev);
    cand.v.evidence = cand.ev;
    accepted.push_back(cand.v);
    deadline.check("extra picture harmonisation");
  }
  return accepted;
}
}  // namespace p3dasset::autotex
