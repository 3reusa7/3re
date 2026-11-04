#include "pixel3d/style/raycast.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pixel3d {
namespace {

vec3d vmin(vec3d a, vec3d b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
vec3d vmax(vec3d a, vec3d b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
double comp(vec3d v, int k) { return k == 0 ? v.x : k == 1 ? v.y : v.z; }

bool box_hit(vec3d lo, vec3d hi, vec3d o, vec3d inv, double tmin, double tmax) {
  for (int k = 0; k < 3; ++k) {
    double t0 = (comp(lo, k) - comp(o, k)) * comp(inv, k), t1 = (comp(hi, k) - comp(o, k)) * comp(inv, k);
    if (t0 > t1) std::swap(t0, t1);
    tmin = std::max(tmin, t0), tmax = std::min(tmax, t1);
    if (tmin > tmax) return false;
  }
  return true;
}

}  // namespace

TriangleBVH::TriangleBVH(const std::vector<vec3d>& positions, const std::vector<uint32_t>& indices)
    : p_(positions), idx_(indices) {
  order_.resize(indices.size() / 3);
  std::iota(order_.begin(), order_.end(), 0);
  if (!order_.empty()) build(0, static_cast<int>(order_.size()));
}

int TriangleBVH::build(int first, int count) {
  Node n;
  n.lo = {1e300, 1e300, 1e300}, n.hi = {-1e300, -1e300, -1e300};
  for (int i = first; i < first + count; ++i)
    for (int k = 0; k < 3; ++k) {
      const vec3d v = p_[idx_[3 * order_[i] + k]];
      n.lo = vmin(n.lo, v), n.hi = vmax(n.hi, v);
    }
  const int id = static_cast<int>(nodes_.size());
  nodes_.push_back(n);
  if (count <= 4) {
    nodes_[id].first = first, nodes_[id].count = count;
    return id;
  }
  const vec3d ext = n.hi - n.lo;
  const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
  auto centre = [&](int t) {
    return comp(p_[idx_[3 * t]], axis) + comp(p_[idx_[3 * t + 1]], axis) + comp(p_[idx_[3 * t + 2]], axis);
  };
  const int mid = first + count / 2;
  std::nth_element(order_.begin() + first, order_.begin() + mid, order_.begin() + first + count,
                   [&](int a, int b) { return centre(a) < centre(b) || (centre(a) == centre(b) && a < b); });
  const int l = build(first, mid - first), r = build(mid, first + count - mid);
  nodes_[id].left = l, nodes_[id].right = r;
  return id;
}

double TriangleBVH::hit(vec3d o, vec3d d, double tmin, double tmax) const {
  if (nodes_.empty()) return -1;
  const vec3d inv{1 / (d.x != 0 ? d.x : 1e-300), 1 / (d.y != 0 ? d.y : 1e-300), 1 / (d.z != 0 ? d.z : 1e-300)};
  double best = -1;
  int stack[128], sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    const Node& n = nodes_[stack[--sp]];
    if (!box_hit(n.lo, n.hi, o, inv, tmin, best >= 0 ? best : tmax)) continue;
    if (n.left >= 0) {
      stack[sp++] = n.left;
      stack[sp++] = n.right;
      continue;
    }
    for (int i = n.first; i < n.first + n.count; ++i) {
      // Möller–Trumbore.
      const vec3d a = p_[idx_[3 * order_[i]]], b = p_[idx_[3 * order_[i] + 1]], c = p_[idx_[3 * order_[i] + 2]];
      const vec3d e1 = b - a, e2 = c - a, pv = cross(d, e2);
      const double det = dot(e1, pv);
      if (std::abs(det) < 1e-14) continue;
      const double id = 1 / det;
      const vec3d tv = o - a;
      const double u = dot(tv, pv) * id;
      if (u < 0 || u > 1) continue;
      const vec3d qv = cross(tv, e1);
      const double v = dot(d, qv) * id;
      if (v < 0 || u + v > 1) continue;
      const double t = dot(e2, qv) * id;
      if (t > tmin && t < (best >= 0 ? best : tmax)) best = t;
    }
  }
  return best;
}

}  // namespace pixel3d
