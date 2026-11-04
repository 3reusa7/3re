// Source-camera registration: which view of the generated mesh does the picture show?
// TRELLIS does not keep the picture's viewpoint (vehicles, for example, come out turned), so the
// camera is found by matching the mesh silhouette against the picture's alpha mask.
#include "auto_texture.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>

namespace p3dasset::autotex {
using namespace pixel3d;
namespace {
struct MaskGrid {
  int w = 0, h = 0;
  std::vector<float> a;
  double total = 0;
  float at(double x, double y) const {  // bilinear, 0 outside
    x -= 0.5;
    y -= 0.5;
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    double s = 0;
    for (int dy = 0; dy < 2; ++dy)
      for (int dx = 0; dx < 2; ++dx) {
        const int sx = ix + dx, sy = iy + dy;
        if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
        s += a[size_t(sy) * w + sx] * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
      }
    return float(s);
  }
};
struct Silhouette {
  std::vector<vec2d> px;  // covered pixel centres (grid units)
  vec2d lo{1e30, 1e30}, hi{-1e30, -1e30}, centroid;
};
struct Fit {
  double iou = -1, k = 1;
  vec2d t;
};
struct Candidate {
  double yaw = 0, el = 0;
  Fit fit;
};

Silhouette silhouette(const Mesh& m, const Pose& p, vec3d centre, double grid_scale, int G) {
  std::vector<uint8_t> cov(size_t(G) * G, 0);
  std::vector<vec2d> s(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const vec3d d = m.positions[i] - centre;
    s[i] = {dot(d, p.right) * grid_scale + G / 2.0, -dot(d, p.up) * grid_scale + G / 2.0};
  }
  for (const auto& t : m.tris) {
    const vec2d a = s[t[0]], b = s[t[1]], c = s[t[2]];
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area) < 1e-14) continue;
    const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}) - 0.5))), x1 = std::min(G - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}) - 0.5))), y1 = std::min(G - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) / area;
        const double w1 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) / area;
        if (w0 < -1e-9 || w1 < -1e-9 || 1 - w0 - w1 < -1e-9) continue;
        cov[size_t(y) * G + x] = 1;
      }
  }
  Silhouette out;
  vec2d sum{};
  for (int y = 0; y < G; ++y)
    for (int x = 0; x < G; ++x)
      if (cov[size_t(y) * G + x]) {
        const vec2d q{x + 0.5, y + 0.5};
        out.px.push_back(q);
        sum = sum + q;
        out.lo = {std::min(out.lo.x, q.x), std::min(out.lo.y, q.y)};
        out.hi = {std::max(out.hi.x, q.x), std::max(out.hi.y, q.y)};
      }
  if (!out.px.empty()) out.centroid = sum * (1.0 / double(out.px.size()));
  return out;
}

double iou(const Silhouette& s, const MaskGrid& M, double k, vec2d t) {
  double inter = 0;
  for (auto q : s.px) inter += M.at(k * q.x + t.x, k * q.y + t.y);
  inter *= k * k;
  const double uni = double(s.px.size()) * k * k + M.total - inter;
  return uni > 0 ? inter / uni : 0;
}

// Uniform scale + translation that best maps the silhouette onto the mask.
Fit align(const Silhouette& s, const MaskGrid& M, vec2d mask_centroid, vec2d mask_lo, vec2d mask_hi) {
  Fit best;
  if (s.px.size() < 8) return best;
  std::vector<std::pair<double, vec2d>> inits;
  const double k_area = std::sqrt(M.total / double(s.px.size()));
  inits.push_back({k_area, mask_centroid - s.centroid * k_area});
  const double sw = std::max(1.0, s.hi.x - s.lo.x + 1), sh = std::max(1.0, s.hi.y - s.lo.y + 1);
  const vec2d mc = (mask_lo + mask_hi) * 0.5, sc = (s.lo + s.hi) * 0.5;
  for (double k : {(mask_hi.x - mask_lo.x + 1) / sw, (mask_hi.y - mask_lo.y + 1) / sh}) inits.push_back({k, mc - sc * k});
  for (auto [k, t] : inits) {
    const double v = iou(s, M, k, t);
    if (v > best.iou) best = {v, k, t};
  }
  double dk = 0.06, dt = 3.0;
  int evals = 0;
  while ((dk > 0.004 || dt > 0.25) && evals < 120) {
    bool moved = false;
    const std::array<std::pair<double, vec2d>, 6> steps{{{1 + dk, {}}, {1 / (1 + dk), {}}, {1, {dt, 0}}, {1, {-dt, 0}}, {1, {0, dt}}, {1, {0, -dt}}}};
    for (auto [fk, ft] : steps) {
      // Scale about the silhouette centroid so translation and scale stay decoupled.
      const double k = best.k * fk;
      const vec2d c = s.centroid * best.k + best.t;
      const vec2d t = c - s.centroid * k + ft;
      const double v = iou(s, M, k, t);
      ++evals;
      if (v > best.iou + 1e-7) {
        best = {v, k, t};
        moved = true;
      }
    }
    if (!moved) {
      dk *= 0.5;
      dt *= 0.5;
    }
  }
  return best;
}

double angular_distance(double y0, double e0, double y1, double e1) {
  const Pose a = Pose::make(y0, e0), b = Pose::make(y1, e1);
  return std::acos(std::clamp(dot(a.forward, b.forward), -1.0, 1.0)) * 180 / std::numbers::pi;
}
}  // namespace

Registration register_source(const Mesh& mesh, const Source& src, const Deadline& deadline, const RegisterPrior* prior_in) {
  const RegisterPrior prior = prior_in ? *prior_in : RegisterPrior{};
  const auto started = std::chrono::steady_clock::now();
  Registration reg;
  vec3d lo = mesh.positions.front(), hi = lo;
  for (auto p : mesh.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const vec3d centre = (lo + hi) * 0.5;
  const double radius = std::max(length(hi - lo) / 2, 1e-9);
  // Source mask statistics.
  int bx0 = src.width, by0 = src.height, bx1 = -1, by1 = -1;
  size_t opaque = 0;
  for (int y = 0; y < src.height; ++y)
    for (int x = 0; x < src.width; ++x)
      if (src.alpha(x, y) >= 0.5f) {
        ++opaque;
        bx0 = std::min(bx0, x);
        by0 = std::min(by0, y);
        bx1 = std::max(bx1, x);
        by1 = std::max(by1, y);
      }
  if (bx1 < 0) throw std::runtime_error("The source picture has no opaque subject");
  const double bbox_px = double(bx1 - bx0 + 1) * (by1 - by0 + 1);
  const double rectangularity = opaque / bbox_px, transparent = 1 - double(opaque) / (double(src.width) * src.height);
  reg.silhouette_usable = transparent > 0.02 && rectangularity < 0.93 && opaque >= 256;
  // Working mask: subject bbox resampled so its long side is ~220 px (area-averaged alpha).
  const double msc = 220.0 / std::max(bx1 - bx0 + 1, by1 - by0 + 1);
  MaskGrid M;
  M.w = std::max(1, int(std::ceil((bx1 - bx0 + 1) * msc)));
  M.h = std::max(1, int(std::ceil((by1 - by0 + 1) * msc)));
  M.a.assign(size_t(M.w) * M.h, 0);
  {
    std::vector<double> wsum(M.a.size(), 0);
    for (int y = by0; y <= by1; ++y)
      for (int x = bx0; x <= bx1; ++x) {
        const int mx = std::min(M.w - 1, int((x - bx0 + 0.5) * msc)), my = std::min(M.h - 1, int((y - by0 + 0.5) * msc));
        M.a[size_t(my) * M.w + mx] += src.alpha(x, y) >= 0.5f ? 1.f : 0.f;
        wsum[size_t(my) * M.w + mx] += 1;
      }
    for (size_t i = 0; i < M.a.size(); ++i) M.a[i] = wsum[i] > 0 ? float(M.a[i] / wsum[i]) : 0.f;
  }
  vec2d mcen{};
  for (int y = 0; y < M.h; ++y)
    for (int x = 0; x < M.w; ++x) {
      const double v = M.a[size_t(y) * M.w + x];
      M.total += v;
      mcen = mcen + vec2d{x + 0.5, y + 0.5} * v;
    }
  mcen = mcen * (1.0 / std::max(M.total, 1e-9));
  const vec2d mlo{0, 0}, mhi{double(M.w - 1), double(M.h - 1)};

  auto evaluate_pose = [&](double yaw, double el, int G) {
    const Pose p = Pose::make(yaw, el);
    const double gs = G * 0.48 / radius;
    const auto s = silhouette(mesh, p, centre, gs, G);
    Candidate c{yaw, el, align(s, M, mcen, mlo, mhi)};
    return c;
  };
  const int coarse_g = 160, fine_g = 240;
  std::vector<Candidate> coarse;
  std::vector<std::pair<double, double>> grid;
  for (double el : {-45.0, -30.0, -15.0, 0.0, 15.0, 30.0, 45.0, 60.0, 75.0})
    for (int yaw = 0; yaw < 360; yaw += 15) grid.push_back({double(yaw), el});
  coarse.resize(grid.size());
  if (reg.silhouette_usable) {
    std::atomic<size_t> next{0};
    const unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1u, 6u);
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; ++w)
      pool.emplace_back([&] {
        for (size_t i; (i = next.fetch_add(1)) < grid.size();) coarse[i] = evaluate_pose(grid[i].first, grid[i].second, coarse_g);
      });
    for (auto& t : pool) t.join();
    deadline.check("camera registration");
  }
  // Refine the best distinct coarse candidates plus the TRELLIS front prior by hill climbing.
  std::vector<Candidate> seeds;
  if (reg.silhouette_usable) {
    auto sorted = coarse;
    std::sort(sorted.begin(), sorted.end(), [](const Candidate& a, const Candidate& b) { return a.fit.iou > b.fit.iou; });
    for (const auto& c : sorted) {
      bool distinct = true;
      for (const auto& s : seeds) distinct &= angular_distance(c.yaw, c.el, s.yaw, s.el) > 25;
      if (distinct) seeds.push_back(c);
      if (seeds.size() == 4) break;
    }
  }
  const Candidate front_exact = evaluate_pose(prior.yaw, prior.elevation, fine_g);
  reg.front_iou = front_exact.fit.iou;
  // max_drift > 0: refinement around a prior, with a small cost per degree so that a slightly better
  // fit of a mis-proportioned shape does not pull the camera off the prior.
  auto refine = [&](Candidate c, double max_drift, double cy, double ce) {
    c = evaluate_pose(c.yaw, c.el, fine_g);
    auto objective = [&](const Candidate& x) { return x.fit.iou - (max_drift > 0 ? 0.0015 * angular_distance(x.yaw, x.el, cy, ce) : 0.0); };
    for (double step : {6.0, 3.0, 1.5, 0.75}) {
      for (bool moved = true; moved;) {
        moved = false;
        for (auto [dy, de] : std::array<std::pair<double, double>, 4>{{{step, 0}, {-step, 0}, {0, step}, {0, -step}}}) {
          const double y = c.yaw + dy, e = std::clamp(c.el + de, -80.0, 85.0);
          if (max_drift > 0 && angular_distance(y, e, cy, ce) > max_drift) continue;
          const auto n = evaluate_pose(y, e, fine_g);
          if (objective(n) > objective(c) + 1e-5) {
            c = n;
            moved = true;
          }
        }
        if (deadline.remaining() < 0) break;
      }
    }
    c.yaw = std::fmod(std::fmod(c.yaw, 360) + 360, 360);
    return c;
  };
  std::vector<Candidate> refined;
  for (const auto& s : seeds) refined.push_back(refine(s, 0, 0, 0));
  // An extra picture's label may be off by a coarse step: a wider start around it.
  Candidate front = front_exact;
  if (reg.silhouette_usable && prior.max_drift > 20)
    for (double dy : {-prior.max_drift * 0.6, prior.max_drift * 0.6}) {
      const auto c = evaluate_pose(prior.yaw + dy, prior.elevation, fine_g);
      if (c.fit.iou > front.fit.iou) front = c;
    }
  if (reg.silhouette_usable) front = refine(front, prior.max_drift, prior.yaw, prior.elevation);
  Candidate best = front;
  for (const auto& c : refined)
    if (c.fit.iou > best.fit.iou) best = c;
  const Candidate global_best = best;
  std::string choice = "best_silhouette";
  // TRELLIS usually keeps the picture as the front: keep the prior unless another view is clearly better.
  if (!reg.silhouette_usable) choice = "front_prior_no_transparency";
  else if (prior.keep_prior) {
    best = front;
    choice = "labelled_angle";
  } else if (front.fit.iou >= best.fit.iou - 0.02) {
    best = front;
    choice = "front_prior";
  }
  for (const auto& c : refined)
    if (angular_distance(c.yaw, c.el, best.yaw, best.el) > 30 && c.fit.iou >= best.fit.iou - 0.02) reg.ambiguous = true;
  // Compose the world → source-pixel mapping.
  const int G = fine_g;
  const double gs = G * 0.48 / radius;
  Pose p = Pose::make(best.yaw, best.el);
  const double k = best.fit.k;
  p.scale = k * gs / msc;
  p.offset = {bx0 + (k * (-dot(centre, p.right) * gs + G / 2.0) + best.fit.t.x) / msc,
              by0 + (k * (dot(centre, p.up) * gs + G / 2.0) + best.fit.t.y) / msc};
  reg.pose = p;
  reg.iou = best.fit.iou;
  nlohmann::json top = nlohmann::json::array();
  for (const auto& c : refined) top.push_back({{"yaw", c.yaw}, {"elevation", c.el}, {"iou", c.fit.iou}});
  reg.evidence = {{"method", "orthographic silhouette search: yaw 0-345/15 x elevation -45..75/15, hill-climb to 0.75 deg, uniform scale + translation"},
                  {"choice", choice}, {"yaw", best.yaw}, {"elevation", best.el}, {"iou", best.fit.iou},
                  {"front_iou", reg.front_iou}, {"front_refined", {{"yaw", front.yaw}, {"elevation", front.el}, {"iou", front.fit.iou}}},
                  {"candidates", top}, {"ambiguous", reg.ambiguous}, {"silhouette_usable", reg.silhouette_usable},
                  {"prior", {{"yaw", prior.yaw}, {"elevation", prior.elevation}, {"max_drift", prior.max_drift}, {"keep_prior", prior.keep_prior}}},
                  {"global_best", {{"yaw", global_best.yaw}, {"elevation", global_best.el}, {"iou", global_best.fit.iou}}},
                  {"source_transparent_fraction", transparent}, {"subject_rectangularity", rectangularity},
                  {"pixels_per_unit", p.scale}, {"perspective", "orthographic approximation"},
                  {"milliseconds", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count()}};
  return reg;
}
}  // namespace p3dasset::autotex
