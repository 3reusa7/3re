// UV atlas, visibility-aware projection and material-consistent reconstruction of hidden surfaces.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <thread>
#include "auto_texture_internal.hpp"
#include "xatlas.h"

namespace p3dasset::autotex {
using namespace pixel3d;
namespace detail {
std::vector<vec3d> vertex_normals(const Mesh& m) {
  std::vector<vec3d> n(m.positions.size());
  for (const auto& t : m.tris) {
    const vec3d f = cross(m.positions[t[1]] - m.positions[t[0]], m.positions[t[2]] - m.positions[t[0]]);
    for (auto i : t) n[i] = n[i] + f;
  }
  for (auto& v : n) {
    const double l = length(v);
    v = l > 1e-20 ? v / l : vec3d{0, 0, 1};
  }
  return n;
}
Mesh render_geometry(const TexturedMesh& m) {
  Mesh out;
  out.positions = m.positions;
  for (size_t f = 0; f + 2 < m.indices.size(); f += 3) out.tris.push_back({m.indices[f], m.indices[f + 1], m.indices[f + 2]});
  return out;
}
double diagonal(const Mesh& m) {
  vec3d lo = m.positions.front(), hi = lo;
  for (auto p : m.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  return length(hi - lo);
}
void raster_uv(vec2d a, vec2d b, vec2d c, int w, int h, int y0, int y1, const std::function<void(int, int, double, double, double)>& f) {
  const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (std::abs(area) < 1e-14) return;
  const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(w - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
  const int ty0 = std::max(y0, int(std::floor(std::min({a.y, b.y, c.y})))), ty1 = std::min({y1 - 1, h - 1, int(std::ceil(std::max({a.y, b.y, c.y})))});
  for (int y = ty0; y <= ty1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const double px = x + 0.5, py = y + 0.5;
      const double w0 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) / area;
      const double w1 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) / area;
      const double w2 = 1 - w0 - w1;
      if (w0 < -1e-7 || w1 < -1e-7 || w2 < -1e-7) continue;
      f(x, y, w0, w1, w2);
    }
}
void raster_uv_border(vec2d a, vec2d b, vec2d c, int w, int h, int y0, int y1, double reach, const std::function<void(int, int, double, double, double)>& f) {
  const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (std::abs(area) < 1e-18) return;
  const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}) - reach))), x1 = std::min(w - 1, int(std::ceil(std::max({a.x, b.x, c.x}) + reach)));
  const int ty0 = std::max(y0, int(std::floor(std::min({a.y, b.y, c.y}) - reach))), ty1 = std::min({y1 - 1, h - 1, int(std::ceil(std::max({a.y, b.y, c.y}) + reach))});
  const vec2d v[3] = {a, b, c};
  for (int y = ty0; y <= ty1; ++y)
    for (int x = x0; x <= x1; ++x) {
      const vec2d p{x + 0.5, y + 0.5};
      const double w0 = ((c.x - b.x) * (p.y - b.y) - (c.y - b.y) * (p.x - b.x)) / area;
      const double w1 = ((a.x - c.x) * (p.y - c.y) - (a.y - c.y) * (p.x - c.x)) / area;
      if (w0 >= -1e-7 && w1 >= -1e-7 && 1 - w0 - w1 >= -1e-7) continue;  // interior: the main pass owns it
      double best = 1e300, bw[3]{};
      for (int e = 0; e < 3; ++e) {
        const vec2d s = v[e], d = v[(e + 1) % 3] - s;
        const double len2 = dot(d, d);
        const double t = len2 > 0 ? std::clamp(dot(p - s, d) / len2, 0.0, 1.0) : 0.0;
        const vec2d q = s + d * t;
        const double dist = length(p - q);
        if (dist < best) {
          best = dist;
          bw[0] = bw[1] = bw[2] = 0;
          bw[e] = 1 - t;
          bw[(e + 1) % 3] = t;
        }
      }
      if (best <= reach) f(x, y, bw[0], bw[1], bw[2]);
    }
}
void parallel_rows(int rows, const std::function<void(int, int)>& body) {
  const int workers = int(std::clamp(std::thread::hardware_concurrency(), 1u, 6u));
  const int band = std::max(1, (rows + workers - 1) / workers);
  std::vector<std::thread> pool;
  for (int y = 0; y < rows; y += band) pool.emplace_back(body, y, std::min(rows, y + band));
  for (auto& t : pool) t.join();
}
std::array<float, 4> sample_texture(const ImageRGBA& t, vec2d uv) {
  const double x = uv.x * t.width() - 0.5, y = uv.y * t.height() - 0.5;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const double fx = x - ix, fy = y - iy;
  std::array<double, 4> s{};
  for (int dy = 0; dy < 2; ++dy)
    for (int dx = 0; dx < 2; ++dx) {
      const auto p = t.at(std::clamp(ix + dx, 0, t.width() - 1), std::clamp(iy + dy, 0, t.height() - 1));
      const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
      s[0] += linear_from_srgb8(p.r) * w;
      s[1] += linear_from_srgb8(p.g) * w;
      s[2] += linear_from_srgb8(p.b) * w;
      s[3] += p.a / 255.0 * w;
    }
  return {float(s[0]), float(s[1]), float(s[2]), float(s[3])};
}

void chamfer(std::vector<float>& f, int w, int h);
View View::build(const Mesh& m, const Source& s, const Pose& p) {
  View v;
  v.w = s.width;
  v.h = s.height;
  v.diag = diagonal(m);
  v.pixel_world = 1.0 / std::max(p.scale, 1e-12);
  v.depth.assign(size_t(v.w) * v.h, std::numeric_limits<float>::infinity());
  std::vector<vec2d> q(m.positions.size());
  std::vector<double> d(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); ++i) {
    q[i] = p.to_image(m.positions[i]);
    d[i] = p.depth(m.positions[i]);
  }
  for (const auto& t : m.tris) {
    const vec2d a = q[t[0]], b = q[t[1]], c = q[t[2]];
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area) < 1e-14) continue;
    const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(v.w - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(v.h - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) / area;
        const double w1 = ((a.x - c.x) * (py - c.y) - (a.y - c.y) * (px - c.x)) / area;
        const double w2 = 1 - w0 - w1;
        if (w0 < -1e-7 || w1 < -1e-7 || w2 < -1e-7) continue;
        auto& z = v.depth[size_t(y) * v.w + x];
        z = std::min(z, float(w0 * d[t[0]] + w1 * d[t[1]] + w2 * d[t[2]]));
      }
  }
  // Unreliable pixels: outside the alpha mask, empty, or next to a depth discontinuity.
  const float far = 64;
  v.edge.assign(v.depth.size(), far);
  const double jump = 8 * v.pixel_world + 0.002 * v.diag;
  for (int y = 0; y < v.h; ++y)
    for (int x = 0; x < v.w; ++x) {
      const size_t i = size_t(y) * v.w + x;
      bool bad = s.alpha(x, y) < 0.5f || !std::isfinite(v.depth[i]);
      if (!bad)
        for (auto [dx, dy] : std::array<std::pair<int, int>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= v.w || ny >= v.h) { bad = true; break; }
          const float n = v.depth[size_t(ny) * v.w + nx];
          if (!std::isfinite(n) || std::abs(n - v.depth[i]) > jump) { bad = true; break; }
        }
      if (bad) v.edge[i] = 0;
    }
  // Two-pass chamfer distances (px): to any unreliable pixel, and to the alpha outline alone.
  v.silhouette.assign(v.depth.size(), far);
  for (int y = 0; y < v.h; ++y)
    for (int x = 0; x < v.w; ++x)
      if (s.alpha(x, y) < 0.5f) v.silhouette[size_t(y) * v.w + x] = 0;
  for (auto* field : {&v.edge, &v.silhouette}) chamfer(*field, v.w, v.h);
  return v;
}
void chamfer(std::vector<float>& f, int w, int h) {
  auto at = [&](int x, int y) -> float& { return f[size_t(y) * w + x]; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float& e = at(x, y);
      if (x) e = std::min(e, at(x - 1, y) + 1.f);
      if (y) e = std::min(e, at(x, y - 1) + 1.f);
      if (x && y) e = std::min(e, at(x - 1, y - 1) + 1.4142f);
      if (y && x + 1 < w) e = std::min(e, at(x + 1, y - 1) + 1.4142f);
    }
  for (int y = h - 1; y >= 0; --y)
    for (int x = w - 1; x >= 0; --x) {
      float& e = at(x, y);
      if (x + 1 < w) e = std::min(e, at(x + 1, y) + 1.f);
      if (y + 1 < h) e = std::min(e, at(x, y + 1) + 1.f);
      if (x + 1 < w && y + 1 < h) e = std::min(e, at(x + 1, y + 1) + 1.4142f);
      if (y + 1 < h && x) e = std::min(e, at(x - 1, y + 1) + 1.4142f);
    }
}

View::Obs View::observe(const Source& s, const Pose& p, vec3d pos, vec3d n, vec3d fn, double c0, double c1) const {
  Obs o;
  const vec3d to_cam = -p.forward;
  o.facing = float(dot(fn, to_cam));
  if (o.facing <= 0.02f) return o;
  const vec2d q = p.to_image(pos);
  const int ix = int(std::floor(q.x)), iy = int(std::floor(q.y));
  o.stage = 1;
  if (ix < 0 || iy < 0 || ix >= w || iy >= h) return o;
  o.stage = 2;
  const size_t i = size_t(iy) * w + ix;
  const double cosn = std::max(0.0, dot(n, to_cam));
  const double tan_t = std::sqrt(std::max(0.0, 1 - double(o.facing) * o.facing)) / std::max(double(o.facing), 0.2);
  const double bias = pixel_world * (1.0 + 1.5 * tan_t) + 0.0015 * diag;
  if (!(p.depth(pos) <= double(depth[i]) + bias)) return o;  // occluded
  o.stage = 3;
  o.rgb = s.sample(q.x, q.y);
  o.w_facing = float(smoothstep(c0, c1, std::min(cosn, double(o.facing) + 0.2)));
  o.w_alpha = float(smoothstep(0.3, 0.9, o.rgb[3]));
  o.w_edge = float(smoothstep(0.5, 2.5, edge[i]));
  o.conf = o.w_facing * o.w_alpha * o.w_edge;
  return o;
}
}  // namespace detail

using namespace detail;

Atlas build_xatlas(const Mesh& m, int size, int attempt, const Deadline& deadline) {
  const auto started = std::chrono::steady_clock::now();
  if (m.positions.empty() || m.tris.empty() || m.positions.size() > 2000000 || m.tris.size() > 4000000) throw std::runtime_error("Mesh outside the atlas profile");
  // xatlas has an absolute area epsilon: parameterise a power-of-two rescaled copy (exact), export originals.
  const double diag = diagonal(m);
  if (!std::isfinite(diag) || diag <= 0) throw std::runtime_error("Invalid mesh extent");
  const double scale = std::exp2(std::floor(std::log2(1024.0 / diag)));
  std::vector<float> points;
  points.reserve(m.positions.size() * 3);
  for (auto p : m.positions)
    for (double c : {p.x, p.y, p.z}) points.push_back(float(c * scale));
  std::vector<uint32_t> indices;
  indices.reserve(m.tris.size() * 3);
  for (const auto& t : m.tris) indices.insert(indices.end(), t.begin(), t.end());
  std::unique_ptr<xatlas::Atlas, decltype(&xatlas::Destroy)> a(xatlas::Create(), xatlas::Destroy);
  struct Cancel { const Deadline* d; bool hit = false; } cancel{&deadline};
  xatlas::SetProgressCallback(a.get(), [](xatlas::ProgressCategory, int, void* user) {
    auto* c = static_cast<Cancel*>(user);
    if (c->d->remaining() < 0) c->hit = true;
    return !c->hit;
  }, &cancel);
  xatlas::MeshDecl decl;
  decl.vertexPositionData = points.data();
  decl.vertexPositionStride = 12;
  decl.vertexCount = uint32_t(m.positions.size());
  decl.indexData = indices.data();
  decl.indexCount = uint32_t(indices.size());
  decl.indexFormat = xatlas::IndexFormat::UInt32;
  decl.epsilon = 0;
  if (xatlas::AddMesh(a.get(), decl) != xatlas::AddMeshError::Success) throw std::runtime_error("xatlas rejected the mesh");
  xatlas::ChartOptions charts;
  charts.fixWinding = true;
  if (attempt == 1) {  // fewer, larger charts: fewer seams, more stretch tolerated
    charts.maxCost = 4;
    charts.normalDeviationWeight = 1;
  } else if (attempt >= 2) {  // many small charts: lowest stretch, always packs
    charts.maxCost = 1;
    charts.maxIterations = 1;
    charts.straightnessWeight = 2;
  }
  xatlas::ComputeCharts(a.get(), charts);
  if (cancel.hit) throw BudgetExceeded("Time budget exceeded during UV charting");
  xatlas::PackOptions pack;
  pack.resolution = uint32_t(size);
  pack.padding = uint32_t(std::max(4, size / 256));
  pack.bilinear = true;
  pack.blockAlign = true;
  xatlas::PackCharts(a.get(), pack);
  // xatlas only enforces the resolution when texelsPerUnit is given: estimate first, then pack at a
  // density that fits one size x size sheet (shrinking until it does).
  if (a->atlasCount != 1 || a->width > uint32_t(size) || a->height > uint32_t(size)) {
    float tpu = a->texelsPerUnit * float(size) / float(std::max({a->width, a->height, 1u})) * 0.98f;
    for (int retry = 0; retry < 8 && !cancel.hit; ++retry) {
      pack.texelsPerUnit = tpu;
      xatlas::PackCharts(a.get(), pack);
      if (a->atlasCount == 1 && a->width <= uint32_t(size) && a->height <= uint32_t(size)) break;
      tpu *= 0.92f;
    }
  }
  if (cancel.hit) throw BudgetExceeded("Time budget exceeded during UV packing");
  if (a->meshCount != 1 || a->atlasCount != 1 || !a->width || !a->height || a->width > uint32_t(size) || a->height > uint32_t(size))
    throw std::runtime_error("xatlas did not produce a single atlas sheet within the size");
  const auto& out = a->meshes[0];
  if (out.indexCount != indices.size()) throw std::runtime_error("xatlas changed the face count");
  Atlas atlas;
  atlas.size = size;
  const auto normals = vertex_normals(m);
  auto& tm = atlas.mesh;
  tm.positions.reserve(out.vertexCount);
  for (uint32_t i = 0; i < out.vertexCount; ++i) {
    const auto& v = out.vertexArray[i];
    if (v.xref >= m.positions.size()) throw std::runtime_error("xatlas returned an invalid vertex reference");
    tm.positions.push_back(m.positions[v.xref]);
    tm.normals.push_back(normals[v.xref]);
    tm.uvs.push_back({double(v.uv[0]) / atlas.size, double(v.uv[1]) / atlas.size});
    atlas.logical_vertex.push_back(v.xref);
  }
  tm.indices.assign(out.indexArray, out.indexArray + out.indexCount);
  for (size_t f = 0; f < m.tris.size(); ++f)
    for (int k = 0; k < 3; ++k)
      if (atlas.logical_vertex[tm.indices[3 * f + size_t(k)]] != m.tris[f][size_t(k)]) throw std::runtime_error("xatlas reordered faces");
  atlas.metrics = {{"attempt", attempt}, {"size", atlas.size}, {"charts", a->chartCount}, {"utilization", a->utilization ? a->utilization[0] : 0.f},
                   {"texels_per_unit", a->texelsPerUnit}, {"padding", pack.padding},
                   {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
  return atlas;
}

namespace {
std::string hex(const std::array<float, 4>& c) {
  char b[8];
  std::snprintf(b, sizeof b, "#%02x%02x%02x", srgb8_from_linear(c[0]), srgb8_from_linear(c[1]), srgb8_from_linear(c[2]));
  return b;
}
struct Clusters {
  std::vector<std::array<float, 3>> centres;
  std::vector<int> label;
  std::vector<double> sse;
};
// Weighted k-means in Oklab with deterministic farthest-point seeding; k picked by diminishing returns.
Clusters cluster_colours(const std::vector<std::array<float, 3>>& lab, const std::vector<double>& weight) {
  const size_t n = lab.size();
  auto dist2 = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const double x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
  };
  auto run = [&](int k) {
    Clusters c;
    std::array<double, 3> mean{};
    double wsum = 0;
    for (size_t i = 0; i < n; ++i) {
      for (int d = 0; d < 3; ++d) mean[size_t(d)] += lab[i][size_t(d)] * weight[i];
      wsum += weight[i];
    }
    c.centres.push_back({float(mean[0] / wsum), float(mean[1] / wsum), float(mean[2] / wsum)});
    std::vector<double> nearest(n, 1e30);
    while (int(c.centres.size()) < k) {
      size_t far = 0;
      double best = -1;
      for (size_t i = 0; i < n; ++i) {
        nearest[i] = std::min(nearest[i], dist2(lab[i], c.centres.back()));
        if (nearest[i] * std::sqrt(weight[i]) > best) { best = nearest[i] * std::sqrt(weight[i]); far = i; }
      }
      c.centres.push_back(lab[far]);
    }
    c.label.assign(n, 0);
    double total = 0;
    for (int it = 0; it < 14; ++it) {
      std::vector<std::array<double, 4>> acc(size_t(k), {0, 0, 0, 0});
      total = 0;
      for (size_t i = 0; i < n; ++i) {
        int bi = 0;
        double bd = 1e30;
        for (int j = 0; j < k; ++j) {
          const double d = dist2(lab[i], c.centres[size_t(j)]);
          if (d < bd) { bd = d; bi = j; }
        }
        c.label[i] = bi;
        total += bd * weight[i];
        for (int d = 0; d < 3; ++d) acc[size_t(bi)][size_t(d)] += lab[i][size_t(d)] * weight[i];
        acc[size_t(bi)][3] += weight[i];
      }
      for (int j = 0; j < k; ++j)
        if (acc[size_t(j)][3] > 0)
          c.centres[size_t(j)] = {float(acc[size_t(j)][0] / acc[size_t(j)][3]), float(acc[size_t(j)][1] / acc[size_t(j)][3]), float(acc[size_t(j)][2] / acc[size_t(j)][3])};
    }
    c.sse.push_back(total);
    return c;
  };
  Clusters best = run(1);
  const double sse1 = std::max(best.sse.back(), 1e-12);
  std::vector<double> history{sse1};
  for (int k = 2; k <= 10 && int(n) >= k * 4; ++k) {
    Clusters c = run(k);
    const double s = c.sse.back();
    history.push_back(s);
    if (s > 0.8 * history[history.size() - 2]) break;  // diminishing returns: keep previous k
    best = std::move(c);
    if (s / sse1 < 0.06) break;
  }
  best.sse = history;
  return best;
}
}  // namespace

Bake bake(const Mesh& L, const Atlas& atlas, const Source& src, const Registration& reg, const BakeOptions& opt, const Deadline& deadline) {
  const auto started = std::chrono::steady_clock::now();
  const Pose& pose = reg.pose;
  // Every registered picture (the front first; extra angles when imported).
  const Pictures pics = Pictures::make(L, src, reg, opt.extra_views);
  const View& view = pics.p[0].view;
  deadline.check("visibility");
  const auto normals = vertex_normals(L);
  const size_t nv = L.positions.size();
  // Vertex area (one third of adjacent faces) and adjacency (CSR) along mesh edges.
  std::vector<double> varea(nv, 0);
  std::vector<uint32_t> deg(nv + 1, 0);
  for (const auto& t : L.tris) {
    const double a = length(cross(L.positions[t[1]] - L.positions[t[0]], L.positions[t[2]] - L.positions[t[0]])) / 6;
    for (int k = 0; k < 3; ++k) {
      varea[t[size_t(k)]] += a;
      deg[t[size_t(k)] + 1] += 2;
    }
  }
  std::partial_sum(deg.begin(), deg.end(), deg.begin());
  std::vector<uint32_t> adj(deg.back()), fill(deg.begin(), deg.end() - 1);
  for (const auto& t : L.tris)
    for (int k = 0; k < 3; ++k) {
      const uint32_t i = t[size_t(k)], j = t[size_t((k + 1) % 3)];
      adj[fill[i]++] = j;
      adj[fill[j]++] = i;
    }
  // 1. Observations at the vertices.
  std::vector<View::Obs> vobs(nv);
  std::vector<int8_t> vdom(nv, -1);  // picture that dominates the vertex's observation
  parallel_rows(int(nv), [&](int a, int b) {
    for (int i = a; i < b; ++i) {
      const auto c = pics.observe(L.positions[size_t(i)], normals[size_t(i)], normals[size_t(i)], opt.observe_cos, opt.observe_full_cos, nullptr, false);
      vobs[size_t(i)] = c.o;
      vdom[size_t(i)] = int8_t(c.dominant);
    }
  });
  // Which picture owns how much of the surface (area, confidently observed vertices).
  nlohmann::json ownership;
  {
    std::vector<double> own(pics.p.size() + 1, 0);
    double total = 0;
    for (size_t i = 0; i < nv; ++i) {
      total += varea[i];
      own[vobs[i].conf >= 0.5f && vdom[i] >= 0 ? size_t(vdom[i]) : pics.p.size()] += varea[i];
    }
    for (size_t k = 0; k < pics.p.size(); ++k) {
      std::string key = pics.p[k].label;
      if (ownership.contains(key)) key += " " + std::to_string(k + 1);  // two pictures from the same angle
      ownership[key] = own[k] / std::max(total, 1e-30);
    }
    ownership["not_seen_by_any_picture"] = own.back() / std::max(total, 1e-30);
  }
  nlohmann::json breakdown;
  {
    double by_stage[4]{}, wf = 0, wa = 0, we = 0, total = 0;
    for (size_t i = 0; i < nv; ++i) {
      by_stage[vobs[i].stage] += varea[i];
      total += varea[i];
      if (vobs[i].stage == 3) { wf += vobs[i].w_facing * varea[i]; wa += vobs[i].w_alpha * varea[i]; we += vobs[i].w_edge * varea[i]; }
    }
    const double seen = std::max(by_stage[3], 1e-30);
    breakdown = {{"faces_away", by_stage[0] / total}, {"outside_picture", by_stage[1] / total}, {"occluded", by_stage[2] / total},
                 {"in_view", by_stage[3] / total}, {"in_view_mean_facing_weight", wf / seen}, {"in_view_mean_alpha_weight", wa / seen},
                 {"in_view_mean_edge_weight", we / seen}};
  }
  std::vector<uint32_t> seeds;
  for (size_t i = 0; i < nv; ++i)
    if (vobs[i].conf >= 0.5f) seeds.push_back(uint32_t(i));
  if (seeds.size() < 16) throw std::runtime_error("Too few surface points are visible in the picture to texture this model");
  // 2. Colour regions of the observed surface (not semantic labels).
  std::vector<std::array<float, 3>> lab;
  std::vector<double> lw;
  for (auto i : seeds) {
    lab.push_back(oklab(vobs[i].rgb));
    lw.push_back(varea[i] * vobs[i].conf);
  }
  const auto clusters = cluster_colours(lab, lw);
  const int k = int(clusters.centres.size());
  std::vector<int> seed_label(nv, -1);
  for (size_t s = 0; s < seeds.size(); ++s) seed_label[seeds[s]] = clusters.label[s];
  // Robust region colour: weighted median of confidently observed members (avoids rim shading).
  std::vector<std::array<float, 4>> region(static_cast<size_t>(k));
  for (int c = 0; c < k; ++c) {
    std::array<float, 3> med{};
    for (int d = 0; d < 3; ++d) {
      std::vector<std::pair<float, double>> v;
      for (size_t s = 0; s < seeds.size(); ++s)
        if (clusters.label[s] == c && vobs[seeds[s]].conf >= 0.7f) v.push_back({lab[s][size_t(d)], lw[s]});
      if (v.empty())
        for (size_t s = 0; s < seeds.size(); ++s)
          if (clusters.label[s] == c) v.push_back({lab[s][size_t(d)], lw[s]});
      std::sort(v.begin(), v.end());
      double total = 0, run = 0;
      for (auto& e : v) total += e.second;
      for (auto& e : v) {
        run += e.second;
        if (run >= total / 2) { med[size_t(d)] = e.first; break; }
      }
    }
    region[size_t(c)] = linear_from_oklab(med);
  }
  deadline.check("colour regions");
  // Observations in a thin band along the picture's outline (cel / pixel-art outlines, dark rims) are
  // kept where they were seen, but as sources for hidden surfaces they start with a distance penalty:
  // interior materials win wherever they reach, so backs do not turn outline-black.
  std::vector<uint8_t> fixed(nv, 0), rim(nv, 0);
  size_t rim_seeds = 0;
  {
    std::vector<double> rim_px(pics.p.size());
    for (size_t k = 0; k < pics.p.size(); ++k) {
      const Source& ps = *pics.p[k].src;
      int sx0 = ps.width, sy0 = ps.height, sx1 = -1, sy1 = -1;
      for (int y = 0; y < ps.height; ++y)
        for (int x = 0; x < ps.width; ++x)
          if (ps.alpha(x, y) >= 0.5f) { sx0 = std::min(sx0, x); sy0 = std::min(sy0, y); sx1 = std::max(sx1, x); sy1 = std::max(sy1, y); }
      rim_px[k] = std::max(3.0, 0.015 * std::max(sx1 - sx0 + 1, sy1 - sy0 + 1));
    }
    for (auto s : seeds) {
      fixed[s] = 1;
      const auto& P = pics.p[size_t(std::max<int8_t>(0, vdom[s]))];
      const vec2d q = P.pose.to_image(L.positions[s]);
      const int x = std::clamp(int(q.x), 0, P.view.w - 1), y = std::clamp(int(q.y), 0, P.view.h - 1);
      if (P.view.silhouette[size_t(y) * P.view.w + x] < rim_px[size_t(std::max<int8_t>(0, vdom[s]))]) { rim[s] = 1; ++rim_seeds; }
    }
  }
  const double rim_penalty = 0.05 * view.diag;
  // 3. Geodesic nearest observation along mesh edges (never jumps between parts or through the body).
  std::vector<double> dist(nv, std::numeric_limits<double>::infinity());
  std::vector<uint32_t> origin(nv, UINT32_MAX);
  using Node = std::pair<double, uint32_t>;
  std::priority_queue<Node, std::vector<Node>, std::greater<Node>> queue;
  for (auto s : seeds) {
    dist[s] = rim[s] ? rim_penalty : 0;
    origin[s] = s;
    queue.push({dist[s], s});
  }
  while (!queue.empty()) {
    const auto [d, i] = queue.top();
    queue.pop();
    if (d != dist[i]) continue;
    for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
      const uint32_t j = adj[e];
      const double nd = d + length(L.positions[i] - L.positions[j]);
      if (nd < dist[j]) {
        dist[j] = nd;
        origin[j] = origin[i];
        queue.push({nd, j});
      }
    }
  }
  // Components with no observation: spatially nearest observation, flagged as a low-confidence fallback.
  std::vector<uint8_t> fallback(nv, 0);
  size_t fallback_vertices = 0;
  size_t fallback_components = 0;
  {
    const auto& sources = seeds;
    const size_t seed_step = std::max<size_t>(1, sources.size() / 4000);
    for (size_t start = 0; start < nv; ++start) {
      if (std::isfinite(dist[start]) || fallback[start]) continue;
      std::vector<uint32_t> comp{uint32_t(start)};
      fallback[start] = 1;
      for (size_t q = 0; q < comp.size(); ++q)
        for (uint32_t e = deg[comp[q]]; e < deg[comp[q] + 1]; ++e)
          if (!std::isfinite(dist[adj[e]]) && !fallback[adj[e]]) {
            fallback[adj[e]] = 1;
            comp.push_back(adj[e]);
          }
      // One observation per component (nearest in space), so a hidden part reads as one material.
      const size_t comp_step = std::max<size_t>(1, comp.size() / 256);
      uint32_t best = sources.front();
      double bd = 1e300;
      for (size_t a = 0; a < comp.size(); a += comp_step)
        for (size_t b = 0; b < sources.size(); b += seed_step) {
          const double d = length(L.positions[sources[b]] - L.positions[comp[a]]);
          if (d < bd) { bd = d; best = sources[b]; }
        }
      for (auto v : comp) {
        origin[v] = best;
        dist[v] = std::max(length(L.positions[best] - L.positions[v]), 3 * 0.03 * view.diag);
      }
      fallback_vertices += comp.size();
      ++fallback_components;
    }
  }
  deadline.check("hidden-surface propagation");
  // 4. Region of each hidden vertex: start from the nearest observation, then a majority (mode) filter
  //    with observations fixed, so thin visible strips (a collar, an ear) do not run across hidden areas.
  std::vector<int> label(nv);
  for (size_t i = 0; i < nv; ++i) label[i] = fixed[i] ? seed_label[i] : seed_label[origin[i]];
  if (k > 1) {
    std::vector<float> prob(nv * size_t(k), 0.f), next(nv * size_t(k));
    for (size_t i = 0; i < nv; ++i) prob[i * size_t(k) + size_t(label[i])] = 1.f;
    for (int it = 0; it < 160; ++it) {
      for (size_t i = 0; i < nv; ++i) {
        float* out = &next[i * size_t(k)];
        if (fixed[i]) { std::copy_n(&prob[i * size_t(k)], k, out); continue; }
        std::copy_n(&prob[i * size_t(k)], k, out);
        for (uint32_t e = deg[i]; e < deg[i + 1]; ++e)
          for (int c = 0; c < k; ++c) out[c] += prob[size_t(adj[e]) * size_t(k) + size_t(c)];
        const float n = 1.f + float(deg[i + 1] - deg[i]);
        for (int c = 0; c < k; ++c) out[c] /= n;
      }
      prob.swap(next);
      if (it % 32 == 31) deadline.check("hidden-surface regions");
    }
    for (size_t i = 0; i < nv; ++i)
      if (!fixed[i]) label[i] = int(std::max_element(&prob[i * size_t(k)], &prob[i * size_t(k)] + k) - &prob[i * size_t(k)]);
  }
  // 5a. Verified bilateral symmetry: a hidden vertex whose mirror vertex was seen takes that side's
  //     colour region and a low-pass of its colour (no mirrored text or fine detail).
  //     A plane facing the camera would copy the seen side onto the far side (a mirrored front): never used.
  Symmetry sym = detect_symmetry(L, view.diag, deadline);
  const bool sym_usable = sym.accepted && std::abs(dot(sym.normal, pose.forward)) < 0.8;
  sym.evidence["used_for_hidden_surfaces"] = sym_usable;
  if (sym.accepted && !sym_usable) sym.evidence["not_used_because"] = "the plane faces the camera: it would mirror the seen side onto the far side";
  std::vector<std::array<float, 4>> symc(nv, {0, 0, 0, 0});
  std::vector<float> symw(nv, 0.f);
  size_t symmetric_vertices = 0;
  if (sym_usable) {
    for (size_t i = 0; i < nv; ++i) {
      const int32_t w = sym.partner[i];
      if (fixed[i] || w < 0 || !fixed[size_t(w)]) continue;
      symc[i] = vobs[size_t(w)].rgb;
      symw[i] = float(vobs[size_t(w)].conf * sym.quality);
    }
    for (int it = 0; it < 16; ++it) {  // low-pass over ~16 edge rings
      auto next = symc;
      for (size_t i = 0; i < nv; ++i) {
        if (symw[i] <= 0) continue;
        std::array<double, 3> acc{symc[i][0] * symw[i], symc[i][1] * symw[i], symc[i][2] * symw[i]};
        double wsum = symw[i];
        for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
          const uint32_t j = adj[e];
          if (symw[j] <= 0) continue;
          for (int d = 0; d < 3; ++d) acc[size_t(d)] += symc[j][size_t(d)] * symw[j];
          wsum += symw[j];
        }
        for (int d = 0; d < 3; ++d) next[i][size_t(d)] = float(acc[size_t(d)] / wsum);
      }
      symc.swap(next);
    }
    for (size_t i = 0; i < nv; ++i)
      if (symw[i] > 0.3f) {
        label[i] = seed_label[size_t(sym.partner[i])];
        ++symmetric_vertices;
      }
  }
  // 5b. Hidden colour: boundary colour fading into the region's colour (or the mirror side's
  //     low-pass colour where the shape is verified symmetric); smoothed within each region.
  const double sigma = 0.03 * view.diag;
  std::vector<std::array<float, 4>> hidden(nv);
  for (size_t i = 0; i < nv; ++i) {
    const uint32_t o = origin[i];
    if (fixed[i]) {
      hidden[i] = vobs[i].rgb;
      continue;
    }
    const double t = seed_label[o] == label[i] ? std::exp(-dist[i] / sigma) : 0.0;
    const auto& r = region[size_t(label[i])];
    const auto& b = vobs[o].rgb;
    hidden[i] = {float(r[0] + (b[0] - r[0]) * t), float(r[1] + (b[1] - r[1]) * t), float(r[2] + (b[2] - r[2]) * t), 1.f};
    if (symw[i] > 0) {
      const double k = 0.8 * symw[i];
      for (int d = 0; d < 3; ++d) hidden[i][size_t(d)] = float(hidden[i][size_t(d)] * (1 - k) + symc[i][size_t(d)] * k);
    }
  }
  {
    auto next = hidden;
    for (int it = 0; it < 24; ++it) {
      for (size_t i = 0; i < nv; ++i) {
        if (fixed[i]) continue;
        std::array<double, 3> s{};
        double w = 0;
        for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
          const uint32_t j = adj[e];
          if (label[j] != label[i]) continue;  // hard barrier between colour regions
          for (int d = 0; d < 3; ++d) s[size_t(d)] += hidden[j][size_t(d)];
          w += 1;
        }
        if (w > 0)
          for (int d = 0; d < 3; ++d) next[i][size_t(d)] = float(0.5 * hidden[i][size_t(d)] + 0.5 * s[size_t(d)] / w);
      }
      hidden.swap(next);
    }
  }
  deadline.check("hidden-surface smoothing");
  // 5b'. Extra pictures (multi-angle): next to surface a real picture of another angle shows, TRELLIS's guess
  //      gives way to the colour spread from that picture (the reconstruction above), fading out over 3% of the
  //      object's size: TRELLIS painted the back from the front only, so its colour there is the least reliable.
  std::vector<float> extra_fill(nv, 0.f);
  if (pics.p.size() > 1) {
    std::vector<double> d(nv, std::numeric_limits<double>::infinity());
    std::priority_queue<std::pair<double, uint32_t>, std::vector<std::pair<double, uint32_t>>, std::greater<>> q;
    for (auto s : seeds)
      if (vdom[s] > 0) { d[s] = 0; q.push({0, s}); }
    const double reach = 0.03 * view.diag;
    while (!q.empty()) {
      const auto [d0, i] = q.top();
      q.pop();
      if (d0 != d[i] || d0 > 3 * reach) continue;
      for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
        const uint32_t j = adj[e];
        const double nd = d0 + length(L.positions[i] - L.positions[j]);
        if (nd < d[j]) { d[j] = nd; q.push({nd, j}); }
      }
    }
    for (size_t i = 0; i < nv; ++i)
      if (std::isfinite(d[i]) && !fixed[i] && vdom[origin[i]] > 0) extra_fill[i] = float(std::exp(-d[i] / reach));
  }
  // 5c. Hybrid: TRELLIS's own texture of the whole object under the picture. Calibrated to the picture's
  //     colours where the picture sees the surface squarely, then used for everything the picture does not
  //     show; the geodesic reconstruction above remains only where the volume has no voxel nearby.
  const NativeVolume* native = opt.native;
  nlohmann::json native_ev;
  ColourMap colour_map;
  std::vector<std::array<float, 3>> palette;
  std::vector<int32_t> vertex_colour;  // pixel art: palette entry per vertex (-1: none)
  std::vector<float> native_low_l;     // low-passed calibrated TRELLIS lightness per vertex (detail transfer)
  std::vector<std::array<float, 3>> mat_lo, mat_hi, mat_med;  // material lock: per picture colour region
  std::vector<std::array<float, 3>> mat_shift;                 // material match: hidden -> seen mean, per material
  std::vector<int32_t> vertex_mat;                             // material per vertex (-1: none)
  std::vector<uint8_t> vertex_mode;    // material lock: 0 none, 1 within its material, 2 unseen colour kept, 3 island
  std::vector<std::array<float, 3>> island_lab;
  std::vector<std::array<float, 3>> vertex_cal;  // calibrated TRELLIS colour per vertex (material lock)
  std::vector<float> island_mean_l;
  if (native) {
    std::vector<std::array<float, 3>> nat(nv);
    std::vector<uint8_t> nat_ok(nv, 0);
    size_t direct = 0, any = 0;
    parallel_rows(int(nv), [&](int a, int b) {
      for (int i = a; i < b; ++i) {
        bool d = false;
        nat_ok[size_t(i)] = native->sample(L.positions[size_t(i)], nat[size_t(i)], &d) ? (d ? 2 : 1) : 0;
      }
    });
    for (auto v : nat_ok) { direct += v == 2; any += v != 0; }
    const double aligned = double(direct) / double(std::max<size_t>(nv, 1));
    native_ev = {{"volume", native->evidence}, {"vertices_with_direct_sample", aligned}, {"vertices_with_any_sample", double(any) / double(std::max<size_t>(nv, 1))}};
    // A volume from another generation (or another frame) misses most of this surface: never forced.
    if (aligned < 0.5) {
      native_ev["used"] = false;
      native_ev["not_used_because"] = "the texture volume does not line up with this geometry (direct samples on " + std::to_string(int(aligned * 100)) + "% of vertices)";
      native = nullptr;
    } else {
      std::vector<std::array<float, 3>> nl, pl;
      std::vector<double> pw;
      const size_t step = std::max<size_t>(1, seeds.size() / 30000);
      for (size_t s = 0; s < seeds.size(); s += step) {
        const auto i = seeds[s];
        if (vobs[i].conf < 0.7f || nat_ok[i] != 2) continue;
        nl.push_back(oklab({nat[i][0], nat[i][1], nat[i][2], 1.f}));
        pl.push_back(oklab(vobs[i].rgb));
        pw.push_back(varea[i] * vobs[i].conf);
      }
      nlohmann::json cal;
      colour_map = fit_colour_map(nl, pl, pw, cal);
      native_ev["calibration"] = cal;
      native_ev["used"] = true;
      if (opt.multiview) {  // generated views give colour and broad tone; TRELLIS keeps its fine light/dark detail
        native_low_l.assign(nv, 0.f);
        for (size_t i = 0; i < nv; ++i)
          if (nat_ok[i]) native_low_l[i] = colour_map.apply(oklab({nat[i][0], nat[i][1], nat[i][2], 1.f}))[0];
        std::vector<float> next(nv);
        for (int it = 0; it < 12; ++it) {
          for (size_t i = 0; i < nv; ++i) {
            double acc = native_low_l[i] * 2.0, n = 2;
            for (uint32_t e = deg[i]; e < deg[i + 1]; ++e)
              if (nat_ok[adj[e]]) { acc += native_low_l[adj[e]]; n += 1; }
            next[i] = nat_ok[i] ? float(acc / n) : native_low_l[i];
          }
          native_low_l.swap(next);
        }
      }
      // Material lock (2.3): TRELLIS paints some hidden surfaces with another part's colour (orange streaks in
      // black hair, pale blotches and a face on a dark cloak). The picture's colour regions are its
      // materials; a hidden vertex keeps its TRELLIS colour only when that colour fits a material AND a chain
      // of vertices fitting the same material connects it to where the picture shows that material.
      // Unconnected islands take the surrounding material, keeping only TRELLIS's light/dark detail.
      if (opt.material_lock && k >= 1) {
        // Material boxes: robust per-channel ranges (3rd-97th weighted percentile) and the median.
        mat_lo.assign(size_t(k), {0, 0, 0});
        mat_hi.assign(size_t(k), {0, 0, 0});
        mat_med.assign(size_t(k), {0, 0, 0});
        for (int c = 0; c < k; ++c)
          for (int d = 0; d < 3; ++d) {
            std::vector<std::pair<float, double>> v;
            for (size_t si = 0; si < seeds.size(); ++si)
              if (clusters.label[si] == c) v.push_back({lab[si][size_t(d)], lw[si]});
            std::sort(v.begin(), v.end());
            double total = 0;
            for (auto& e : v) total += e.second;
            auto pct = [&](double q) {
              double run = 0;
              for (auto& e : v) {
                run += e.second;
                if (run >= q * total) return e.first;
              }
              return v.empty() ? 0.f : v.back().first;
            };
            const float margin = d == 0 ? 0.03f : 0.015f;
            mat_lo[size_t(c)][size_t(d)] = pct(0.03) - margin;
            mat_hi[size_t(c)][size_t(d)] = pct(0.97) + margin;
            mat_med[size_t(c)][size_t(d)] = pct(0.5);
          }
        std::vector<std::array<float, 3>> cal_lab(nv, {0, 0, 0});
        for (size_t i = 0; i < nv; ++i)
          if (nat_ok[i]) cal_lab[i] = colour_map.apply(oklab({nat[i][0], nat[i][1], nat[i][2], 1.f}));
        auto box_dist = [&](const std::array<float, 3>& x, int c) {
          double s2 = 0;
          for (int d = 0; d < 3; ++d) {
            const double v = std::clamp(x[size_t(d)], mat_lo[size_t(c)][size_t(d)], mat_hi[size_t(c)][size_t(d)]) - x[size_t(d)];
            s2 += v * v;
          }
          return std::sqrt(s2);
        };
        const double tau = 0.06;
        std::vector<float> best_fit(nv, 1e9f);
        for (size_t i = 0; i < nv; ++i)
          if (nat_ok[i])
            for (int c = 0; c < k; ++c) best_fit[i] = std::min(best_fit[i], float(box_dist(cal_lab[i], c)));
        // Chains: Dijkstra from the picture's confident observations, only through fitting hidden vertices.
        vertex_mat.assign(nv, -1);
        vertex_mode.assign(nv, 0);
        std::vector<double> dd(nv, std::numeric_limits<double>::infinity());
        std::priority_queue<std::pair<double, uint32_t>, std::vector<std::pair<double, uint32_t>>, std::greater<>> q;
        for (size_t i = 0; i < nv; ++i)
          if (fixed[i]) {
            vertex_mat[i] = seed_label[i];
            dd[i] = 0;
            q.push({0, uint32_t(i)});
          }
        while (!q.empty()) {
          const auto [d0, i] = q.top();
          q.pop();
          if (d0 != dd[i]) continue;
          const int c = vertex_mat[i];
          for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
            const uint32_t j = adj[e];
            if (fixed[j] || !nat_ok[j]) continue;
            const double bd = box_dist(cal_lab[j], c);
            if (bd > tau || bd > best_fit[j] + 0.02) continue;
            const double nd = d0 + length(L.positions[i] - L.positions[j]);
            if (nd < dd[j]) {
              dd[j] = nd;
              vertex_mat[j] = c;
              q.push({nd, j});
            }
          }
        }
        // Colours the picture never shows: a large connected region is TRELLIS's guess of an unseen part
        // (kept as painted); small patches of them are blotches and smudges (reassigned like islands).
        std::vector<uint8_t> keep(nv, 0);
        size_t kept_v = 0;
        {
          double total_area = 0;
          for (size_t i = 0; i < nv; ++i) total_area += varea[i];
          std::vector<uint8_t> seen(nv, 0);
          for (size_t s0 = 0; s0 < nv; ++s0) {
            if (seen[s0] || fixed[s0] || !nat_ok[s0] || vertex_mat[s0] >= 0 || best_fit[s0] <= tau) continue;
            std::vector<uint32_t> comp{uint32_t(s0)};
            seen[s0] = 1;
            double area = 0;
            for (size_t h = 0; h < comp.size(); ++h) {
              area += varea[comp[h]];
              for (uint32_t e = deg[comp[h]]; e < deg[comp[h] + 1]; ++e) {
                const uint32_t j = adj[e];
                if (!seen[j] && !fixed[j] && nat_ok[j] && vertex_mat[j] < 0 && best_fit[j] > tau) { seen[j] = 1; comp.push_back(j); }
              }
            }
            if (area >= 0.02 * total_area) {
              for (auto v : comp) keep[v] = 1;
              kept_v += comp.size();
            }
          }
        }
        // Islands: every remaining hidden vertex is inpainted from what surrounds it (nearest non-island
        // colour along the surface: the picture where it is seen, the locked TRELLIS colour elsewhere), then
        // smoothed within the island; TRELLIS keeps only 30% of its light/dark detail there.
        size_t chained = 0, islands_v = 0;
        for (size_t i = 0; i < nv; ++i) {
          if (fixed[i] || !nat_ok[i]) continue;
          if (keep[i]) vertex_mode[i] = 2;
          else if (vertex_mat[i] >= 0) { vertex_mode[i] = 1; ++chained; }
          else { vertex_mode[i] = 3; ++islands_v; }
        }
        // Thin streaks (2.4): a chained material may still reach into another material's area as narrow
        // stripes (orange streaks from the collar running up into black back hair). Opening per material on
        // the hidden surface: erode by r (distance to any vertex of another material), dilate back by r through
        // the material; what does not come back is narrower than 2r and becomes an island.
        size_t streak_v = 0;
        const double open_r = opt.streak_radius * view.diag;
        if (open_r > 0 && k > 1) {
          std::vector<double> dout(nv), din(nv);
          for (int c = 0; c < k; ++c) {
            auto member = [&](size_t i) { return fixed[i] ? seed_label[i] == c : vertex_mode[i] == 1 && vertex_mat[i] == c; };
            bool any = false;
            for (size_t i = 0; i < nv && !any; ++i) any = !fixed[i] && vertex_mode[i] == 1 && vertex_mat[i] == c;
            if (!any) continue;
            // Erosion: distance from each member to the nearest non-member.
            std::fill(dout.begin(), dout.end(), std::numeric_limits<double>::infinity());
            std::priority_queue<std::pair<double, uint32_t>, std::vector<std::pair<double, uint32_t>>, std::greater<>> qq;
            for (size_t i = 0; i < nv; ++i)
              if (!member(i)) { dout[i] = 0; qq.push({0, uint32_t(i)}); }
            while (!qq.empty()) {
              const auto [d0, i] = qq.top();
              qq.pop();
              if (d0 != dout[i] || d0 > open_r) continue;
              for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
                const uint32_t j = adj[e];
                const double nd = d0 + length(L.positions[i] - L.positions[j]);
                if (nd < dout[j]) { dout[j] = nd; qq.push({nd, j}); }
              }
            }
            // Dilation back from the members that survive erosion (or are seen), through members only.
            std::fill(din.begin(), din.end(), std::numeric_limits<double>::infinity());
            for (size_t i = 0; i < nv; ++i)
              if (member(i) && (fixed[i] || dout[i] > open_r)) { din[i] = 0; qq.push({0, uint32_t(i)}); }
            while (!qq.empty()) {
              const auto [d0, i] = qq.top();
              qq.pop();
              if (d0 != din[i] || d0 > open_r) continue;
              for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
                const uint32_t j = adj[e];
                if (!member(j)) continue;
                const double nd = d0 + length(L.positions[i] - L.positions[j]);
                if (nd < din[j]) { din[j] = nd; qq.push({nd, j}); }
              }
            }
            for (size_t i = 0; i < nv; ++i)
              if (!fixed[i] && vertex_mode[i] == 1 && vertex_mat[i] == c && din[i] > open_r) {
                vertex_mode[i] = 3;
                ++streak_v;
              }
          }
          chained -= std::min(chained, streak_v);
          islands_v += streak_v;
        }
        // Material match (agent M2, B4): TRELLIS often paints the hidden part of a material duller or browner
        // than the picture shows it (the anime-style character's orange back). Per material, the hidden chained vertices' mean colour
        // is moved onto the mean of the same material where the picture shows it; TRELLIS's own variation
        // around that mean (folds, shading detail) is kept, and the result stays inside the material's box.
        mat_shift.assign(size_t(k), {0, 0, 0});
        nlohmann::json match_table = nlohmann::json::array();
        {
          std::vector<std::array<double, 4>> front(size_t(k), {0, 0, 0, 0}), back(size_t(k), {0, 0, 0, 0});
          for (size_t si = 0; si < seeds.size(); ++si) {
            const int c = clusters.label[si];
            if (c < 0 || c >= k) continue;
            for (int d = 0; d < 3; ++d) front[size_t(c)][size_t(d)] += lab[si][size_t(d)] * lw[si];
            front[size_t(c)][3] += lw[si];
          }
          auto boxed = [&](const std::array<float, 3>& x, int c, const std::array<float, 3>& sh) {
            std::array<float, 3> b;
            for (int d = 0; d < 3; ++d) b[size_t(d)] = std::clamp(x[size_t(d)] + sh[size_t(d)], mat_lo[size_t(c)][size_t(d)], mat_hi[size_t(c)][size_t(d)]);
            return b;
          };
          std::vector<size_t> count(size_t(k), 0);
          for (size_t i = 0; i < nv; ++i) {
            if (fixed[i] || !nat_ok[i] || vertex_mode[i] != 1) continue;
            const int c = vertex_mat[i];
            const auto& x = cal_lab[i];  // unclamped: the shift is applied before the box clamp
            for (int d = 0; d < 3; ++d) back[size_t(c)][size_t(d)] += x[size_t(d)] * varea[i];
            back[size_t(c)][3] += varea[i];
            ++count[size_t(c)];
          }
          auto de3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
          };
          for (int c = 0; c < k; ++c) {
            if (front[size_t(c)][3] <= 0 || back[size_t(c)][3] <= 0 || count[size_t(c)] < 20) continue;
            std::array<double, 3> fm, bm;
            for (int d = 0; d < 3; ++d) {
              fm[size_t(d)] = front[size_t(c)][size_t(d)] / front[size_t(c)][3];
              bm[size_t(d)] = back[size_t(c)][size_t(d)] / back[size_t(c)][3];
            }
            if (opt.material_match)
              for (int d = 0; d < 3; ++d) mat_shift[size_t(c)][size_t(d)] = float(fm[size_t(d)] - bm[size_t(d)]);
            std::array<double, 4> after{0, 0, 0, 0}, locked{0, 0, 0, 0};
            for (size_t i = 0; i < nv; ++i) {
              if (fixed[i] || !nat_ok[i] || vertex_mode[i] != 1 || vertex_mat[i] != c) continue;
              const auto x = boxed(cal_lab[i], c, mat_shift[size_t(c)]), y = boxed(cal_lab[i], c, {0, 0, 0});
              for (int d = 0; d < 3; ++d) {
                after[size_t(d)] += x[size_t(d)] * varea[i];
                locked[size_t(d)] += y[size_t(d)] * varea[i];
              }
              after[3] += varea[i];
              locked[3] += varea[i];
            }
            const std::array<double, 3> am{after[0] / after[3], after[1] / after[3], after[2] / after[3]};
            bm = {locked[0] / locked[3], locked[1] / locked[3], locked[2] / locked[3]};  // reported "before" = the lock alone
            auto chroma = [](const std::array<double, 3>& x) { return std::sqrt(x[1] * x[1] + x[2] * x[2]); };
            match_table.push_back({{"median", hex(linear_from_oklab(mat_med[size_t(c)]))}, {"hidden_vertices", count[size_t(c)]},
                                   {"front_mean_lab", fm}, {"hidden_mean_lab_before", bm}, {"hidden_mean_lab_after", am},
                                   {"delta_e_before", de3(fm, bm)}, {"delta_e_after", de3(fm, am)},
                                   {"lightness_before", bm[0] - fm[0]}, {"lightness_after", am[0] - fm[0]},
                                   {"chroma_ratio_before", chroma(fm) > 1e-6 ? chroma(bm) / chroma(fm) : 1.0},
                                   {"chroma_ratio_after", chroma(fm) > 1e-6 ? chroma(am) / chroma(fm) : 1.0}});
          }
          native_ev["material_match"] = {{"applied", opt.material_match}, {"materials", match_table},
                                         {"rule", "per picture material: hidden chained vertices' area-weighted mean Oklab moved onto the mean of the "
                                                  "picture's own pixels of that material; TRELLIS variation around it kept; clamped to the material box; "
                                                  "materials with < 20 hidden vertices unchanged"}};
        }
        auto locked_vertex = [&](uint32_t i) -> std::array<float, 3> {
          if (fixed[i] || !nat_ok[i]) return oklab(vobs[i].rgb);
          if (vertex_mode[i] == 2 || vertex_mode[i] == 4) return cal_lab[i];
          std::array<float, 3> x = cal_lab[i];
          const int c = vertex_mat[i];
          for (int d = 0; d < 3; ++d) x[size_t(d)] = std::clamp(x[size_t(d)] + mat_shift[size_t(c)][size_t(d)], mat_lo[size_t(c)][size_t(d)], mat_hi[size_t(c)][size_t(d)]);
          return x;
        };
        vertex_cal = cal_lab;
        // Kept vertices next to an island: their texels must not carry the island's colour into the kept region.
        for (size_t i = 0; i < nv; ++i) {
          if (vertex_mode[i] != 2) continue;
          for (uint32_t e = deg[i]; e < deg[i + 1]; ++e)
            if (vertex_mode[adj[e]] == 3) { vertex_mode[i] = 4; break; }
        }
        island_lab.assign(nv, {0, 0, 0});
        {
          std::vector<double> d2(nv, std::numeric_limits<double>::infinity());
          std::vector<uint32_t> from(nv, UINT32_MAX);
          for (size_t i = 0; i < nv; ++i)
            if (vertex_mode[i] != 3) {  // outline-band observations start behind (pixel-art outlines are not a fill colour)
              d2[i] = rim[i] ? rim_penalty : 0;
              from[i] = uint32_t(i);
              q.push({d2[i], uint32_t(i)});
            }
          while (!q.empty()) {
            const auto [d0, i] = q.top();
            q.pop();
            if (d0 != d2[i]) continue;
            for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
              const uint32_t j = adj[e];
              if (vertex_mode[j] != 3) continue;
              const double nd = d0 + length(L.positions[i] - L.positions[j]);
              if (nd < d2[j]) { d2[j] = nd; from[j] = from[i]; q.push({nd, j}); }
            }
          }
          for (size_t i = 0; i < nv; ++i)
            if (vertex_mode[i] == 3) island_lab[i] = from[i] != UINT32_MAX ? locked_vertex(from[i]) : cal_lab[i];
          auto next = island_lab;
          for (int it = 0; it < 12; ++it) {
            for (size_t i = 0; i < nv; ++i) {
              if (vertex_mode[i] != 3) continue;
              std::array<double, 3> acc{island_lab[i][0] * 2.0, island_lab[i][1] * 2.0, island_lab[i][2] * 2.0};
              double n = 2;
              for (uint32_t e = deg[i]; e < deg[i + 1]; ++e) {
                const uint32_t j = adj[e];
                const auto c = vertex_mode[j] == 3 ? island_lab[j] : locked_vertex(j);
                for (int d = 0; d < 3; ++d) acc[size_t(d)] += c[size_t(d)];
                n += 1;
              }
              for (int d = 0; d < 3; ++d) next[i][size_t(d)] = float(acc[size_t(d)] / n);
            }
            island_lab.swap(next);
          }
        }
        // Island light/dark detail around each island's own mean L (connected components of island vertices).
        island_mean_l.assign(nv, 0.f);
        {
          std::vector<uint8_t> seen(nv, 0);
          for (size_t s0 = 0; s0 < nv; ++s0) {
            if (vertex_mode[s0] != 3 || seen[s0]) continue;
            std::vector<uint32_t> comp{uint32_t(s0)};
            seen[s0] = 1;
            for (size_t h = 0; h < comp.size(); ++h)
              for (uint32_t e = deg[comp[h]]; e < deg[comp[h] + 1]; ++e) {
                const uint32_t j = adj[e];
                if (vertex_mode[j] == 3 && !seen[j]) { seen[j] = 1; comp.push_back(j); }
              }
            double m = 0;
            for (auto v : comp) m += cal_lab[v][0];
            m /= double(comp.size());
            for (auto v : comp) island_mean_l[v] = float(m);
          }
        }
        native_ev["material_lock"] = {{"materials", k}, {"fit_tolerance", tau}, {"thin_streak_vertices_reassigned", streak_v},
                                      {"material_table", [&] {
                                         nlohmann::json t = nlohmann::json::array();
                                         for (int c = 0; c < k; ++c) {
                                           size_t n = 0;
                                           for (size_t i = 0; i < nv; ++i) n += !fixed[i] && vertex_mode[i] == 1 && vertex_mat[i] == c;
                                           const auto m = linear_from_oklab(mat_med[size_t(c)]);
                                           t.push_back({{"median", hex(m)}, {"hidden_vertices", n},
                                                        {"box_lo", {mat_lo[size_t(c)][0], mat_lo[size_t(c)][1], mat_lo[size_t(c)][2]}},
                                                        {"box_hi", {mat_hi[size_t(c)][0], mat_hi[size_t(c)][1], mat_hi[size_t(c)][2]}}});
                                         }
                                         return t;
                                       }()}, {"streak_radius", open_r},
                                      {"hidden_vertices_connected", double(chained) / double(std::max<size_t>(chained + islands_v + kept_v, 1))},
                                      {"hidden_vertices_reassigned", double(islands_v) / double(std::max<size_t>(chained + islands_v + kept_v, 1))},
                                      {"hidden_vertices_unseen_colour_kept", double(kept_v) / double(std::max<size_t>(chained + islands_v + kept_v, 1))},
                                      {"rule", "TRELLIS colour kept where it fits a picture material connected to where the picture shows it, or where it is a colour the picture never shows over >= 2% of the surface; other islands take the surrounding material with TRELLIS light/dark detail"}};
      }
      if (src.crisp) {
        nlohmann::json pe;
        palette = picture_palette(src, 64, 32, pe);
        native_ev["palette"] = pe;
        // Palette regions are decided per vertex on the lightly smoothed calibrated colour, so they follow
        // the surface instead of flickering texel by texel between two neighbouring palette entries.
        std::vector<std::array<float, 3>> lab(nv), next(nv);
        for (size_t i = 0; i < nv; ++i) lab[i] = nat_ok[i] ? colour_map.apply(oklab({nat[i][0], nat[i][1], nat[i][2], 1.f})) : std::array<float, 3>{0, 0, 0};
        for (int it = 0; it < 2; ++it) {
          for (size_t i = 0; i < nv; ++i) {
            if (!nat_ok[i]) { next[i] = lab[i]; continue; }
            std::array<double, 3> acc{lab[i][0] * 2.0, lab[i][1] * 2.0, lab[i][2] * 2.0};
            double n = 2;
            for (uint32_t e = deg[i]; e < deg[i + 1]; ++e)
              if (nat_ok[adj[e]]) {
                for (int d = 0; d < 3; ++d) acc[size_t(d)] += lab[adj[e]][size_t(d)];
                n += 1;
              }
            for (int d = 0; d < 3; ++d) next[i][size_t(d)] = float(acc[size_t(d)] / n);
          }
          lab.swap(next);
        }
        vertex_colour.assign(nv, -1);
        for (size_t i = 0; i < nv; ++i)
          if (nat_ok[i]) {
            std::array<float, 3> x = lab[i];
            if (!vertex_mode.empty()) {
              if (vertex_mode[i] == 1)
                for (int d = 0; d < 3; ++d)
                  x[size_t(d)] = std::clamp(x[size_t(d)] + mat_shift[size_t(vertex_mat[i])][size_t(d)], mat_lo[size_t(vertex_mat[i])][size_t(d)],
                                            mat_hi[size_t(vertex_mat[i])][size_t(d)]);
              else if (vertex_mode[i] == 3) {
                const float l = std::clamp(island_lab[i][0] + 0.3f * (x[0] - island_mean_l[i]), 0.f, 1.f);
                x = {l, island_lab[i][1], island_lab[i][2]};
              }
            }
            vertex_colour[i] = int32_t(nearest_colour(palette, x));
          }
      }
    }
    deadline.check("native texture calibration");
  }
  // 6. Texels: observed colour where the camera saw the surface, reconstruction elsewhere, blended by confidence.
  const int S = atlas.size;
  const auto& R = atlas.mesh;
  std::vector<std::array<float, 3>> texel(size_t(S) * S);
  // Glass-like objects: opacity from TRELLIS's own volume (the picture's alpha is the cutout, not the glass).
  const bool translucent = native && native->translucent;
  std::vector<float> opacity(translucent ? size_t(S) * S : 0, 1.f);
  std::vector<uint8_t> state(size_t(S) * S, 0);  // 0 empty, 1 observed, 2 blended, 3 reconstructed, 4 fallback, 5 native, 6 generated view
  std::vector<vec3d> face_n(L.tris.size());
  std::vector<uint8_t> face_fb(L.tris.size());
  for (size_t f = 0; f < L.tris.size(); ++f) {
    const auto& t = L.tris[f];
    const vec3d c = cross(L.positions[t[1]] - L.positions[t[0]], L.positions[t[2]] - L.positions[t[0]]);
    const double l = length(c);
    face_n[f] = l > 0 ? c / l : vec3d{0, 0, 1};
    face_fb[f] = fallback[t[0]] && fallback[t[1]] && fallback[t[2]];
  }
  // 5d. Generated views (multi-view diffusion from the picture, on this geometry): calibrated to the picture
  //     where the picture sees the surface squarely, then laid over the native layer where a view sees it.
  const MultiViewSet* mv = opt.multiview;
  nlohmann::json mv_ev;
  ColourMap mv_map;
  if (mv) {
    std::vector<std::array<float, 3>> ml, pl;
    std::vector<double> pw;
    const size_t step = std::max<size_t>(1, seeds.size() / 30000);
    for (size_t s = 0; s < seeds.size(); s += step) {
      const auto i = seeds[s];
      std::array<float, 3> c;
      float w = 0, best = 0;
      if (vobs[i].conf < 0.7f || !mv->sample(L.positions[i], normals[i], normals[i], c, w, best) || best < 0.5f) continue;
      ml.push_back(oklab({c[0], c[1], c[2], 1.f}));
      pl.push_back(oklab(vobs[i].rgb));
      pw.push_back(varea[i] * vobs[i].conf);
    }
    // The views are generated FROM the picture, so they only need a gentle exposure/tint correction: a full
    // map fitted on the few colours the picture shows would distort the colours only the views show.
    nlohmann::json cal;
    if (ml.size() >= 64) {
      double wsum = 0, mean_de = 0;
      std::array<double, 3> mm{}, mp{}, vm{}, vp{};
      for (size_t i = 0; i < ml.size(); ++i) {
        const double w = pw[i];
        wsum += w;
        double d2 = 0;
        for (int d = 0; d < 3; ++d) {
          mm[size_t(d)] += ml[i][size_t(d)] * w;
          mp[size_t(d)] += pl[i][size_t(d)] * w;
          d2 += double(ml[i][size_t(d)] - pl[i][size_t(d)]) * (ml[i][size_t(d)] - pl[i][size_t(d)]);
        }
        mean_de += std::sqrt(d2) * w;
      }
      for (int d = 0; d < 3; ++d) { mm[size_t(d)] /= wsum; mp[size_t(d)] /= wsum; }
      mean_de /= wsum;
      for (size_t i = 0; i < ml.size(); ++i)
        for (int d = 0; d < 3; ++d) {
          vm[size_t(d)] += (ml[i][size_t(d)] - mm[size_t(d)]) * (ml[i][size_t(d)] - mm[size_t(d)]) * pw[i];
          vp[size_t(d)] += (pl[i][size_t(d)] - mp[size_t(d)]) * (pl[i][size_t(d)] - mp[size_t(d)]) * pw[i];
        }
      if (mean_de >= 0.03) {
        mv_map.method = "mean_std_clamped";
        for (int d = 0; d < 3; ++d) {
          const double g = d == 0 ? std::clamp(std::sqrt(vp[0] / std::max(vm[0], 1e-12)), 0.8, 1.25) : 1.0;
          mv_map.gain[size_t(d)] = g;
          mv_map.offset[size_t(d)] = std::clamp(mp[size_t(d)] - g * mm[size_t(d)], d == 0 ? -0.08 : -0.03, d == 0 ? 0.08 : 0.03);
        }
      }
      cal = {{"method", mv_map.method}, {"mean_delta_e_before", mean_de}, {"pairs", ml.size()},
             {"gain_l", mv_map.gain[0]}, {"offset", {mv_map.offset[0], mv_map.offset[1], mv_map.offset[2]}}};
    } else {
      cal = {{"method", "identity"}, {"why", "too few surface points seen by both the picture and a generated view"}};
    }
    mv_ev = {{"views", mv->evidence}, {"calibration", cal}, {"strength", opt.multiview_strength}};
    deadline.check("multi-view calibration");
  }
  auto lock_colour = [&](const std::array<float, 3>& x, uint32_t v) {
    switch (vertex_mode.empty() ? 0 : vertex_mode[v]) {
      case 1: {
        const int c = vertex_mat[v];
        std::array<float, 3> b;
        for (int d = 0; d < 3; ++d)
          b[size_t(d)] = std::clamp(x[size_t(d)] + (mat_shift.empty() ? 0.f : mat_shift[size_t(c)][size_t(d)]), mat_lo[size_t(c)][size_t(d)], mat_hi[size_t(c)][size_t(d)]);
        return b;
      }
      case 3: {  // inpainted from the surroundings, 30% of TRELLIS's light/dark detail
        auto b = island_lab[v];
        b[0] = std::clamp(b[0] + 0.3f * (x[0] - island_mean_l[v]), 0.f, 1.f);
        return b;
      }
      case 4: {  // kept, next to an island: texel detail around the vertex's own colour (no leak from the island)
        std::array<float, 3> b;
        for (int d = 0; d < 3; ++d) b[size_t(d)] = std::clamp(x[size_t(d)], vertex_cal[v][size_t(d)] - 0.06f, vertex_cal[v][size_t(d)] + 0.06f);
        return b;
      }
      default: return x;
    }
  };
  auto write = [&](size_t f, int x, int y, double w0, double w1, double w2) {
    const auto& t = L.tris[f];
    const vec3d p = L.positions[t[0]] * w0 + L.positions[t[1]] * w1 + L.positions[t[2]] * w2;
    const vec3d n = normalize(normals[t[0]] * w0 + normals[t[1]] * w1 + normals[t[2]] * w2);
    // Grazing fade (2.3): where the surface turns away from a camera one picture pixel is smeared over
    // 1/cos of surface — the brush-stroke streaks on shoulders and the sides of arms and legs. With a
    // TRELLIS layer underneath, each picture fades out there and the calibrated TRELLIS colour (or another
    // picture that sees the surface squarely) takes over.
    const double graze[2] = {opt.grazing_cos0, opt.grazing_cos1};
    const auto cb = pics.observe(p, n, face_n[f], opt.observe_cos, opt.observe_full_cos, native ? graze : nullptr, !src.crisp);
    const auto& o = cb.o;
    const double conf = o.conf;
    auto& out = texel[size_t(y) * S + x];
    std::array<float, 3> hid;
    uint8_t hidden_state;
    std::array<float, 3> nat;
    float native_detail = 0;  // TRELLIS's fine light/dark detail at this texel (0 without a volume)
    if (native && native->sample(p, nat)) {
      auto lab = colour_map.apply(oklab({nat[0], nat[1], nat[2], 1.f}));
      if (!native_low_l.empty())
        native_detail = std::clamp(lab[0] - float(native_low_l[t[0]] * w0 + native_low_l[t[1]] * w1 + native_low_l[t[2]] * w2), -0.15f, 0.15f);
      if (!vertex_mode.empty()) {
        const auto a = lock_colour(lab, t[0]), b = lock_colour(lab, t[1]), c = lock_colour(lab, t[2]);
        for (int d = 0; d < 3; ++d) lab[size_t(d)] = float(a[size_t(d)] * w0 + b[size_t(d)] * w1 + c[size_t(d)] * w2);
      }
      if (!palette.empty() && conf < 0.5) {  // pixel art: the picture's own colours, regions per vertex
        const uint32_t v = t[w0 >= w1 && w0 >= w2 ? 0 : w1 >= w2 ? 1 : 2];
        lab = palette[vertex_colour[v] >= 0 ? size_t(vertex_colour[v]) : nearest_colour(palette, lab)];
      }
      const auto c = linear_from_oklab(lab);
      hid = {c[0], c[1], c[2]};
      hidden_state = 5;
      const float fill = float(extra_fill[t[0]] * w0 + extra_fill[t[1]] * w1 + extra_fill[t[2]] * w2);
      if (fill > 0.01f)
        for (int d = 0; d < 3; ++d)
          hid[size_t(d)] = float((1 - fill) * hid[size_t(d)] + fill * (hidden[t[0]][size_t(d)] * w0 + hidden[t[1]][size_t(d)] * w1 + hidden[t[2]][size_t(d)] * w2));
    } else {
      for (int d = 0; d < 3; ++d)
        hid[size_t(d)] = float(hidden[t[0]][size_t(d)] * w0 + hidden[t[1]][size_t(d)] * w1 + hidden[t[2]][size_t(d)] * w2);
      hidden_state = face_fb[f] ? 4 : 3;
    }
    if (mv && conf < 0.9) {
      std::array<float, 3> c;
      float w = 0, best = 0;
      if (mv->sample(p, n, face_n[f], c, w, best)) {
        auto lab = mv_map.apply(oklab({c[0], c[1], c[2], 1.f}));
        lab[0] = std::clamp(lab[0] + 0.8f * native_detail, 0.f, 1.f);
        if (!palette.empty()) lab = palette[nearest_colour(palette, lab)];
        const auto m = linear_from_oklab(lab);
        const double a = opt.multiview_strength * smoothstep(0.05, 0.45, best);
        for (int d = 0; d < 3; ++d) hid[size_t(d)] = float(a * m[size_t(d)] + (1 - a) * hid[size_t(d)]);
        if (a >= 0.5) hidden_state = 6;
      }
    }
    // Picture-owned texels take the picture exactly (no TRELLIS haze), sampled with the sharper kernel.
    const std::array<float, 4> prgb = conf > 0 && !src.crisp ? cb.sharp : o.rgb;
    const double wp = conf >= 0.9 ? 1.0 : conf;
    for (int d = 0; d < 3; ++d) out[size_t(d)] = float(wp * prgb[size_t(d)] + (1 - wp) * hid[size_t(d)]);
    if (translucent) opacity[size_t(y) * S + x] = std::clamp(native->sample_alpha(p), 0.12f, 1.f);
    const uint8_t st = conf >= 0.9 ? 1 : conf >= 0.1 ? 2 : hidden_state;
    state[size_t(y) * S + x] = st;
    return st;
  };
  std::atomic<size_t> counts[7]{};
  parallel_rows(S, [&](int y0, int y1) {
    size_t local[7]{};
    for (size_t f = 0; f < L.tris.size(); ++f) {
      const uint32_t* r = &R.indices[3 * f];
      raster_uv(R.uvs[r[0]] * S, R.uvs[r[1]] * S, R.uvs[r[2]] * S, S, S, y0, y1,
                [&](int x, int y, double w0, double w1, double w2) { ++local[write(f, x, y, w0, w1, w2)]; });
    }
    // Border pass: texels next to a face that no face covers take that face's nearest surface point,
    // so faces thinner than a texel still own their texels (no colour pulled from another chart).
    for (size_t f = 0; f < L.tris.size(); ++f) {
      const uint32_t* r = &R.indices[3 * f];
      raster_uv_border(R.uvs[r[0]] * S, R.uvs[r[1]] * S, R.uvs[r[2]] * S, S, S, y0, y1, 1.0,
                       [&](int x, int y, double w0, double w1, double w2) {
                         if (!state[size_t(y) * S + x]) write(f, x, y, w0, w1, w2);
                       });
    }
    for (int i = 0; i < 7; ++i) counts[i] += local[i];
  });
  deadline.check("texel bake");
  // 7. Gutter: two rings of nearest dilation, then push-pull for every remaining empty texel, so linear
  //    filtering and mipmaps never pull in an unrelated colour.
  std::vector<uint8_t> filled(state.size());
  for (size_t i = 0; i < state.size(); ++i) filled[i] = state[i] != 0;
  for (int ring = 0; ring < 2; ++ring) {
    auto prev = filled;
    for (int y = 0; y < S; ++y)
      for (int x = 0; x < S; ++x) {
        const size_t i = size_t(y) * S + x;
        if (prev[i]) continue;
        std::array<double, 3> s{};
        int n = 0;
        for (auto [dx, dy] : std::array<std::pair<int, int>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= S || ny >= S || !prev[size_t(ny) * S + nx]) continue;
          for (int d = 0; d < 3; ++d) s[size_t(d)] += texel[size_t(ny) * S + nx][size_t(d)];
          ++n;
        }
        if (n) {
          for (int d = 0; d < 3; ++d) texel[i][size_t(d)] = float(s[size_t(d)] / n);
          if (translucent) {
            double a = 0;
            int m = 0;
            for (auto [dx, dy] : std::array<std::pair<int, int>, 4>{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}}) {
              const int nx = x + dx, ny = y + dy;
              if (nx < 0 || ny < 0 || nx >= S || ny >= S || !prev[size_t(ny) * S + nx]) continue;
              a += opacity[size_t(ny) * S + nx];
              ++m;
            }
            opacity[i] = float(a / m);
          }
          filled[i] = 1;
        }
      }
  }
  {
    struct Level { int s; std::vector<std::array<float, 4>> c; };  // premultiplied rgb + weight
    std::vector<Level> pyr;
    Level base{S, std::vector<std::array<float, 4>>(texel.size())};
    for (size_t i = 0; i < texel.size(); ++i)
      base.c[i] = filled[i] ? std::array<float, 4>{texel[i][0], texel[i][1], texel[i][2], 1.f} : std::array<float, 4>{0, 0, 0, 0};
    pyr.push_back(std::move(base));
    while (pyr.back().s > 1) {
      const auto& p = pyr.back();
      Level n{p.s / 2, std::vector<std::array<float, 4>>(size_t(p.s / 2) * (p.s / 2))};
      for (int y = 0; y < n.s; ++y)
        for (int x = 0; x < n.s; ++x) {
          std::array<float, 4> a{};
          for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
              for (int d = 0; d < 4; ++d) a[size_t(d)] += p.c[size_t(2 * y + dy) * p.s + 2 * x + dx][size_t(d)];
          n.c[size_t(y) * n.s + x] = a;
        }
      pyr.push_back(std::move(n));
    }
    for (size_t l = pyr.size() - 1; l > 0; --l) {
      const auto& coarse = pyr[l];
      auto& fine = pyr[l - 1];
      for (int y = 0; y < fine.s; ++y)
        for (int x = 0; x < fine.s; ++x) {
          auto& c = fine.c[size_t(y) * fine.s + x];
          if (c[3] >= 1) continue;
          const auto& up = coarse.c[size_t(std::min(y / 2, coarse.s - 1)) * coarse.s + std::min(x / 2, coarse.s - 1)];
          if (up[3] <= 0) continue;
          const float need = 1 - c[3];
          for (int d = 0; d < 3; ++d) c[size_t(d)] += up[size_t(d)] / up[3] * need;
          c[3] = 1;
        }
    }
    for (size_t i = 0; i < texel.size(); ++i)
      if (!filled[i] && pyr[0].c[i][3] > 0)
        for (int d = 0; d < 3; ++d) texel[i][size_t(d)] = pyr[0].c[i][size_t(d)] / pyr[0].c[i][3];
  }
  Bake out;
  out.mesh = R;
  auto image = std::make_shared<ImageRGBA>(S, S);
  out.provenance = ImageRGBA(S, S, {24, 24, 28, 255});
  const Rgba8 prov[7] = {{24, 24, 28, 255}, {40, 200, 90, 255}, {230, 210, 60, 255}, {240, 140, 40, 255}, {220, 50, 50, 255}, {80, 140, 235, 255}, {190, 90, 230, 255}};
  for (int y = 0; y < S; ++y)
    for (int x = 0; x < S; ++x) {
      const auto& c = texel[size_t(y) * S + x];
      const uint8_t a = translucent ? uint8_t(std::lround(std::clamp(opacity[size_t(y) * S + x], 0.f, 1.f) * 255)) : 255;
      image->at(x, y) = {srgb8_from_linear(c[0]), srgb8_from_linear(c[1]), srgb8_from_linear(c[2]), a};
      out.provenance.at(x, y) = prov[state[size_t(y) * S + x]];
    }
  out.mesh.texture = image;
  const double covered = double(counts[1] + counts[2] + counts[3] + counts[4] + counts[5] + counts[6]);
  nlohmann::json regions = nlohmann::json::array();
  for (int c = 0; c < k; ++c) regions.push_back(hex(region[size_t(c)]));
  out.evidence = {{"atlas_size", S}, {"observed_fraction", counts[1] / std::max(covered, 1.0)},
                  {"blended_fraction", counts[2] / std::max(covered, 1.0)}, {"reconstructed_fraction", counts[3] / std::max(covered, 1.0)},
                  {"fallback_fraction", counts[4] / std::max(covered, 1.0)}, {"native_fraction", counts[5] / std::max(covered, 1.0)}, {"translucent", translucent}, {"multiview_fraction", counts[6] / std::max(covered, 1.0)},
                  {"multiview", mv_ev.is_null() ? nlohmann::json{{"used", false}} : mv_ev},
                  {"pictures", pics.p.size()}, {"picture_ownership", ownership},
                  {"native", native_ev.is_null() ? nlohmann::json{{"used", false}, {"not_used_because", "no TRELLIS texture volume"}} : native_ev},
                  {"fallback_vertices", fallback_vertices}, {"fallback_components", fallback_components},
                  {"visible_seed_vertices", seeds.size()}, {"visibility_breakdown", breakdown}, {"symmetry", sym.evidence}, {"symmetry_vertices", symmetric_vertices}, {"colour_regions", regions}, {"outline_band_seeds", rim_seeds}, {"kmeans_sse", clusters.sse},
                  {"hidden_method", native ? "TRELLIS native texture (sparse PBR volume, engine sampler rules), colour-calibrated to the picture; geodesic reconstruction only where no voxel is near; never mirrored"
                                           : "geodesic nearest observation along mesh edges; boundary colour fades (sigma 3% of size) into the colour region's robust median; region-barrier smoothing; never mirrored"},
                  {"projection", "visibility-aware: z-buffer occlusion, facing ramp, alpha and silhouette/depth-edge distance confidence"},
                  {"gutter", "2 dilation rings + push-pull"},
                  {"seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()}};
  return out;
}
}  // namespace p3dasset::autotex
