#include "pixel3d/lowpoly/assemble.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numbers>

namespace pixel3d::lowpoly {
namespace {

vec3d tri_normal(const vec3d& a, const vec3d& b, const vec3d& c) { return cross(b - a, c - a); }

}  // namespace

Mesh shell_mesh(const TubeMesh& t) {
  Mesh m;
  m.positions = t.positions;
  m.tris = t.tris;
  m.tri_part.assign(t.tris.size(), 0);
  return m;
}

Assembly assemble(const Layout& l, const Allocation& a, int H) {
  Assembly out;
  SkinnedMesh& M = out.mesh;
  for (size_t ti = 0; ti < l.tubes.size(); ++ti) {
    const TubeSpec& spec = l.tubes[ti];
    const TubeShape shape = shape_for(l, spec, a.tubes[ti], H);
    const TubeMesh tm = build_tube(shape);
    const int N = shape.sides, R = static_cast<int>(shape.rings.size());
    const int shell = static_cast<int>(ti);
    // Smooth normals from body and apex triangles (flat caps keep their own normal).
    std::vector<vec3d> wn(tm.positions.size());
    for (const auto& t : tm.tris) {
      const bool flat_cap = tm.vertex_ring[t[0]] == tm.vertex_ring[t[1]] && tm.vertex_ring[t[1]] == tm.vertex_ring[t[2]] &&
                            tm.vertex_side[t[0]] >= 0 && tm.vertex_side[t[1]] >= 0 && tm.vertex_side[t[2]] >= 0;
      if (flat_cap) continue;
      const vec3d n = tri_normal(tm.positions[t[0]], tm.positions[t[1]], tm.positions[t[2]]);
      for (uint32_t v : t) wn[v] = wn[v] + n;
    }
    for (auto& n : wn) n = normalize(n);
    auto skin = [&](const LoftRing& r, std::array<uint16_t, 4>& j, std::array<float, 4>& w) {
      j = {static_cast<uint16_t>(std::max(0, r.bone_a)), 0, 0, 0};
      w = {1, 0, 0, 0};
      if (r.joint_loop && r.bone_b >= 0 && r.w_b > 0) {
        j[1] = static_cast<uint16_t>(r.bone_b);
        w[0] = 1 - r.w_b;
        w[1] = r.w_b;
      }
    };
    auto add_vertex = [&](vec3d p, vec3d n, vec2d uv, const LoftRing& r, int island) {
      std::array<uint16_t, 4> j;
      std::array<float, 4> w;
      skin(r, j, w);
      M.positions.push_back(p);
      M.normals.push_back(n);
      M.uvs.push_back(uv);
      M.joints.push_back(j);
      M.weights.push_back(w);
      M.vertex_shell.push_back(shell);
      M.vertex_part.push_back(r.owner >= 0 ? r.owner : spec.parts.front());
      M.vertex_joint_loop.push_back(r.joint_loop);
      out.vertex_island.push_back(island);
      return static_cast<uint32_t>(M.positions.size() - 1);
    };
    // ---- body island: seam at the back-most side ------------------------------
    int j0 = 0;
    for (int j = 1; j < N; ++j)
      if (std::sin(2 * std::numbers::pi * j / N) < std::sin(2 * std::numbers::pi * j0 / N) - 1e-12) j0 = j;
    const int body_island = static_cast<int>(out.islands.size());
    out.islands.push_back({shell, 0, spec.parts.front(), {}, {}});
    std::vector<double> vcoord(static_cast<size_t>(R), 0);
    for (int i = 1; i < R; ++i) {
      double s = 0;
      for (int j = 0; j < N; ++j) s += length(tm.positions[static_cast<size_t>(i * N + j)] - tm.positions[static_cast<size_t>((i - 1) * N + j)]);
      vcoord[static_cast<size_t>(i)] = vcoord[static_cast<size_t>(i) - 1] + s / N;
    }
    std::vector<uint32_t> body(static_cast<size_t>(R) * (N + 1));
    double umax = 0;
    for (int i = 0; i < R; ++i) {
      double per = 0;
      for (int k = 0; k < N; ++k)
        per += length(tm.positions[static_cast<size_t>(i * N + (j0 + k + 1) % N)] - tm.positions[static_cast<size_t>(i * N + (j0 + k) % N)]);
      umax = std::max(umax, per / 2);
      double c = 0;
      for (int k = 0; k <= N; ++k) {
        const int j = (j0 + k) % N;
        const size_t tv = static_cast<size_t>(i * N + j);
        body[static_cast<size_t>(i * (N + 1) + k)] =
            add_vertex(tm.positions[tv], wn[tv], {c - per / 2, vcoord[static_cast<size_t>(i)]}, shape.rings[static_cast<size_t>(i)], body_island);
        if (k < N) c += length(tm.positions[static_cast<size_t>(i * N + (j + 1) % N)] - tm.positions[tv]);
      }
    }
    out.islands[static_cast<size_t>(body_island)].lo = {-umax, 0};
    out.islands[static_cast<size_t>(body_island)].hi = {umax, vcoord.back()};
    auto local_k = [&](int j) { return (j - j0 + N) % N; };
    // ---- caps ---------------------------------------------------------------------
    std::vector<int> cap_island(2, -1);
    std::vector<std::vector<uint32_t>> cap_ring(2);
    std::vector<uint32_t> cap_apex(2, 0);
    for (int e = 0; e < 2; ++e) {
      const int ring = e == 0 ? 0 : R - 1;
      const LoftRing& r = shape.rings[static_cast<size_t>(ring)];
      const Cap cap = e == 0 ? shape.cap_start : shape.cap_end;
      const vec3d e1 = r.n, e2{0, 0, 1};
      cap_island[static_cast<size_t>(e)] = static_cast<int>(out.islands.size());
      Island isl{shell, 1 + e, r.owner, {1e30, 1e30}, {-1e30, -1e30}};
      // Flat cap normal: the cap faces away from the tube.
      vec3d axis = R > 1 ? normalize(shape.rings[static_cast<size_t>(R > 1 ? (e == 0 ? 1 : R - 2) : 0)].c - r.c) : vec3d{0, 1, 0};
      vec3d capn = e == 0 ? -axis : axis;
      if (R == 1) capn = e == 0 ? normalize(r.c - shape.apex_end) : normalize(shape.apex_end - r.c);
      for (int j = 0; j < N; ++j) {
        const size_t tv = static_cast<size_t>(ring * N + j);
        const vec3d p = tm.positions[tv];
        const vec2d uv{dot(p - r.c, e1), dot(p - r.c, e2)};
        isl.lo = {std::min(isl.lo.x, uv.x), std::min(isl.lo.y, uv.y)};
        isl.hi = {std::max(isl.hi.x, uv.x), std::max(isl.hi.y, uv.y)};
        cap_ring[static_cast<size_t>(e)].push_back(
            add_vertex(p, cap == Cap::Flat ? capn : wn[tv], uv, r, cap_island[static_cast<size_t>(e)]));
      }
      if (cap == Cap::Apex) {
        // The apex vertex of the welded mesh: the one with side −1 belonging to this end.
        for (size_t v = 0; v < tm.positions.size(); ++v)
          if (tm.vertex_side[v] < 0 && tm.vertex_ring[v] == ring &&
              length(tm.positions[v] - (e == 0 ? shape.apex_start : shape.apex_end)) < 1e-9) {
            const vec2d uv{dot(tm.positions[v] - r.c, e1), dot(tm.positions[v] - r.c, e2)};
            cap_apex[static_cast<size_t>(e)] = add_vertex(tm.positions[v], wn[v], uv, r, cap_island[static_cast<size_t>(e)]);
          }
      }
      out.islands.push_back(isl);
    }
    const size_t first_tri = M.indices.size() / 3;
    // ---- triangles ----------------------------------------------------------------
    for (const auto& t : tm.tris) {
      const bool apex = tm.vertex_side[t[0]] < 0 || tm.vertex_side[t[1]] < 0 || tm.vertex_side[t[2]] < 0;
      const bool same_ring = tm.vertex_ring[t[0]] == tm.vertex_ring[t[1]] && tm.vertex_ring[t[1]] == tm.vertex_ring[t[2]];
      std::array<uint32_t, 3> o{};
      if (apex || same_ring) {
        // Cap triangle: which end? (R == 1: the flat one is the start, the apex the end.)
        const int ring = tm.vertex_ring[t[0]];
        int e = ring == 0 ? 0 : 1;
        if (R == 1) e = apex ? (shape.cap_end == Cap::Apex ? 1 : 0) : (shape.cap_start == Cap::Flat ? 0 : 1);
        for (int k = 0; k < 3; ++k)
          o[static_cast<size_t>(k)] = tm.vertex_side[t[static_cast<size_t>(k)]] < 0
                                          ? cap_apex[static_cast<size_t>(e)]
                                          : cap_ring[static_cast<size_t>(e)][static_cast<size_t>(tm.vertex_side[t[static_cast<size_t>(k)]])];
      } else {
        int ks[3];
        bool has0 = false, hasN1 = false;
        for (int k = 0; k < 3; ++k) {
          ks[k] = local_k(tm.vertex_side[t[static_cast<size_t>(k)]]);
          has0 |= ks[k] == 0;
          hasN1 |= ks[k] == N - 1;
        }
        for (int k = 0; k < 3; ++k) {
          const int kk = (has0 && hasN1 && ks[k] == 0) ? N : ks[k];
          o[static_cast<size_t>(k)] = body[static_cast<size_t>(tm.vertex_ring[t[static_cast<size_t>(k)]] * (N + 1) + kk)];
        }
      }
      M.indices.insert(M.indices.end(), o.begin(), o.end());
      M.tri_shell.push_back(shell);
    }
    // ---- normals: smooth around sections, hard across creases ---------------------
    {
      const size_t nt = tm.tris.size();
      std::vector<vec3d> fn(nt);
      std::vector<int> kind(nt);  // 0 body, 1 flat cap, 2 apex
      for (size_t t = 0; t < nt; ++t) {
        const auto& tr = tm.tris[t];
        fn[t] = tri_normal(tm.positions[tr[0]], tm.positions[tr[1]], tm.positions[tr[2]]);  // area-weighted
        const bool apex = tm.vertex_side[tr[0]] < 0 || tm.vertex_side[tr[1]] < 0 || tm.vertex_side[tr[2]] < 0;
        const bool same = tm.vertex_ring[tr[0]] == tm.vertex_ring[tr[1]] && tm.vertex_ring[tr[1]] == tm.vertex_ring[tr[2]];
        kind[t] = apex ? 2 : same ? 1 : 0;
      }
      const double cos_crease = std::cos(kCreaseDeg * std::numbers::pi / 180);
      // Around a 6–8-sided section neighbours differ by 45–60°: smooth; a fold beyond 75° stays hard.
      const double kAroundSectionCos = std::cos(75 * std::numbers::pi / 180);
      auto soft = [&](size_t f, size_t g, uint32_t v, uint32_t w) {
        const double c = dot(normalize(fn[f]), normalize(fn[g]));
        if (kind[f] == 0 && kind[g] == 0)  // across a ring edge the profile may turn; around the section it is round
          return tm.vertex_ring[v] == tm.vertex_ring[w] ? c >= cos_crease : c >= kAroundSectionCos;
        if (kind[f] == 2 && kind[g] == 2) return c >= kAroundSectionCos;  // around a cone tip
        return c >= cos_crease;
      };
      std::vector<std::vector<size_t>> inc(tm.positions.size());
      for (size_t t = 0; t < nt; ++t)
        for (uint32_t v : tm.tris[t]) inc[v].push_back(t);
      // corner[t][k]: normal of triangle t at its k-th corner.
      std::vector<std::array<vec3d, 3>> corner(nt);
      for (uint32_t v = 0; v < tm.positions.size(); ++v) {
        const auto& F = inc[v];
        std::vector<size_t> grp(F.size());
        for (size_t i = 0; i < F.size(); ++i) grp[i] = i;
        std::function<size_t(size_t)> root = [&](size_t i) { return grp[i] == i ? i : grp[i] = root(grp[i]); };
        for (size_t i = 0; i < F.size(); ++i)
          for (size_t j = i + 1; j < F.size(); ++j) {
            // The other shared vertex of an edge through v.
            for (uint32_t w : tm.tris[F[i]]) {
              if (w == v) continue;
              const auto& tg = tm.tris[F[j]];
              if (std::find(tg.begin(), tg.end(), w) == tg.end()) continue;
              if (soft(F[i], F[j], v, w)) grp[root(i)] = root(j);
            }
          }
        std::map<size_t, vec3d> sum;
        for (size_t i = 0; i < F.size(); ++i) sum[root(i)] = sum[root(i)] + fn[F[i]];
        for (size_t i = 0; i < F.size(); ++i) {
          const auto& tr = tm.tris[F[i]];
          const int k = tr[0] == v ? 0 : tr[1] == v ? 1 : 2;
          // Chained soft edges can bend a group far from one of its faces: never past 80°.
          const vec3d gn = normalize(sum[root(i)]), own = normalize(fn[F[i]]);
          corner[F[i]][static_cast<size_t>(k)] = dot(gn, own) >= std::cos(80 * std::numbers::pi / 180) ? gn : own;
        }
      }
      // Give each mesh corner its normal; a vertex whose corners disagree is split.
      std::vector<std::vector<std::pair<vec3d, uint32_t>>> copies(M.positions.size());
      for (size_t t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k) {
          uint32_t& gv = M.indices[3 * (first_tri + t) + static_cast<size_t>(k)];
          const vec3d n = corner[t][static_cast<size_t>(k)];
          if (copies.size() < M.positions.size()) copies.resize(M.positions.size());
          auto& c = copies[gv];
          uint32_t use = UINT32_MAX;
          for (const auto& [cn, idx] : c)
            if (length(cn - n) < 1e-9) use = idx;
          if (use == UINT32_MAX) {
            if (c.empty()) {
              use = gv;
            } else {
              use = static_cast<uint32_t>(M.positions.size());
              M.positions.push_back(M.positions[gv]);
              M.normals.push_back(n);
              M.uvs.push_back(M.uvs[gv]);
              M.joints.push_back(M.joints[gv]);
              M.weights.push_back(M.weights[gv]);
              M.vertex_shell.push_back(M.vertex_shell[gv]);
              M.vertex_part.push_back(M.vertex_part[gv]);
              M.vertex_joint_loop.push_back(M.vertex_joint_loop[gv]);
              out.vertex_island.push_back(out.vertex_island[gv]);
            }
            M.normals[use] = n;
            c.push_back({n, use});
          }
          gv = use;
        }
    }
    out.shells.push_back(tm);
    out.shapes.push_back(shape);
  }
  return out;
}

}  // namespace pixel3d::lowpoly
