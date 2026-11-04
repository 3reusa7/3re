#include "pixel3d/lowpoly/rig.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pixel3d::lowpoly {
namespace {

double band_volume(const std::vector<std::vector<vec3d>>& rings) {
  // Closed band: quads between consecutive rings plus a fan over each end ring.
  double v = 0;
  auto tri = [&](vec3d a, vec3d b, vec3d c) { v += dot(a, cross(b, c)) / 6; };
  const size_t n = rings.front().size();
  for (size_t i = 0; i + 1 < rings.size(); ++i)
    for (size_t j = 0; j < n; ++j) {
      const vec3d a = rings[i][j], b = rings[i][(j + 1) % n], c = rings[i + 1][(j + 1) % n], d = rings[i + 1][j];
      tri(a, b, c);
      tri(a, c, d);
    }
  for (int e = 0; e < 2; ++e) {
    const auto& r = e == 0 ? rings.front() : rings.back();
    vec3d c{};
    for (const vec3d& p : r) c = c + p;
    c = c * (1.0 / static_cast<double>(n));
    for (size_t j = 0; j < n; ++j) e == 0 ? tri(c, r[(j + 1) % n], r[j]) : tri(c, r[j], r[(j + 1) % n]);
  }
  return std::abs(v);
}

}  // namespace

Mat3 Mat3::operator*(const Mat3& o) const {
  Mat3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += m[static_cast<size_t>(i * 3 + k)] * o.m[static_cast<size_t>(k * 3 + j)];
      r.m[static_cast<size_t>(i * 3 + j)] = s;
    }
  return r;
}

Mat3 Mat3::transposed() const {
  Mat3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[static_cast<size_t>(i * 3 + j)] = m[static_cast<size_t>(j * 3 + i)];
  return r;
}

Mat3 Mat3::axis_angle(vec3d a, double deg) {
  a = normalize(a);
  const double t = deg * std::numbers::pi / 180, c = std::cos(t), s = std::sin(t), k = 1 - c;
  Mat3 r;
  r.m = {c + a.x * a.x * k,       a.x * a.y * k - a.z * s, a.x * a.z * k + a.y * s,
         a.y * a.x * k + a.z * s, c + a.y * a.y * k,       a.y * a.z * k - a.x * s,
         a.z * a.x * k - a.y * s, a.z * a.y * k + a.x * s, c + a.z * a.z * k};
  return r;
}

Pose test_pose(const Layout& l) {
  Pose p;
  p.name = "test";
  const Skeleton& s = l.skeleton;
  if (l.templ == Template::GenericChain) {
    // The first chain: its first joint 90° in the drawing plane, the second 90° about X.
    int n = 0;
    for (const auto& t : l.tubes) {
      if (!t.chain || t.trunk) continue;
      for (const auto& seg : t.segments) {
        if (n == 0) p.local[seg.bone] = Mat3::axis_angle({0, 0, 1}, 90);
        else if (n == 1) p.local[seg.bone] = Mat3::axis_angle({1, 0, 0}, 90);
        ++n;
      }
      break;
    }
    p.notes.push_back("generic chain: the first chain's first joint is rotated 90° in the drawing plane, the second 90° about X");
    return p;
  }
  const int arm = s.find("LeftUpperArm"), knee = s.find("LeftLowerLeg");
  if (arm >= 0) p.local[arm] = Mat3::axis_angle({0, 0, 1}, 90);  // the left arm is on +X: raised towards +Y
  else p.notes.push_back("no arm bones: the arm raise is not part of this test pose");
  if (knee >= 0) p.local[knee] = Mat3::axis_angle({1, 0, 0}, 90);  // the foot goes back (−Z)
  else p.notes.push_back("no leg bones: the knee bend is not part of this test pose");
  return p;
}

PoseResult apply_pose(const SkinnedMesh& m, const Skeleton& s, const Pose& p) {
  PoseResult r;
  const size_t nb = s.bones.size();
  r.bone_world.resize(nb);
  r.skin.resize(nb);
  std::vector<bool> done(nb, false);
  // Parents first (bones are stored parents-first; loop until every bone is resolved anyway).
  for (size_t pass = 0; pass < nb + 1; ++pass)
    for (size_t b = 0; b < nb; ++b) {
      if (done[b]) continue;
      const Bone& bone = s.bones[b];
      const auto it = p.local.find(static_cast<int>(b));
      const Mat3 R = it == p.local.end() ? Mat3{} : it->second;
      if (bone.parent < 0) {
        r.bone_world[b] = {R, bone.head};
      } else {
        if (!done[static_cast<size_t>(bone.parent)]) continue;
        const Xform& P = r.bone_world[static_cast<size_t>(bone.parent)];
        r.bone_world[b] = {P.R * R, P.apply(bone.head - s.bones[static_cast<size_t>(bone.parent)].head)};
      }
      r.skin[b] = {r.bone_world[b].R, r.bone_world[b].t - r.bone_world[b].R * bone.head};
      done[b] = true;
    }
  r.positions.resize(m.positions.size());
  r.normals.resize(m.positions.size());
  for (size_t v = 0; v < m.positions.size(); ++v) {
    // Float weights sum to 1 only within float precision: normalise like an engine does.
    double wsum = 0;
    for (float w : m.weights[v]) wsum += std::max(0.f, w);
    vec3d p3{}, n3{};
    for (int k = 0; k < 4; ++k) {
      const double w = wsum > 0 ? m.weights[v][static_cast<size_t>(k)] / wsum : 0;
      if (w <= 0) continue;
      const Xform& X = r.skin[std::min<size_t>(m.joints[v][static_cast<size_t>(k)], nb - 1)];
      p3 = p3 + X.apply(m.positions[v]) * w;
      n3 = n3 + (X.R * m.normals[v]) * w;
    }
    r.positions[v] = p3;
    r.normals[v] = normalize(n3);
  }
  return r;
}

RigCheck check_rig(const SkinnedMesh& m, const Skeleton& s, const Pose& p, const PoseResult& r, const Assembly* as) {
  RigCheck c;
  c.vertices = static_cast<int>(m.positions.size());
  const size_t nb = s.bones.size();
  std::vector<std::vector<size_t>> rigid_of(nb);
  for (size_t v = 0; v < m.positions.size(); ++v) {
    double sum = 0;
    int infl = 0;
    bool neg = false, bad_index = false;
    int b0 = -1, b1 = -1;
    for (int k = 0; k < 4; ++k) {
      const float w = m.weights[v][static_cast<size_t>(k)];
      neg |= w < 0;
      if (w <= 0) continue;
      sum += w;
      ++infl;
      const int j = m.joints[v][static_cast<size_t>(k)];
      if (j < 0 || static_cast<size_t>(j) >= nb) bad_index = true;
      (b0 < 0 ? b0 : b1) = j;
    }
    if (infl == 0) {
      ++c.unskinned;
      continue;
    }
    if (neg || std::abs(sum - 1) > 1e-5) ++c.bad_weight_sum;
    if (bad_index) {
      ++c.bad_joint_index;
      continue;
    }
    if (infl > 2) ++c.non_adjacent;
    if (infl == 2) {
      const bool adj = s.bones[static_cast<size_t>(b0)].parent == b1 || s.bones[static_cast<size_t>(b1)].parent == b0;
      if (!adj) ++c.non_adjacent;
      if (m.vertex_joint_loop.empty() || !m.vertex_joint_loop[v]) ++c.blended_off_loop;
    }
    if (infl == 1 && std::abs(sum - 1) <= 1e-5) {
      ++c.rigid_vertices;
      const Xform& G = r.bone_world[static_cast<size_t>(b0)];
      // The rest position in the bone's frame, carried by the posed frame.
      const vec3d expect = G.apply(m.positions[v] - s.bones[static_cast<size_t>(b0)].head);
      c.rigid_max_error = std::max(c.rigid_max_error, length(r.positions[v] - expect));
      rigid_of[static_cast<size_t>(b0)].push_back(v);
    }
  }
  // Distances inside every rigid set (no matrices involved).
  for (const auto& set : rigid_of) {
    const size_t n = std::min<size_t>(set.size(), 160);
    for (size_t i = 0; i < n; ++i)
      for (size_t j = i + 1; j < n; ++j) {
        const size_t a = set[i * set.size() / n], b = set[j * set.size() / n];
        const double d0 = length(m.positions[a] - m.positions[b]), d1 = length(r.positions[a] - r.positions[b]);
        c.rigid_max_distance_change = std::max(c.rigid_max_distance_change, std::abs(d1 - d0));
      }
  }
  // Volume around every bent joint: the rings from just before its loops to just after.
  if (as)
    for (const auto& [bone, R] : p.local) {
      for (size_t ti = 0; ti < as->shapes.size(); ++ti) {
        const TubeShape& sh = as->shapes[ti];
        const TubeMesh& tm = as->shells[ti];
        int first = -1, last = -1;
        for (size_t i = 0; i < sh.rings.size(); ++i)
          if (sh.rings[i].joint_loop && sh.rings[i].bone_b == bone) {
            if (first < 0) first = static_cast<int>(i);
            last = static_cast<int>(i);
          }
        if (first < 0) continue;
        const int i0 = std::max(0, first - 1), i1 = std::min(static_cast<int>(sh.rings.size()) - 1, last + 1);
        std::vector<std::vector<vec3d>> rest, posed;
        for (int i = i0; i <= i1; ++i) {
          const LoftRing& ring = sh.rings[static_cast<size_t>(i)];
          std::vector<vec3d> a, b;
          for (int j = 0; j < sh.sides; ++j) {
            const vec3d q = tm.positions[static_cast<size_t>(i * sh.sides + j)];
            a.push_back(q);
            const vec3d pa = r.skin[static_cast<size_t>(ring.bone_a)].apply(q);
            b.push_back(ring.bone_b >= 0 && ring.w_b > 0
                            ? pa * (1 - ring.w_b) + r.skin[static_cast<size_t>(ring.bone_b)].apply(q) * ring.w_b
                            : pa);
          }
          rest.push_back(a);
          posed.push_back(b);
        }
        const double v0 = band_volume(rest);
        c.joint_volume.emplace_back(s.bones[static_cast<size_t>(bone)].name, v0 > 0 ? band_volume(posed) / v0 : 1.0);
      }
    }
  return c;
}

}  // namespace pixel3d::lowpoly
