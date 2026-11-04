// Planar-chart UV atlas: bounded, deterministic and fast on dense generated meshes (xatlas can take
// minutes or stall on 150k+ noisy TRELLIS triangles). Charts grow over edge-adjacent faces whose
// normals stay inside a cone around the seed normal and are projected orthographically onto that
// plane, so stretch is bounded by 1/cos(cone) and orientation is always consistent. A face whose
// projection would overlap the chart's existing faces (a fold) starts another chart instead.
// Charts are rotated to their principal axes and shelf-packed at the largest texel density that fits.
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <queue>
#include <unordered_map>
#include "auto_texture_internal.hpp"

namespace p3dasset::autotex {
using namespace pixel3d;
using namespace detail;
namespace {
struct Tri2 { vec2d p[3]; };
bool overlap(const Tri2& a, const Tri2& b) {
  auto shrink = [](const Tri2& t) {
    const vec2d c = (t.p[0] + t.p[1] + t.p[2]) * (1.0 / 3);
    Tri2 o;
    for (int i = 0; i < 3; ++i) o.p[i] = c + (t.p[i] - c) * 0.98;
    return o;
  };
  const Tri2 A = shrink(a), B = shrink(b);
  for (const Tri2* t : {&A, &B})
    for (int e = 0; e < 3; ++e) {
      const vec2d d = t->p[(e + 1) % 3] - t->p[e];
      const vec2d n{-d.y, d.x};
      double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
      for (int i = 0; i < 3; ++i) {
        const double x = dot(A.p[i], n), y = dot(B.p[i], n);
        a0 = std::min(a0, x); a1 = std::max(a1, x);
        b0 = std::min(b0, y); b1 = std::max(b1, y);
      }
      if (a1 <= b0 || b1 <= a0) return false;
    }
  return true;
}
}  // namespace

Atlas detail::build_planar_atlas(const Mesh& m, int size, double cone_deg, const Deadline& deadline, const std::vector<float>* face_density) {
  const auto started = std::chrono::steady_clock::now();
  const size_t nf = m.tris.size();
  std::vector<vec3d> fn(nf);
  std::vector<double> fa(nf);
  double total_area = 0, edge_sum = 0;
  for (size_t f = 0; f < nf; ++f) {
    const auto& t = m.tris[f];
    const vec3d c = cross(m.positions[t[1]] - m.positions[t[0]], m.positions[t[2]] - m.positions[t[0]]);
    const double l = length(c);
    fn[f] = l > 0 ? c / l : vec3d{0, 0, 1};
    fa[f] = l / 2;
    total_area += fa[f];
    edge_sum += length(m.positions[t[1]] - m.positions[t[0]]);
  }
  // Edge adjacency.
  std::vector<std::pair<uint64_t, uint32_t>> edges;
  edges.reserve(nf * 3);
  for (size_t f = 0; f < nf; ++f)
    for (int k = 0; k < 3; ++k) {
      const uint64_t a = m.tris[f][size_t(k)], b = m.tris[f][size_t((k + 1) % 3)];
      edges.push_back({std::min(a, b) << 32 | std::max(a, b), uint32_t(f)});
    }
  std::sort(edges.begin(), edges.end());
  std::vector<std::array<int32_t, 3>> nbr(nf, {-1, -1, -1});
  std::vector<uint8_t> nn(nf, 0);
  for (size_t i = 0; i + 1 < edges.size(); ++i)
    if (edges[i].first == edges[i + 1].first) {
      const auto f = edges[i].second, g = edges[i + 1].second;
      if (nn[f] < 3) nbr[f][nn[f]++] = int32_t(g);
      if (nn[g] < 3) nbr[g][nn[g]++] = int32_t(f);
    }
  edges.clear();
  edges.shrink_to_fit();
  deadline.check("UV charting");
  const double cone = std::cos(cone_deg * std::numbers::pi / 180);
  const double cell = std::max(2.0 * edge_sum / double(std::max<size_t>(nf, 1)), 1e-12);
  std::vector<int32_t> chart_of(nf, -1);
  struct Chart { vec3d axis, u, v; std::vector<uint32_t> faces; };
  std::vector<Chart> charts;
  std::unordered_map<uint64_t, std::vector<uint32_t>> grid;  // (chart, cell) -> faces, for the fold test
  auto cell_key = [](uint64_t chart, int64_t x, int64_t y) {
    return (chart * 0x9E3779B97F4A7C15ull) ^ (uint64_t(x) * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t(y) * 0x165667B19E3779F9ull);
  };
  auto project = [&](const Chart& c, uint32_t f) {
    Tri2 t;
    for (int k = 0; k < 3; ++k) {
      const vec3d p = m.positions[m.tris[f][size_t(k)]];
      t.p[k] = {dot(p, c.u), dot(p, c.v)};
    }
    return t;
  };
  // Seeds in order of descending area: big flat regions first get big charts.
  std::vector<uint32_t> order(nf);
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return fa[a] > fa[b]; });
  using Item = std::pair<double, uint32_t>;
  // Fold test against the faces already in a chart (spatial hash of projected triangles).
  auto cells = [&](const Tri2& t) {
    return std::array<int64_t, 4>{int64_t(std::floor(std::min({t.p[0].x, t.p[1].x, t.p[2].x}) / cell)),
                                  int64_t(std::floor(std::max({t.p[0].x, t.p[1].x, t.p[2].x}) / cell)),
                                  int64_t(std::floor(std::min({t.p[0].y, t.p[1].y, t.p[2].y}) / cell)),
                                  int64_t(std::floor(std::max({t.p[0].y, t.p[1].y, t.p[2].y}) / cell))};
  };
  auto folds = [&](uint64_t id, const Chart& c, const Tri2& t) {
    const auto b = cells(t);
    for (int64_t y = b[2]; y <= b[3]; ++y)
      for (int64_t x = b[0]; x <= b[1]; ++x) {
        auto it = grid.find(cell_key(id, x, y));
        if (it == grid.end()) continue;
        for (auto g : it->second)
          if (overlap(t, project(c, g))) return true;
      }
    return false;
  };
  auto insert = [&](uint64_t id, Chart& c, uint32_t f, const Tri2& t) {
    chart_of[f] = int32_t(id);
    c.faces.push_back(f);
    const auto b = cells(t);
    for (int64_t y = b[2]; y <= b[3]; ++y)
      for (int64_t x = b[0]; x <= b[1]; ++x) grid[cell_key(id, x, y)].push_back(f);
  };
  for (const uint32_t seed : order) {
    if (chart_of[seed] >= 0) continue;
    if ((charts.size() & 255) == 0) deadline.check("UV charting");
    Chart c;
    c.axis = fn[seed];
    const vec3d helper = std::abs(c.axis.y) < 0.9 ? vec3d{0, 1, 0} : vec3d{1, 0, 0};
    c.u = normalize(cross(helper, c.axis));
    c.v = cross(c.axis, c.u);
    const uint64_t id = charts.size();
    const auto& st = m.tris[seed];
    const vec3d origin = (m.positions[st[0]] + m.positions[st[1]] + m.positions[st[2]]) * (1.0 / 3);
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
    queue.push({0, seed});
    while (!queue.empty()) {
      const uint32_t f = queue.top().second;
      queue.pop();
      if (chart_of[f] >= 0 || dot(fn[f], c.axis) < cone) continue;
      const Tri2 t = project(c, f);
      if (folds(id, c, t)) continue;  // stays free: it will seed or join another chart
      insert(id, c, f, t);
      for (int k = 0; k < 3; ++k) {
        const int32_t g = nbr[f][size_t(k)];
        if (g < 0 || chart_of[size_t(g)] >= 0) continue;
        const auto& gt = m.tris[size_t(g)];
        const vec3d gc = (m.positions[gt[0]] + m.positions[gt[1]] + m.positions[gt[2]]) * (1.0 / 3);
        queue.push({length(gc - origin), uint32_t(g)});
      }
    }
    charts.push_back(std::move(c));
  }
  // Tiny charts (surface noise, slivers) cost padding: move their faces into an adjacent larger chart
  // when they still project within 75 degrees of its axis and do not fold over it.
  const double merge_cone = std::cos(75 * std::numbers::pi / 180);
  const size_t tiny = 12;
  size_t merged = 0;
  for (size_t ci = 0; ci < charts.size(); ++ci) {
    if (charts[ci].faces.size() >= tiny) continue;
    std::vector<uint32_t> keep;
    for (auto f : charts[ci].faces) {
      bool moved = false;
      for (int k = 0; k < 3 && !moved; ++k) {
        const int32_t g = nbr[f][size_t(k)];
        if (g < 0) continue;
        const auto target = size_t(chart_of[size_t(g)]);
        if (target == ci || charts[target].faces.size() < tiny || dot(fn[f], charts[target].axis) < merge_cone) continue;
        const Tri2 t = project(charts[target], f);
        if (folds(target, charts[target], t)) continue;
        insert(target, charts[target], f, t);
        moved = true;
        ++merged;
      }
      if (!moved) keep.push_back(f);
    }
    charts[ci].faces = std::move(keep);
  }
  charts.erase(std::remove_if(charts.begin(), charts.end(), [](const Chart& c) { return c.faces.empty(); }), charts.end());
  grid.clear();
  deadline.check("UV charting");
  // Per chart: projected coordinates rotated to the principal axes (tight bounding rectangle).
  struct Rect { double w = 0, h = 0; vec2d lo; double cs = 1, sn = 0; };
  std::vector<Rect> rect(charts.size());
  for (size_t ci = 0; ci < charts.size(); ++ci) {
    const auto& c = charts[ci];
    std::vector<vec2d> pts;
    for (auto f : c.faces) {
      const Tri2 t = project(c, f);
      pts.insert(pts.end(), {t.p[0], t.p[1], t.p[2]});
    }
    vec2d mean{};
    for (auto p : pts) mean = mean + p;
    mean = mean * (1.0 / double(pts.size()));
    double sxx = 0, sxy = 0, syy = 0;
    for (auto p : pts) {
      const vec2d d = p - mean;
      sxx += d.x * d.x; sxy += d.x * d.y; syy += d.y * d.y;
    }
    const double angle = 0.5 * std::atan2(2 * sxy, sxx - syy);
    Rect& r = rect[ci];
    r.cs = std::cos(angle);
    r.sn = std::sin(angle);
    vec2d lo{1e300, 1e300}, hi{-1e300, -1e300};
    for (auto p : pts) {
      const vec2d q{p.x * r.cs + p.y * r.sn, -p.x * r.sn + p.y * r.cs};
      lo = {std::min(lo.x, q.x), std::min(lo.y, q.y)};
      hi = {std::max(hi.x, q.x), std::max(hi.y, q.y)};
    }
    r.lo = lo;
    r.w = hi.x - lo.x;
    r.h = hi.y - lo.y;
    if (r.w < r.h) {  // lay charts flat: better shelves
      std::swap(r.w, r.h);
      const double cs = r.cs, sn = r.sn;  // rotate a further 90 degrees
      r.cs = -sn;
      r.sn = cs;
      lo = {1e300, 1e300};
      for (auto p : pts) lo = {std::min(lo.x, p.x * r.cs + p.y * r.sn), std::min(lo.y, -p.x * r.sn + p.y * r.cs)};
      r.lo = lo;
    }
  }
  // Relative density per chart (area-weighted mean of its faces; 1 without a density map).
  std::vector<double> wc(charts.size(), 1.0);
  if (face_density && face_density->size() == nf)
    for (size_t ci = 0; ci < charts.size(); ++ci) {
      double a = 0, w = 0;
      for (auto f : charts[ci].faces) { a += fa[f]; w += fa[f] * double((*face_density)[f]); }
      wc[ci] = a > 0 ? std::max(1.0, w / a) : 1.0;
    }
  // Packing at the largest density that fits. Skyline packing of the charts' own outlines (column
  // profiles, four orientations, mirrored allowed: the texture is baked for this layout), so charts
  // interlock instead of each wasting its bounding rectangle; shelf packing of rectangles is the fallback.
  const int pad = std::max(2, size / 1024);
  std::vector<uint32_t> by_size(charts.size());
  std::iota(by_size.begin(), by_size.end(), 0);
  std::sort(by_size.begin(), by_size.end(), [&](uint32_t a, uint32_t b) {
    return std::max(rect[a].w, rect[a].h) * (rect[a].w + rect[a].h) * wc[a] * wc[a] > std::max(rect[b].w, rect[b].h) * (rect[b].w + rect[b].h) * wc[b] * wc[b];
  });
  // Local (rotated, lo-subtracted) chart triangles in world units, computed once.
  std::vector<std::vector<Tri2>> local(charts.size());
  for (size_t ci = 0; ci < charts.size(); ++ci) {
    const auto& r = rect[ci];
    for (auto f : charts[ci].faces) {
      Tri2 t = project(charts[ci], f), q;
      for (int k = 0; k < 3; ++k) q.p[k] = {t.p[k].x * r.cs + t.p[k].y * r.sn - r.lo.x, -t.p[k].x * r.sn + t.p[k].y * r.cs - r.lo.y};
      local[ci].push_back(q);
    }
  }
  struct Profile { int w = 0, h = 0, top_max = 0; std::vector<int> bottom, top; };
  constexpr int kEmpty = std::numeric_limits<int>::max();
  // Column profile of a chart at density s: lowest and highest texel row per column, dilated by the padding.
  auto profile_of = [&](size_t ci, double s0, bool transpose) {
    const double s = s0 * wc[ci];
    const double W = (transpose ? rect[ci].h : rect[ci].w) * s, H = (transpose ? rect[ci].w : rect[ci].h) * s;
    Profile p;
    p.w = int(std::ceil(W)) + 1 + 2 * pad;
    p.h = int(std::ceil(H)) + 1 + 2 * pad;
    std::vector<int> lo(size_t(p.w), kEmpty), hi(size_t(p.w), -1);
    for (const auto& t : local[ci]) {
      vec2d v[3];
      for (int k = 0; k < 3; ++k) {
        const vec2d q = transpose ? vec2d{t.p[k].y, t.p[k].x} : t.p[k];
        v[k] = {pad + 0.5 + q.x * s, pad + 0.5 + q.y * s};
      }
      const int x0 = std::clamp(int(std::floor(std::min({v[0].x, v[1].x, v[2].x}))), 0, p.w - 1);
      const int x1 = std::clamp(int(std::floor(std::max({v[0].x, v[1].x, v[2].x}))), 0, p.w - 1);
      for (int x = x0; x <= x1; ++x) {
        double ylo = 1e300, yhi = -1e300;
        for (int k = 0; k < 3; ++k) {
          if (v[k].x >= x && v[k].x <= x + 1) { ylo = std::min(ylo, v[k].y); yhi = std::max(yhi, v[k].y); }
          const vec2d ea = v[k], eb = v[(k + 1) % 3];
          for (double xs : {double(x), double(x + 1)})
            if ((ea.x - xs) * (eb.x - xs) <= 0 && ea.x != eb.x) {
              const double y = ea.y + (eb.y - ea.y) * (xs - ea.x) / (eb.x - ea.x);
              ylo = std::min(ylo, y);
              yhi = std::max(yhi, y);
            }
        }
        if (ylo > yhi) continue;
        lo[size_t(x)] = std::min(lo[size_t(x)], int(std::floor(ylo)));
        hi[size_t(x)] = std::max(hi[size_t(x)], int(std::floor(yhi)) + 1);
      }
    }
    p.bottom.assign(size_t(p.w), kEmpty);
    p.top.assign(size_t(p.w), -1);
    for (int x = 0; x < p.w; ++x)
      for (int d = -pad; d <= pad; ++d) {
        const int c = x + d;
        if (c < 0 || c >= p.w || lo[size_t(c)] == kEmpty) continue;
        p.bottom[size_t(x)] = std::max(0, std::min(p.bottom[size_t(x)], lo[size_t(c)] - pad));
        p.top[size_t(x)] = std::min(p.h, std::max(p.top[size_t(x)], hi[size_t(c)] + pad));
      }
    for (int x = 0; x < p.w; ++x)
      if (p.bottom[size_t(x)] != kEmpty) p.top_max = std::max(p.top_max, p.top[size_t(x)]);
    return p;
  };
  auto rotate180 = [&](const Profile& p) {
    Profile q = p;
    q.top_max = 0;
    for (int x = 0; x < p.w; ++x) {
      const int c = p.w - 1 - x;
      if (p.bottom[size_t(c)] == kEmpty) { q.bottom[size_t(x)] = kEmpty; q.top[size_t(x)] = -1; continue; }
      q.bottom[size_t(x)] = p.h - p.top[size_t(c)];
      q.top[size_t(x)] = p.h - p.bottom[size_t(c)];
      q.top_max = std::max(q.top_max, q.top[size_t(x)]);
    }
    return q;
  };
  struct Place { int x = 0, y = 0, variant = 0; };  // variant: bit0 transpose, bit1 rotate 180
  std::vector<Place> place(charts.size());
  auto skyline = [&](double s, bool commit) {
    std::vector<int> sky(size_t(size), 0);
    for (auto ci : by_size) {
      int best_peak = kEmpty, best_x = 0, best_y = 0, best_v = -1;
      const Profile base[2] = {profile_of(ci, s, false), profile_of(ci, s, true)};
      for (int v = 0; v < 4; ++v) {
        const Profile pr = (v & 2) ? rotate180(base[v & 1]) : base[v & 1];
        if (pr.w > size || pr.h > size) continue;
        for (int x = 0; x + pr.w <= size; ++x) {
          int y = 0;
          bool worse = false;
          for (int c = 0; c < pr.w; ++c) {
            if (pr.bottom[size_t(c)] == kEmpty) continue;
            y = std::max(y, sky[size_t(x + c)] - pr.bottom[size_t(c)]);
            if (y + pr.top_max >= best_peak) { worse = true; break; }
          }
          if (worse) continue;
          best_peak = y + pr.top_max;
          best_x = x;
          best_y = y;
          best_v = v;
        }
      }
      if (best_v < 0 || best_peak > size) return false;
      const Profile pr = (best_v & 2) ? rotate180(base[best_v & 1]) : base[best_v & 1];
      for (int c = 0; c < pr.w; ++c)
        if (pr.bottom[size_t(c)] != kEmpty) sky[size_t(best_x + c)] = std::max(sky[size_t(best_x + c)], best_y + pr.top[size_t(c)]);
      if (commit) place[ci] = {best_x, best_y, best_v};
    }
    return true;
  };
  std::vector<uint32_t> by_height(charts.size());
  std::iota(by_height.begin(), by_height.end(), 0);
  std::sort(by_height.begin(), by_height.end(), [&](uint32_t a, uint32_t b) { return rect[a].h * wc[a] > rect[b].h * wc[b]; });
  auto shelf = [&](double s, bool commit) {
    int x = 0, y = 0, row = 0;
    for (auto ci : by_height) {
      const int w = int(std::ceil(rect[ci].w * s * wc[ci])) + 1 + 2 * pad, h = int(std::ceil(rect[ci].h * s * wc[ci])) + 1 + 2 * pad;
      if (w > size) return false;
      if (x + w > size) { y += row; x = 0; row = 0; }
      if (y + h > size) return false;
      if (commit) place[ci] = {x, y, 0};
      x += w;
      row = std::max(row, h);
    }
    return true;
  };
  double rect_area = 0, chart_area = 0;
  for (size_t ci = 0; ci < charts.size(); ++ci) rect_area += rect[ci].w * rect[ci].h * wc[ci] * wc[ci];
  for (size_t ci = 0; ci < charts.size(); ++ci)
    for (auto f : charts[ci].faces) chart_area += fa[f] * std::abs(dot(fn[f], charts[ci].axis)) * wc[ci] * wc[ci];
  // Shelf density first (always found), then the skyline from above it, a few bisection steps.
  double lo = 0, hi = std::sqrt(double(size) * size / std::max(rect_area, 1e-30));
  for (int it = 0; it < 18; ++it) {
    const double mid = (lo + hi) / 2;
    (shelf(mid, false) ? lo : hi) = mid;
  }
  if (lo <= 0) throw std::runtime_error("UV charts do not fit the atlas");
  const double shelf_s = lo;
  std::string packer = "shelf";
  double s = shelf_s;
  int skyline_steps = 0;
  {
    double sl = shelf_s, sh = std::max(shelf_s * 1.05, std::sqrt(double(size) * size / std::max(chart_area, 1e-30)));
    bool any = false;
    for (int it = 0; it < 7 && deadline.remaining() > 5; ++it, ++skyline_steps) {
      const double mid = it == 0 ? std::min(sh, shelf_s * 1.3) : (sl + sh) / 2;
      if (skyline(mid, false)) { sl = mid; any = true; } else sh = mid;
    }
    if (any && sl > shelf_s * 1.01 && skyline(sl, true)) { s = sl; packer = "skyline"; }
  }
  if (packer == "shelf" && !shelf(s, true)) throw std::runtime_error("UV charts do not fit the atlas");
  // Render mesh: vertices split per chart; face f stays face f.
  Atlas atlas;
  atlas.size = size;
  const auto normals = vertex_normals(m);
  auto& tm = atlas.mesh;
  tm.indices.assign(nf * 3, 0);
  std::unordered_map<uint64_t, uint32_t> remap;
  remap.reserve(nf * 2);
  double uv_area = 0;
  for (size_t ci = 0; ci < charts.size(); ++ci) {
    const auto& c = charts[ci];
    const auto& r = rect[ci];
    const double sc = s * wc[ci];
    for (auto f : c.faces) {
      const Tri2 t = project(c, f);
      for (int k = 0; k < 3; ++k) {
        const uint32_t v = m.tris[f][size_t(k)];
        const uint64_t key = uint64_t(ci) << 32 | v;
        auto [it, inserted] = remap.try_emplace(key, uint32_t(tm.positions.size()));
        if (inserted) {
          const vec2d p = t.p[k];
          vec2d q{p.x * r.cs + p.y * r.sn - r.lo.x, -p.x * r.sn + p.y * r.cs - r.lo.y};
          const Place& pl = place[ci];
          double W = r.w, H = r.h;
          if (pl.variant & 1) {
            q = {q.y, q.x};
            std::swap(W, H);
          }
          vec2d tex{pad + 0.5 + q.x * sc, pad + 0.5 + q.y * sc};
          if (pl.variant & 2) {  // rotate 180 inside the chart's box (the same box as its profile)
            const double bw = std::ceil(W * sc) + 1 + 2 * pad, bh = std::ceil(H * sc) + 1 + 2 * pad;
            tex = {bw - tex.x, bh - tex.y};
          }
          tm.positions.push_back(m.positions[v]);
          tm.normals.push_back(normals[v]);
          tm.uvs.push_back({(pl.x + tex.x) / size, (pl.y + tex.y) / size});
          atlas.logical_vertex.push_back(v);
        }
        tm.indices[3 * f + size_t(k)] = it->second;
      }
      uv_area += fa[f] * std::abs(dot(fn[f], c.axis)) * sc * sc;
    }
  }
  atlas.metrics = {{"method", "planar charts"}, {"packer", packer}, {"shelf_texels_per_unit", shelf_s}, {"skyline_steps", skyline_steps}, {"cone_degrees", cone_deg}, {"size", size}, {"charts", charts.size()}, {"tiny_chart_faces_merged", merged},
                   {"utilization", uv_area / (double(size) * size)}, {"texels_per_unit", s}, {"padding", pad},
                   {"max_projection_stretch", 1 / cone},
                   {"density_weighting", {{"used", face_density != nullptr}, {"chart_weight_max", *std::max_element(wc.begin(), wc.end())},
                                          {"charts_boosted", std::count_if(wc.begin(), wc.end(), [](double w) { return w > 1.05; })}}},
                   {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
  return atlas;
}
}  // namespace p3dasset::autotex

namespace p3dasset::autotex {
Atlas build_xatlas(const pixel3d::Mesh& m, int size, int attempt, const Deadline& deadline);
// attempt 0: planar charts, 60 degree cone (few seams); 1: 45 degrees (less stretch);
// 2: xatlas on meshes small enough for it to finish quickly, else a 35 degree cone.
Atlas build_atlas(const pixel3d::Mesh& m, int size, int attempt, const Deadline& deadline, const std::vector<float>* face_density) {
  if (m.positions.empty() || m.tris.empty() || m.tris.size() > 4000000) throw std::runtime_error("Mesh outside the atlas profile");
  if (attempt <= 0) return build_planar_atlas(m, size, 60, deadline, face_density);
  if (attempt == 1) return build_planar_atlas(m, size, 45, deadline, face_density);
  if (m.tris.size() <= 60000) return build_xatlas(m, size, 0, deadline);
  return build_planar_atlas(m, size, 35, deadline, face_density);
}
}  // namespace p3dasset::autotex

namespace p3dasset::autotex {
std::vector<float> picture_detail_density(const pixel3d::Mesh& m, const Source& src, const Registration& reg, double max_ratio,
                                          nlohmann::json& evidence) {
  using namespace pixel3d;
  using namespace detail;
  const View view = View::build(m, src, reg.pose);
  // Picture detail: Oklab gradient magnitude (L and chroma), box-averaged over ~1% of the subject, normalised by
  // its 90th percentile over the subject. Faces, hands and emblems are where the picture changes fastest.
  const int W = src.width, H = src.height;
  std::vector<float> g(size_t(W) * H, 0.f);
  std::vector<std::array<float, 3>> lab(size_t(W) * H);
  for (size_t i = 0; i < lab.size(); ++i) lab[i] = oklab(src.px[i]);
  for (int y = 1; y + 1 < H; ++y)
    for (int x = 1; x + 1 < W; ++x) {
      const size_t i = size_t(y) * W + x;
      if (src.px[i][3] < 0.5f || src.px[i - 1][3] < 0.5f || src.px[i + 1][3] < 0.5f || src.px[i - W][3] < 0.5f || src.px[i + W][3] < 0.5f) continue;
      double s2 = 0;
      for (int d = 0; d < 3; ++d) {
        const double gx = lab[i + 1][size_t(d)] - lab[i - 1][size_t(d)], gy = lab[i + W][size_t(d)] - lab[i - W][size_t(d)];
        s2 += (gx * gx + gy * gy) * (d == 0 ? 1.0 : 2.0);
      }
      g[i] = float(std::sqrt(s2));
    }
  int bx0 = W, by0 = H, bx1 = -1, by1 = -1;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (src.px[size_t(y) * W + x][3] >= 0.5f) { bx0 = std::min(bx0, x); by0 = std::min(by0, y); bx1 = std::max(bx1, x); by1 = std::max(by1, y); }
  const int r = std::max(2, int(std::lround(0.012 * std::max(bx1 - bx0 + 1, by1 - by0 + 1))));
  // Box average (radius r) via prefix sums, rows then columns.
  std::vector<double> hsum(size_t(W) * H, 0), box(size_t(W) * H, 0), pre(size_t(std::max(W, H)) + 1, 0);
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) pre[size_t(x) + 1] = pre[size_t(x)] + g[size_t(y) * W + x];
    for (int x = 0; x < W; ++x) {
      const int a = std::max(0, x - r), b = std::min(W, x + r + 1);
      hsum[size_t(y) * W + x] = (pre[size_t(b)] - pre[size_t(a)]) / (b - a);
    }
  }
  for (int x = 0; x < W; ++x) {
    for (int y = 0; y < H; ++y) pre[size_t(y) + 1] = pre[size_t(y)] + hsum[size_t(y) * W + x];
    for (int y = 0; y < H; ++y) {
      const int a = std::max(0, y - r), b = std::min(H, y + r + 1);
      box[size_t(y) * W + x] = (pre[size_t(b)] - pre[size_t(a)]) / (b - a);
    }
  }
  std::vector<double> inside;
  for (size_t i = 0; i < box.size(); i += 3)
    if (src.px[i][3] >= 0.5f) inside.push_back(box[i]);
  double p90 = 1e-6;
  if (!inside.empty()) {
    std::nth_element(inside.begin(), inside.begin() + std::ptrdiff_t(inside.size() * 9 / 10), inside.end());
    p90 = std::max(1e-6, inside[inside.size() * 9 / 10]);
  }
  const auto normals = vertex_normals(m);
  std::vector<float> w(m.tris.size(), 1.f);
  double boosted_area = 0, total_area = 0;
  for (size_t f = 0; f < m.tris.size(); ++f) {
    const auto& t = m.tris[f];
    const vec3d c = (m.positions[t[0]] + m.positions[t[1]] + m.positions[t[2]]) * (1.0 / 3);
    const vec3d cr = cross(m.positions[t[1]] - m.positions[t[0]], m.positions[t[2]] - m.positions[t[0]]);
    const double A = length(cr) / 2;
    total_area += A;
    if (A <= 0) continue;
    const auto o = view.observe(src, reg.pose, c, normalize(normals[t[0]] + normals[t[1]] + normals[t[2]]), cr / (2 * A), 0.25, 0.6);
    if (o.conf <= 0.05f) continue;
    const vec2d q = reg.pose.to_image(c);
    const int x = std::clamp(int(q.x), 0, W - 1), y = std::clamp(int(q.y), 0, H - 1);
    const double d = std::clamp(box[size_t(y) * W + x] / p90, 0.0, 1.0);
    // Seen surface gets a little more (the picture is sharper than TRELLIS wherever it shows the surface),
    // detailed surface up to max_ratio.
    w[f] = float(1 + o.conf * (max_ratio - 1) * (0.25 + 0.75 * d * d));
    if (w[f] > 1.5f) boosted_area += A;
  }
  evidence = {{"rule", "relative linear texel density 1 (hidden) .. max_ratio: 1 + conf (max_ratio - 1)(0.25 + 0.75 detail^2); detail = picture Oklab gradient, box-averaged, / its p90"},
              {"max_ratio", max_ratio}, {"box_radius_px", r}, {"area_fraction_above_1_5", total_area > 0 ? boosted_area / total_area : 0}};
  return w;
}
}  // namespace p3dasset::autotex
