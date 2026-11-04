#include "retopo_engine.hpp"
#include "mesh_asset.hpp"
#include <numeric>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

#include "auto_texture.hpp"
#include "manifold/manifold.h"
#include "pixel3d/reconstruct/true_solid.hpp"

namespace p3dasset::retopo {
using pixel3d::cross;
using pixel3d::dot;
using pixel3d::length;

namespace {
void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}
bool finite(vec3d p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5), as barycentrics.
std::array<double, 3> closest_bary(vec3d p, vec3d a, vec3d b, vec3d c) {
  const vec3d ab = b - a, ac = c - a, ap = p - a;
  const double d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return {1, 0, 0};
  const vec3d bp = p - b;
  const double d3 = dot(ab, bp), d4 = dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return {0, 1, 0};
  const double vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    const double v = d1 / (d1 - d3);
    return {1 - v, v, 0};
  }
  const vec3d cp = p - c;
  const double d5 = dot(ab, cp), d6 = dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return {0, 0, 1};
  const double vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    const double w = d2 / (d2 - d6);
    return {1 - w, 0, w};
  }
  const double va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return {0, 1 - w, w};
  }
  const double denom = va + vb + vc;
  if (!(std::abs(denom) > 0)) return {1, 0, 0};  // degenerate triangle: its first corner
  const double v = vb / denom, w = vc / denom;
  return {1 - v - w, v, w};
}

double triangle_area(vec3d a, vec3d b, vec3d c) { return 0.5 * length(cross(b - a, c - a)); }

// Area-weighted random points on a triangle mesh (deterministic).
std::vector<vec3d> sample_surface(const pixel3d::Mesh& m, size_t count, uint32_t seed) {
  std::vector<double> cdf(m.tris.size());
  double total = 0;
  for (size_t i = 0; i < m.tris.size(); ++i) {
    const auto& t = m.tris[i];
    total += triangle_area(m.positions[t[0]], m.positions[t[1]], m.positions[t[2]]);
    cdf[i] = total;
  }
  std::vector<vec3d> out;
  if (!(total > 0)) return out;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> u(0, 1);
  out.reserve(count);
  for (size_t k = 0; k < count; ++k) {
    const size_t i = size_t(std::lower_bound(cdf.begin(), cdf.end(), u(rng) * total) - cdf.begin());
    const auto& t = m.tris[std::min(i, m.tris.size() - 1)];
    double r1 = u(rng), r2 = u(rng);
    if (r1 + r2 > 1) { r1 = 1 - r1; r2 = 1 - r2; }
    const vec3d a = m.positions[t[0]];
    out.push_back(a + (m.positions[t[1]] - a) * r1 + (m.positions[t[2]] - a) * r2);
  }
  return out;
}

struct DistanceStats {
  double mean = 0, p95 = 0, max = 0;
};
DistanceStats distances(const std::vector<vec3d>& points, const SurfaceIndex& to, double diag) {
  std::vector<double> d;
  d.reserve(points.size());
  for (auto p : points) d.push_back(to.closest(p).distance / diag);
  DistanceStats s;
  if (d.empty()) return s;
  for (double v : d) s.mean += v;
  s.mean /= double(d.size());
  std::sort(d.begin(), d.end());
  s.p95 = d[std::min(d.size() - 1, size_t(0.95 * double(d.size())))];
  s.max = d.back();
  return s;
}

std::array<float, 4> sample_bilinear(const pixel3d::ImageRGBA& t, double u, double v) {
  const double x = u * t.width() - 0.5, y = v * t.height() - 0.5;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const double fx = x - x0, fy = y - y0;
  std::array<float, 4> out{};
  for (int dy = 0; dy < 2; ++dy)
    for (int dx = 0; dx < 2; ++dx) {
      const int xi = std::clamp(x0 + dx, 0, t.width() - 1), yi = std::clamp(y0 + dy, 0, t.height() - 1);
      const double w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
      const auto& px = t.at(xi, yi);
      out[0] += float(w * px.r);
      out[1] += float(w * px.g);
      out[2] += float(w * px.b);
      out[3] += float(w * px.a);
    }
  return out;
}
}  // namespace

PolyMesh read_obj(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  require(bool(in), "Cannot open the OBJ file");
  PolyMesh m;
  std::string line;
  std::vector<std::vector<long long>> raw;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream s(line);
    std::string tag;
    if (!(s >> tag)) continue;
    if (tag == "v") {
      vec3d p;
      require(bool(s >> p.x >> p.y >> p.z) && finite(p), "Invalid OBJ vertex");
      m.positions.push_back(p);
    } else if (tag == "f") {
      std::vector<long long> face;
      std::string corner;
      while (s >> corner) face.push_back(std::stoll(corner.substr(0, corner.find('/'))));
      require(face.size() >= 3, "OBJ face with fewer than three corners");
      raw.push_back(std::move(face));
    }
  }
  require(!m.positions.empty() && !raw.empty(), "The OBJ file has no faces");
  require(m.positions.size() <= 4000000 && raw.size() <= 4000000, "OBJ outside the bounded mesh profile");
  for (auto& f : raw) {
    std::vector<uint32_t> face;
    for (long long i : f) {
      const long long index = i < 0 ? (long long)m.positions.size() + i : i - 1;
      require(index >= 0 && index < (long long)m.positions.size(), "OBJ face index out of range");
      face.push_back(uint32_t(index));
    }
    m.faces.push_back(std::move(face));
  }
  return m;
}

void write_obj(const PolyMesh& m, const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  require(bool(out), "Cannot write the OBJ file");
  out.precision(9);
  out << "# 3re Smart Retopology\n";
  for (auto p : m.positions) out << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
  for (const auto& f : m.faces) {
    out << 'f';
    for (auto i : f) out << ' ' << (i + 1);
    out << '\n';
  }
  require(bool(out.flush()), "Writing the OBJ file failed");
}

PolyMesh to_poly(const pixel3d::Mesh& mesh) {
  PolyMesh p;
  p.positions = mesh.positions;
  for (const auto& t : mesh.tris) p.faces.push_back({t[0], t[1], t[2]});
  return p;
}

pixel3d::Mesh triangulate(const PolyMesh& m) {
  pixel3d::Mesh out;
  out.positions = m.positions;
  for (const auto& f : m.faces)
    for (size_t j = 1; j + 1 < f.size(); ++j) out.tris.push_back({f[0], f[j], f[j + 1]});
  return out;
}

pixel3d::Mesh engine_input(const pixel3d::Mesh& mesh) {
  require(!mesh.tris.empty(), "The model has no triangles");
  std::vector<uint8_t> used(mesh.positions.size(), 0);
  for (const auto& t : mesh.tris)
    for (auto i : t) used[i] = 1;
  // Restore exact, oppositely oriented attribute seams only within an already
  // connected shell. Never weld disconnected shells or bridge a nonzero gap.
  // The appearance mesh and its UVs stay untouched; this is the solver copy.
  std::vector<uint32_t> component(mesh.positions.size()), seam(mesh.positions.size());
  std::iota(component.begin(), component.end(), 0);
  std::iota(seam.begin(), seam.end(), 0);
  const auto root = [](auto& parents, uint32_t i) {
    while (parents[i] != i) { parents[i] = parents[parents[i]]; i = parents[i]; }
    return i;
  };
  const auto join = [&](auto& parents, uint32_t a, uint32_t b) {
    a=root(parents,a); b=root(parents,b); if(a!=b) parents[b]=a;
  };
  using Edge = std::array<uint32_t,2>;
  std::map<Edge,std::vector<Edge>> edges;
  for(const auto& t:mesh.tris) for(int k=0;k<3;++k) {
    const uint32_t a=t[k], b=t[(k+1)%3]; join(component,a,b);
    edges[{std::min(a,b),std::max(a,b)}].push_back({a,b});
  }
  const auto xyz = [&](uint32_t i) { const auto p=mesh.positions[i]; return std::array<double,3>{p.x,p.y,p.z}; };
  using GeometricEdge = std::array<std::array<double,3>,2>;
  std::map<GeometricEdge,std::vector<Edge>> borders;
  for(const auto& [key,uses]:edges) if(uses.size()==1) {
    const auto e=uses.front(); auto a=xyz(e[0]), b=xyz(e[1]);
    borders[{std::min(a,b),std::max(a,b)}].push_back(e);
  }
  bool stitched=false;
  for(const auto& [key,uses]:borders) if(uses.size()==2) {
    const auto a=uses[0], b=uses[1];
    if(root(component,a[0])!=root(component,b[0])) continue;
    if(xyz(a[0])!=xyz(b[1]) || xyz(a[1])!=xyz(b[0])) continue;
    join(seam,a[0],b[1]); join(seam,a[1],b[0]); stitched=true;
  }
  const bool soup = mesh.positions.size() >= 3 * mesh.tris.size();
  std::vector<uint32_t> remap(mesh.positions.size(), UINT32_MAX);
  pixel3d::Mesh out;
  std::map<std::array<double, 3>, uint32_t> welded;
  for (size_t i = 0; i < mesh.positions.size(); ++i) {
    if (!used[i]) continue;
    const auto p = mesh.positions[i];
    if (soup) {
      const auto [it, added] = welded.try_emplace({p.x, p.y, p.z}, uint32_t(out.positions.size()));
      if (added) out.positions.push_back(p);
      remap[i] = it->second;
    } else {
      const auto representative = root(seam, uint32_t(i));
      if(remap[representative]==UINT32_MAX) {
        remap[representative]=uint32_t(out.positions.size()); out.positions.push_back(p);
      }
      remap[i]=remap[representative];
    }
  }
  for (const auto& t : mesh.tris) {
    const std::array<uint32_t, 3> r{remap[t[0]], remap[t[1]], remap[t[2]]};
    if (r[0] == r[1] || r[1] == r[2] || r[0] == r[2]) continue;
    if (!(triangle_area(out.positions[r[0]], out.positions[r[1]], out.positions[r[2]]) > 0)) continue;
    out.tris.push_back(r);
  }
  require(!out.tris.empty(), "The model has no usable triangles");
  if(stitched && !soup) {
    require(geometrically_identical(mesh,out), "Seam reconstruction changed the source surface");
    const auto audit=inspect_geometry(out);
    for(const char* field:{"overfull_edges","invalid_vertex_links","duplicate_faces","zero_area_faces","inconsistent_winding_edges"})
      require(audit.at(field).get<int64_t>()==0, "Ambiguous coincident seam; cannot safely reconstruct connectivity");
  }
  return out;
}

namespace {
// Symmetric 4x4 quadric: a,b,c,d of plane ax+by+cz+d summed as outer products (10 coefficients).
struct Quadric {
  std::array<double, 10> q{};
  static Quadric plane(vec3d n, double d, double w) {
    Quadric r;
    const double a = n.x, b = n.y, c = n.z;
    r.q = {a * a * w, a * b * w, a * c * w, a * d * w, b * b * w, b * c * w, b * d * w, c * c * w, c * d * w, d * d * w};
    return r;
  }
  Quadric& operator+=(const Quadric& o) {
    for (size_t i = 0; i < 10; ++i) q[i] += o.q[i];
    return *this;
  }
  double error(vec3d p) const {
    const double x = p.x, y = p.y, z = p.z;
    return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z + 2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z + 2 * q[6] * y +
           q[7] * z * z + 2 * q[8] * z + q[9];
  }
  // Minimiser of the quadric, if well conditioned.
  bool optimum(vec3d& out) const {
    const double a00 = q[0], a01 = q[1], a02 = q[2], a11 = q[4], a12 = q[5], a22 = q[7];
    const double det = a00 * (a11 * a22 - a12 * a12) - a01 * (a01 * a22 - a12 * a02) + a02 * (a01 * a12 - a11 * a02);
    const double scale = std::abs(a00) + std::abs(a11) + std::abs(a22);
    if (!(std::abs(det) > 1e-12 * scale * scale * scale) || !std::isfinite(det)) return false;
    const double b0 = -q[3], b1 = -q[6], b2 = -q[8];
    out = {(b0 * (a11 * a22 - a12 * a12) - a01 * (b1 * a22 - a12 * b2) + a02 * (b1 * a12 - a11 * b2)) / det,
           (a00 * (b1 * a22 - a12 * b2) - b0 * (a01 * a22 - a12 * a02) + a02 * (a01 * b2 - b1 * a02)) / det,
           (a00 * (a11 * b2 - b1 * a12) - a01 * (a01 * b2 - b1 * a02) + b0 * (a01 * a12 - a11 * a02)) / det};
    return finite(out);
  }
};
}  // namespace

pixel3d::Mesh simplify(const pixel3d::Mesh& in, size_t target) {
  if (in.tris.size() <= target) return in;
  std::vector<vec3d> pos = in.positions;
  std::vector<std::array<uint32_t, 3>> tris = in.tris;
  std::vector<uint8_t> face_alive(tris.size(), 1);
  std::vector<std::vector<uint32_t>> vf(pos.size());
  for (uint32_t f = 0; f < tris.size(); ++f)
    for (auto v : tris[f]) vf[v].push_back(f);
  const auto normal_of = [&](const std::array<uint32_t, 3>& t) { return cross(pos[t[1]] - pos[t[0]], pos[t[2]] - pos[t[0]]); };
  std::vector<Quadric> quad(pos.size());
  std::map<std::pair<uint32_t, uint32_t>, int> edge_count;
  for (const auto& t : tris) {
    const vec3d n = normal_of(t);
    const double len = length(n);
    if (!(len > 0)) continue;
    const vec3d u = n / len;
    const auto qf = Quadric::plane(u, -dot(u, pos[t[0]]), len * 0.5);
    for (auto v : t) quad[v] += qf;
    for (int k = 0; k < 3; ++k) ++edge_count[{std::min(t[k], t[(k + 1) % 3]), std::max(t[k], t[(k + 1) % 3])}];
  }
  // Open borders: a plane through the edge, perpendicular to its face, heavily weighted.
  std::vector<uint8_t> border(pos.size(), 0);
  for (const auto& t : tris) {
    const vec3d n = pixel3d::normalize(normal_of(t));
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = t[k], b = t[(k + 1) % 3];
      if (edge_count[{std::min(a, b), std::max(a, b)}] != 1) continue;
      border[a] = border[b] = 1;
      const vec3d e = pos[b] - pos[a];
      const vec3d p = pixel3d::normalize(cross(e, n));
      if (length(p) < 0.5) continue;
      const auto qb = Quadric::plane(p, -dot(p, pos[a]), 1000.0 * dot(e, e));
      quad[a] += qb;
      quad[b] += qb;
    }
  }
  struct Candidate {
    double cost;
    uint32_t a, b;
    uint64_t stamp;
    vec3d at;
    bool operator<(const Candidate& o) const { return cost > o.cost; }
  };
  std::vector<uint64_t> stamp(pos.size(), 0);
  std::vector<Candidate> heap;
  const auto evaluate = [&](uint32_t a, uint32_t b) {
    Quadric q = quad[a];
    q += quad[b];
    vec3d best = (pos[a] + pos[b]) * 0.5;
    double cost = q.error(best);
    vec3d opt;
    if (!border[a] && !border[b] && q.optimum(opt) && length(opt - best) < 2 * length(pos[a] - pos[b])) {
      if (const double c = q.error(opt); c < cost) { cost = c; best = opt; }
    }
    for (vec3d p : {pos[a], pos[b]})
      if (const double c = q.error(p); c < cost) { cost = c; best = p; }
    // A border vertex stays where it is.
    if (border[a] && !border[b]) { best = pos[a]; cost = q.error(best); }
    if (border[b] && !border[a]) { best = pos[b]; cost = q.error(best); }
    heap.push_back({std::max(cost, 0.0), a, b, stamp[a] + stamp[b], best});
    std::push_heap(heap.begin(), heap.end());
  };
  for (const auto& [e, n] : edge_count)
    if (n <= 2) evaluate(e.first, e.second);
  size_t alive = tris.size();
  std::vector<uint32_t> ring_a, ring_b;
  const auto ring = [&](uint32_t v, std::vector<uint32_t>& out) {
    out.clear();
    for (auto f : vf[v])
      if (face_alive[f])
        for (auto w : tris[f])
          if (w != v) out.push_back(w);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
  };
  while (alive > target && !heap.empty()) {
    std::pop_heap(heap.begin(), heap.end());
    const Candidate c = heap.back();
    heap.pop_back();
    if (c.stamp != stamp[c.a] + stamp[c.b]) continue;  // stale
    const uint32_t a = c.a, b = c.b;
    // Shared faces and the link condition (manifold collapses only).
    size_t shared = 0;
    for (auto f : vf[a])
      if (face_alive[f] && (tris[f][0] == b || tris[f][1] == b || tris[f][2] == b)) ++shared;
    if (shared == 0 || shared > 2) continue;
    ring(a, ring_a);
    ring(b, ring_b);
    size_t common = 0;
    for (size_t i = 0, j = 0; i < ring_a.size() && j < ring_b.size();) {
      if (ring_a[i] == ring_b[j]) { ++common; ++i; ++j; }
      else if (ring_a[i] < ring_b[j]) ++i;
      else ++j;
    }
    if (common != shared) continue;
    if (border[a] && border[b] && shared != 1) continue;  // would pinch a border
    // No face around a or b may flip or collapse to a sliver.
    bool ok = true;
    for (uint32_t v : {a, b}) {
      for (auto f : vf[v]) {
        if (!face_alive[f]) continue;
        const auto& t = tris[f];
        if ((t[0] == a || t[1] == a || t[2] == a) && (t[0] == b || t[1] == b || t[2] == b)) continue;  // removed
        std::array<vec3d, 3> p{pos[t[0]], pos[t[1]], pos[t[2]]};
        const vec3d before = cross(p[1] - p[0], p[2] - p[0]);
        for (int k = 0; k < 3; ++k)
          if (t[k] == v) p[k] = c.at;
        const vec3d after = cross(p[1] - p[0], p[2] - p[0]);
        const double la = length(after), lb = length(before);
        if (!(la > 1e-12 * lb) || dot(after, before) < 0.2 * la * lb) { ok = false; break; }
      }
      if (!ok) break;
    }
    if (!ok) continue;
    // Collapse b into a.
    pos[a] = c.at;
    quad[a] += quad[b];
    border[a] = border[a] || border[b];
    for (auto f : vf[b]) {
      if (!face_alive[f]) continue;
      auto& t = tris[f];
      if (t[0] == a || t[1] == a || t[2] == a) {
        face_alive[f] = 0;
        --alive;
        continue;
      }
      for (auto& v : t)
        if (v == b) v = a;
      vf[a].push_back(f);
    }
    vf[b].clear();
    std::erase_if(vf[a], [&](uint32_t f) { return !face_alive[f]; });
    // Only the edges at a changed cost (other quadrics and positions are as they were); validity is
    // checked again when an edge comes off the heap.
    ++stamp[a];
    ++stamp[b];
    ring(a, ring_a);
    for (auto w : ring_a) evaluate(std::min(a, w), std::max(a, w));
    if (heap.size() > 8 * tris.size()) {  // drop stale entries now and then
      std::erase_if(heap, [&](const Candidate& h) { return h.stamp != stamp[h.a] + stamp[h.b]; });
      std::make_heap(heap.begin(), heap.end());
    }
  }
  pixel3d::Mesh out;
  std::vector<uint32_t> remap(pos.size(), UINT32_MAX);
  for (size_t f = 0; f < tris.size(); ++f) {
    if (!face_alive[f]) continue;
    std::array<uint32_t, 3> t{};
    for (int k = 0; k < 3; ++k) {
      auto& r = remap[tris[f][k]];
      if (r == UINT32_MAX) {
        r = uint32_t(out.positions.size());
        out.positions.push_back(pos[tris[f][k]]);
      }
      t[k] = r;
    }
    out.tris.push_back(t);
  }
  return out;
}

int handles(const pixel3d::Mesh& m) {
  std::map<std::pair<uint32_t, uint32_t>, int> edges;
  std::vector<uint32_t> parent(m.positions.size());
  for (uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
  const auto find = [&](uint32_t v) {
    while (parent[v] != v) v = parent[v] = parent[parent[v]];
    return v;
  };
  std::vector<uint8_t> used(m.positions.size(), 0);
  for (const auto& t : m.tris)
    for (int k = 0; k < 3; ++k) {
      const uint32_t a = t[k], b = t[(k + 1) % 3];
      edges[{std::min(a, b), std::max(a, b)}]++;
      used[a] = 1;
      parent[find(a)] = find(b);
    }
  long long v = 0, components = 0;
  for (uint32_t i = 0; i < used.size(); ++i)
    if (used[i]) {
      ++v;
      if (find(i) == i) ++components;
    }
  const long long chi = v - (long long)edges.size() + (long long)m.tris.size();
  return int(std::max(0LL, (2 * components - chi) / 2));
}

namespace {
// Samples on a regular grid with a trilinear field for manifold's level set (positive inside).
struct Grid {
  vec3d lo;
  double edge = 1;
  int nx = 0, ny = 0, nz = 0;
  size_t at(int i, int j, int k) const { return (size_t(k) * ny + j) * nx + i; }
  size_t size() const { return size_t(nx) * ny * nz; }
  vec3d point(int i, int j, int k) const { return lo + vec3d{i * edge, j * edge, k * edge}; }
  double sample(const std::vector<float>& f, manifold::vec3 p) const {
    const double fx = std::clamp((p.x - lo.x) / edge, 0.0, nx - 1.000001), fy = std::clamp((p.y - lo.y) / edge, 0.0, ny - 1.000001),
                 fz = std::clamp((p.z - lo.z) / edge, 0.0, nz - 1.000001);
    const int i = int(fx), j = int(fy), k = int(fz);
    const double u = fx - i, v = fy - j, w = fz - k;
    double s = 0;
    for (int dz = 0; dz < 2; ++dz)
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
          s += (dx ? u : 1 - u) * (dy ? v : 1 - v) * (dz ? w : 1 - w) * f[at(std::min(i + dx, nx - 1), std::min(j + dy, ny - 1), std::min(k + dz, nz - 1))];
    return s;
  }
  // Unsigned distance to `m` where it is below `band` (exact), `band` elsewhere.
  std::vector<float> distance(const pixel3d::Mesh& m, double band) const {
    std::vector<float> d(size(), float(band));
    std::vector<uint8_t> near(size(), 0);
    for (const auto& t : m.tris) {
      vec3d a{1e300, 1e300, 1e300}, b{-1e300, -1e300, -1e300};
      for (auto v : t) {
        const auto p = m.positions[v];
        a = {std::min(a.x, p.x), std::min(a.y, p.y), std::min(a.z, p.z)};
        b = {std::max(b.x, p.x), std::max(b.y, p.y), std::max(b.z, p.z)};
      }
      const int i0 = std::max(0, int(std::floor((a.x - lo.x - band) / edge))), i1 = std::min(nx - 1, int(std::ceil((b.x - lo.x + band) / edge)));
      const int j0 = std::max(0, int(std::floor((a.y - lo.y - band) / edge))), j1 = std::min(ny - 1, int(std::ceil((b.y - lo.y + band) / edge)));
      const int k0 = std::max(0, int(std::floor((a.z - lo.z - band) / edge))), k1 = std::min(nz - 1, int(std::ceil((b.z - lo.z + band) / edge)));
      for (int k = k0; k <= k1; ++k)
        for (int j = j0; j <= j1; ++j)
          for (int i = i0; i <= i1; ++i) near[at(i, j, k)] = 1;
    }
    const SurfaceIndex index(m);
    for (int k = 0; k < nz; ++k)
      for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
          if (near[at(i, j, k)]) d[at(i, j, k)] = float(std::min(index.closest(point(i, j, k)).distance, band));
    return d;
  }
  pixel3d::Mesh level_set(const std::vector<float>& f, double level) const {
    const manifold::Box box({lo.x, lo.y, lo.z}, {lo.x + (nx - 1) * edge, lo.y + (ny - 1) * edge, lo.z + (nz - 1) * edge});
    const auto shell = manifold::Manifold::LevelSet([&](manifold::vec3 p) { return sample(f, p); }, box, edge, level, -1, true);
    require(shell.Status() == manifold::Manifold::Error::NoError && !shell.IsEmpty(), "The closed shell could not be built");
    return pixel3d::mesh_from_manifold(shell, {});
  }
};
}  // namespace

pixel3d::Mesh closed_shell(const pixel3d::Mesh& m, double edge) {
  require(!m.tris.empty() && edge > 0, "Empty mesh");
  vec3d lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  for (auto p : m.positions) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
  const double tau = edge;  // closing radius: gaps narrower than 2 tau close
  Grid g;
  const double pad = tau + 3 * edge;
  g.lo = lo - vec3d{pad, pad, pad};
  const vec3d ext = hi - lo + vec3d{2 * pad, 2 * pad, 2 * pad};
  g.edge = std::max(edge, std::cbrt(ext.x * ext.y * ext.z / 16.0e6));  // at most ~16M samples
  g.nx = int(std::ceil(ext.x / g.edge)) + 1;
  g.ny = int(std::ceil(ext.y / g.edge)) + 1;
  g.nz = int(std::ceil(ext.z / g.edge)) + 1;
  const double band = tau + 2 * g.edge;
  const auto d = g.distance(m, band);
  // Outside: everything reachable from the grid's border without coming closer than tau to the surface.
  // Tunnels and cavities the tau-ball cannot pass, and every internal sheet, end up inside.
  std::vector<uint8_t> out(g.size(), 0);
  std::vector<size_t> todo;
  for (int k = 0; k < g.nz; ++k)
    for (int j = 0; j < g.ny; ++j)
      for (int i = 0; i < g.nx; ++i)
        if ((i == 0 || j == 0 || k == 0 || i == g.nx - 1 || j == g.ny - 1 || k == g.nz - 1) && d[g.at(i, j, k)] > tau) {
          out[g.at(i, j, k)] = 1;
          todo.push_back(g.at(i, j, k));
        }
  while (!todo.empty()) {
    const size_t s = todo.back();
    todo.pop_back();
    const int i = int(s % g.nx), j = int((s / g.nx) % g.ny), k = int(s / (size_t(g.nx) * g.ny));
    const int nb[6][3] = {{i - 1, j, k}, {i + 1, j, k}, {i, j - 1, k}, {i, j + 1, k}, {i, j, k - 1}, {i, j, k + 1}};
    for (const auto& n : nb) {
      if (n[0] < 0 || n[1] < 0 || n[2] < 0 || n[0] >= g.nx || n[1] >= g.ny || n[2] >= g.nz) continue;
      const size_t t = g.at(n[0], n[1], n[2]);
      if (out[t] || d[t] <= tau) continue;
      out[t] = 1;
      todo.push_back(t);
    }
  }
  // Dilated solid: zero where the outside region meets distance tau (no other zero crossing).
  std::vector<float> dilated(g.size());
  for (size_t s = 0; s < g.size(); ++s) dilated[s] = out[s] ? float(tau - d[s]) : float(std::max(tau - d[s], 0.0) + 1e-4 * g.edge);
  const auto outer = g.level_set(dilated, 0.0);
  // Erode by tau: signed distance to the dilated surface (inside = not outside), cut at tau.
  const double band2 = tau + 2 * g.edge;
  const auto d2 = g.distance(outer, band2);
  std::vector<float> eroded(g.size());
  for (size_t s = 0; s < g.size(); ++s) eroded[s] = dilated[s] > 0 ? d2[s] : -d2[s];
  auto shell = g.level_set(eroded, tau);
  require(!shell.tris.empty(), "The closed shell is empty");
  return shell;
}

SurfaceIndex::SurfaceIndex(const pixel3d::Mesh& mesh) : mesh_(mesh) {
  require(!mesh.tris.empty(), "Empty surface");
  vec3d lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
  for (const auto& t : mesh.tris)
    for (auto i : t) {
      const auto p = mesh.positions[i];
      lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
      hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
  diagonal_ = std::max(length(hi - lo), 1e-12);
  const vec3d ext{std::max(hi.x - lo.x, diagonal_ * 1e-6), std::max(hi.y - lo.y, diagonal_ * 1e-6), std::max(hi.z - lo.z, diagonal_ * 1e-6)};
  // About two triangles per cell, at most 2M cells.
  const double cells = std::clamp(double(mesh.tris.size()) / 2.0, 1.0, 2.0e6);
  const double edge = std::cbrt(ext.x * ext.y * ext.z / cells);
  dims_ = {std::clamp(int(std::ceil(ext.x / edge)), 1, 512), std::clamp(int(std::ceil(ext.y / edge)), 1, 512),
           std::clamp(int(std::ceil(ext.z / edge)), 1, 512)};
  lo_ = lo;
  cell_size_ = {ext.x / dims_[0], ext.y / dims_[1], ext.z / dims_[2]};
  const auto range = [&](const std::array<uint32_t, 3>& t, int axis, int& a, int& b) {
    double mn = 1e300, mx = -1e300;
    for (auto i : t) {
      const auto p = mesh.positions[i];
      const double v = axis == 0 ? p.x - lo.x : axis == 1 ? p.y - lo.y : p.z - lo.z;
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    const double s = axis == 0 ? cell_size_.x : axis == 1 ? cell_size_.y : cell_size_.z;
    a = std::clamp(int(std::floor(mn / s)), 0, dims_[size_t(axis)] - 1);
    b = std::clamp(int(std::floor(mx / s)), 0, dims_[size_t(axis)] - 1);
  };
  const size_t total = size_t(dims_[0]) * dims_[1] * dims_[2];
  std::vector<uint32_t> count(total + 1, 0);
  for (int pass = 0; pass < 2; ++pass) {
    for (size_t ti = 0; ti < mesh.tris.size(); ++ti) {
      int x0, x1, y0, y1, z0, z1;
      range(mesh.tris[ti], 0, x0, x1);
      range(mesh.tris[ti], 1, y0, y1);
      range(mesh.tris[ti], 2, z0, z1);
      for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
          for (int x = x0; x <= x1; ++x) {
            if (pass == 0) ++count[cell(x, y, z) + 1];
            else items_[count[cell(x, y, z)]++] = uint32_t(ti);
          }
    }
    if (pass == 0) {
      for (size_t i = 1; i <= total; ++i) count[i] += count[i - 1];
      start_ = count;
      items_.resize(count[total]);
    }
  }
}

SurfaceIndex::Hit SurfaceIndex::closest(vec3d p) const {
  const auto clamp_cell = [&](double v, double s, int n) { return std::clamp(int(std::floor(v / s)), 0, n - 1); };
  const int cx = clamp_cell(p.x - lo_.x, cell_size_.x, dims_[0]), cy = clamp_cell(p.y - lo_.y, cell_size_.y, dims_[1]),
            cz = clamp_cell(p.z - lo_.z, cell_size_.z, dims_[2]);
  Hit best;
  best.distance = std::numeric_limits<double>::infinity();
  const int max_r = std::max({dims_[0], dims_[1], dims_[2]});
  for (int r = 0; r <= max_r; ++r) {
    const int x0 = cx - r, x1 = cx + r, y0 = cy - r, y1 = cy + r, z0 = cz - r, z1 = cz + r;
    for (int z = std::max(z0, 0); z <= std::min(z1, dims_[2] - 1); ++z)
      for (int y = std::max(y0, 0); y <= std::min(y1, dims_[1] - 1); ++y)
        for (int x = std::max(x0, 0); x <= std::min(x1, dims_[0] - 1); ++x) {
          if (r > 0 && x != x0 && x != x1 && y != y0 && y != y1 && z != z0 && z != z1) continue;  // ring shell only
          const size_t c = cell(x, y, z);
          for (uint32_t k = start_[c]; k < start_[c + 1]; ++k) {
            const auto& t = mesh_.tris[items_[k]];
            const vec3d a = mesh_.positions[t[0]], b = mesh_.positions[t[1]], cc = mesh_.positions[t[2]];
            const auto w = closest_bary(p, a, b, cc);
            const double d = length(a * w[0] + b * w[1] + cc * w[2] - p);
            if (d < best.distance) best = {int(items_[k]), w, d};
          }
        }
    // Cells outside the searched cube are at least this far away (sides at the grid edge have none).
    double bound = std::numeric_limits<double>::infinity();
    const auto side = [&](double pv, double lo, double s, int a, int b, int n) {
      if (a > 0) bound = std::min(bound, pv - (lo + a * s));
      if (b < n - 1) bound = std::min(bound, lo + (b + 1) * s - pv);
    };
    side(p.x, lo_.x, cell_size_.x, x0, x1, dims_[0]);
    side(p.y, lo_.y, cell_size_.y, y0, y1, dims_[1]);
    side(p.z, lo_.z, cell_size_.z, z0, z1, dims_[2]);
    if (best.tri >= 0 && best.distance <= std::max(bound, 0.0)) break;
    if (x0 <= 0 && y0 <= 0 && z0 <= 0 && x1 >= dims_[0] - 1 && y1 >= dims_[1] - 1 && z1 >= dims_[2] - 1) break;
  }
  return best;
}

nlohmann::json measure(const pixel3d::Mesh& source, const PolyMesh& result, size_t samples) {
  const pixel3d::Mesh tri = triangulate(result);
  size_t quads = 0, triangles = 0, other = 0;
  std::map<std::pair<uint32_t, uint32_t>, int> edges;
  for (const auto& f : result.faces) {
    (f.size() == 4 ? quads : f.size() == 3 ? triangles : other)++;
    for (size_t i = 0; i < f.size(); ++i) {
      auto a = f[i], b = f[(i + 1) % f.size()];
      if (a > b) std::swap(a, b);
      ++edges[{a, b}];
    }
  }
  size_t open = 0, non_manifold = 0;
  for (const auto& [e, n] : edges) {
    if (n == 1) ++open;
    if (n > 2) ++non_manifold;
  }
  const SurfaceIndex src(source), dst(tri);
  const double diag = src.diagonal();
  const auto there = distances(sample_surface(source, samples, 1), dst, diag);
  const auto back = distances(sample_surface(tri, samples, 2), src, diag);
  return {{"source_triangles", source.tris.size()},
          {"vertices", result.positions.size()},
          {"faces", result.faces.size()},
          {"quads", quads},
          {"triangles", triangles},
          {"other_polygons", other},
          {"quad_share", result.faces.empty() ? 0.0 : double(quads) / double(result.faces.size())},
          {"open_edges", open},
          {"non_manifold_edges", non_manifold},
          {"deviation",
           {{"relative_to", "source bounding-box diagonal"},
            {"source_to_result", {{"mean", there.mean}, {"p95", there.p95}, {"max", there.max}}},
            {"result_to_source", {{"mean", back.mean}, {"p95", back.p95}, {"max", back.max}}}}}};
}

double quad_scale(double area, double edge, int target_faces) {
  require(area > 0 && edge > 0 && target_faces > 0, "Invalid density request");
  return std::sqrt(area / double(target_faces)) / edge;
}

pixel3d::TexturedMesh transfer_appearance(const pixel3d::Mesh& target, const pixel3d::TexturedMesh* source, int size,
                                          double seconds, nlohmann::json& evidence, bool nearest) {
  namespace at = p3dasset::autotex;
  const auto deadline = at::Deadline::in_seconds(seconds);
  at::Atlas atlas;
  std::string failures;
  for (int attempt = 0; attempt < 3 && atlas.mesh.positions.empty(); ++attempt) {
    try {
      atlas = at::build_atlas(target, size, attempt, deadline);
      evidence["atlas_attempt"] = attempt;
    } catch (const at::BudgetExceeded&) {
      throw;
    } catch (const std::exception& e) {
      failures += std::string(e.what()) + "; ";
    }
  }
  require(!atlas.mesh.positions.empty(), "No UV atlas could be built for the new mesh");
  if (!failures.empty()) evidence["atlas_failures"] = failures;
  evidence["atlas"] = atlas.metrics;
  auto image = std::make_shared<pixel3d::ImageRGBA>(size, size);
  std::vector<uint8_t> filled(size_t(size) * size, 0);
  if (!source || !source->texture || source->indices.empty()) {
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) image->at(x, y) = {200, 200, 200, 255};
    evidence["appearance"] = "none in the source (plain grey)";
  } else {
    pixel3d::Mesh surface;
    surface.positions = source->positions;
    for (size_t i = 0; i + 2 < source->indices.size(); i += 3)
      surface.tris.push_back({source->indices[i], source->indices[i + 1], source->indices[i + 2]});
    const SurfaceIndex index(surface);
    const auto& tm = atlas.mesh;
    double distance_sum = 0;
    size_t texels = 0;
    for (size_t f = 0; f + 2 < tm.indices.size(); f += 3) {
      if (f % 3000 == 0) deadline.check("colour transfer");
      const uint32_t i0 = tm.indices[f], i1 = tm.indices[f + 1], i2 = tm.indices[f + 2];
      const pixel3d::vec2d a = tm.uvs[i0] * size, b = tm.uvs[i1] * size, c = tm.uvs[i2] * size;
      const double area = pixel3d::cross(b - a, c - a);
      if (std::abs(area) < 1e-12) continue;
      const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}) - 1))), x1 = std::min(size - 1, int(std::ceil(std::max({a.x, b.x, c.x}) + 1)));
      const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}) - 1))), y1 = std::min(size - 1, int(std::ceil(std::max({a.y, b.y, c.y}) + 1)));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          const pixel3d::vec2d p{x + 0.5, y + 0.5};
          double w1 = pixel3d::cross(c - b, p - b) / area, w2 = pixel3d::cross(a - c, p - c) / area;
          double w3 = 1 - w1 - w2;
          // Texel centres up to ~0.7 px outside the triangle still belong to it (no seams at chart edges);
          // they take the colour of the nearest point inside.
          const double edge_px = 0.7 * std::max({length(b - a), length(c - b), length(a - c)}) / std::abs(area);
          if (w1 < -edge_px || w2 < -edge_px || w3 < -edge_px) continue;
          const size_t k = size_t(y) * size + x;
          const bool inside = w1 >= 0 && w2 >= 0 && w3 >= 0;
          if (filled[k] == 2 || (filled[k] == 1 && !inside)) continue;
          w1 = std::max(w1, 0.0); w2 = std::max(w2, 0.0); w3 = std::max(w3, 0.0);
          const double sum = w1 + w2 + w3;
          const vec3d q = (tm.positions[i0] * w1 + tm.positions[i1] * w2 + tm.positions[i2] * w3) / sum;
          const auto hit = index.closest(q);
          if (hit.tri < 0) continue;
          const auto& st = surface.tris[size_t(hit.tri)];
          const auto uv = source->uvs[st[0]] * hit.bary[0] + source->uvs[st[1]] * hit.bary[1] + source->uvs[st[2]] * hit.bary[2];
          auto rgba = sample_bilinear(*source->texture, uv.x, uv.y);
          if (nearest) {
            const auto px = source->texture->at(std::clamp(int(std::floor(uv.x * source->texture->width())), 0, source->texture->width()-1),
                                                std::clamp(int(std::floor(uv.y * source->texture->height())), 0, source->texture->height()-1));
            rgba = {float(px.r),float(px.g),float(px.b),float(px.a)};
          }
          image->at(x, y) = {uint8_t(std::lround(std::clamp(rgba[0], 0.f, 255.f))), uint8_t(std::lround(std::clamp(rgba[1], 0.f, 255.f))),
                             uint8_t(std::lround(std::clamp(rgba[2], 0.f, 255.f))), uint8_t(std::lround(std::clamp(rgba[3], 0.f, 255.f)))};
          if (!filled[k]) { distance_sum += hit.distance; ++texels; }
          filled[k] = inside ? 2 : 1;
        }
    }
    require(texels > 0, "The colour transfer covered no texels");
    // Gutter: grow the charts outward until every texel has a colour (linear filtering and mipmaps stay clean).
    std::vector<uint8_t> next;
    for (int pass = 0; pass < size; ++pass) {
      next = filled;
      bool changed = false, empty = false;
      for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
          if (filled[size_t(y) * size + x]) continue;
          int n = 0;
          std::array<int, 4> acc{};
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const int xx = x + dx, yy = y + dy;
              if (xx < 0 || yy < 0 || xx >= size || yy >= size || !filled[size_t(yy) * size + xx]) continue;
              const auto px = image->at(xx, yy);
              acc[0] += px.r; acc[1] += px.g; acc[2] += px.b; acc[3] += px.a;
              ++n;
            }
          if (!n) { empty = true; continue; }
          image->at(x, y) = {uint8_t(acc[0] / n), uint8_t(acc[1] / n), uint8_t(acc[2] / n), uint8_t(acc[3] / n)};
          next[size_t(y) * size + x] = 1;
          changed = true;
        }
      filled.swap(next);
      if (!empty || !changed) break;
    }
    evidence["appearance"] = "base colour from the closest point of the source surface";
    evidence["texels"] = texels;
    evidence["mean_projection_distance"] = distance_sum / double(texels) / index.diagonal();
  }
  auto out = atlas.mesh;
  out.texture = image;
  evidence["texture_size"] = size;
  return out;
}

}  // namespace p3dasset::retopo
