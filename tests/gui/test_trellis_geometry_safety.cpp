// Regression tests for the TRELLIS cleanup geometry-safety fixes. The contact
// fixtures are exact F32 coordinates from a saved run (the anime-style character, 2026-10-19 13:56,
// QEM replay rounds 25 and 29) where the engine's opt-in collision guard let a
// fold and a sheet crossing through.
#include "trellis/qem_contact_guard.hpp"
#include "trellis/exact_degenerate.hpp"
#include "trellis/inner_shells.hpp"
#include "trellis/decimate_qem_guarded.hpp"
#include <cmath>
#include <iostream>
#include <map>
#include <random>
#include <string>

namespace {
using namespace p3dtrellis;
using contact::P;
using contact::Tri;
int failures = 0;
void check(bool ok, const std::string& what) {
  if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
}
P f(float x, float y, float z) { return {x, y, z}; }
// the anime-style character bounding box: diagonal 1.0285, largest |coordinate| 0.5052.
const contact::Tolerances anime = contact::tolerances(1.0285, 0.5052);

// The engine guard's test: SAT overlap of triangles shrunk 1e-7 towards their
// centroids (trellis.cpp include/triangle_intersection.h), restated here only to
// show which saved cases it accepted.
bool legacy_inset_overlap(Tri a, Tri b) {
  for (Tri* t : {&a, &b}) {
    P c{}; for (const P& p : *t) for (int k = 0; k < 3; ++k) c[size_t(k)] += p[size_t(k)] / 3;
    for (P& p : *t) for (int k = 0; k < 3; ++k) p[size_t(k)] += 1e-7 * (c[size_t(k)] - p[size_t(k)]);
  }
  using contact::sub; using contact::cross;
  const auto separated = [&](P axis) {
    if (axis == P{0, 0, 0}) return false;
    double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
    for (int i = 0; i < 3; ++i) {
      const double x = contact::dot(a[size_t(i)], axis), y = contact::dot(b[size_t(i)], axis);
      a0 = std::min(a0, x); a1 = std::max(a1, x); b0 = std::min(b0, y); b1 = std::max(b1, y);
    }
    return a1 < b0 || b1 < a0;
  };
  P ae[3], be[3];
  for (int i = 0; i < 3; ++i) { ae[i] = sub(a[size_t((i+1)%3)], a[size_t(i)]); be[i] = sub(b[size_t((i+1)%3)], b[size_t(i)]); }
  const P an = cross(ae[0], ae[1]), bn = cross(be[0], be[1]);
  if (separated(an) || separated(bn)) return false;
  for (const P& x : ae) for (const P& y : be) if (separated(cross(x, y))) return false;
  for (const P& e : ae) if (separated(cross(an, e))) return false;
  for (const P& e : be) if (separated(cross(bn, e))) return false;
  return true;
}

void audit_predicates() {
  const Tri a{f(0, 0, 0), f(1, 0, 0), f(0, 1, 0)};
  check(contact::intersect(a, {f(.2f, .2f, -1), f(.2f, .2f, 1), f(.8f, .2f, 0)}, 1e-10).kind == contact::Crossing, "crossing");
  check(contact::intersect(a, {f(.1f, .1f, 0), f(.4f, .1f, 0), f(.1f, .4f, 0)}, 1e-10).kind == contact::CoplanarOverlap, "coplanar overlap");
  check(contact::intersect(a, {f(0, 0, 0), f(1, 0, 0), f(0, -1, 0)}, 1e-10).kind == contact::EdgeContact, "edge contact");
  check(contact::intersect(a, {f(0, 0, 0), f(-1, 0, 0), f(0, -1, 0)}, 1e-10).kind == contact::PointContact, "point contact");
  check(contact::intersect(a, {f(0, 0, 1), f(1, 0, 1), f(0, 1, 1)}, 1e-10).kind == contact::Empty, "separation");
  check(contact::intersect(a, {f(.2f, .2f, 0), f(.6f, .2f, 0), f(.4f, .2f, 1)}, 1e-10).kind == contact::EdgeFaceContact, "edge-face contact");
  // Shared features explain their own contacts, nothing more.
  const contact::Tolerances unit{1e-12, 1e-7};
  check(contact::acceptable(a, {0, 1, 2}, {f(0, 0, 0), f(1, 0, 0), f(0, -1, 0)}, {0, 1, 3}, unit), "indexed shared edge");
  check(contact::acceptable(a, {0, 1, 2}, {f(0, 0, 0), f(-1, 0, 0), f(0, -1, 0)}, {0, 4, 3}, unit), "indexed shared vertex");
  check(contact::acceptable(a, {0, 1, 2}, {f(0, 0, 0), f(-1, 0, 0), f(0, -1, 0)}, {5, 4, 3}, unit), "pinch: same position, other id");
  check(!contact::acceptable(a, {0, 1, 2}, {f(0, 0, 0), f(1, 0, 0), f(.25f, .5f, 0)}, {0, 1, 3}, unit), "folded shared edge (coplanar overlap)");
  check(!contact::acceptable(a, {0, 1, 2}, {f(.2f, .2f, -1), f(.2f, .2f, 1), f(.8f, .2f, 0)}, {3, 4, 5}, unit), "crossing without shared features");
  check(!contact::acceptable(a, {0, 1, 2}, {f(0, 0, 0), f(.4f, .4f, 0), f(.4f, .2f, -1)}, {0, 3, 4}, unit), "contact along a segment from a shared vertex");
}

// R2: collapse of edge into vertex 44712 (round 25). Faces 88111 and 90327 of
// the new fan touch along 1.76 mm of 88111's edge: an edge-face contact the
// audit reports UNSAFE. The inset test accepted it.
void saved_fold() {
  const P v44712 = f(-0.13744938373565674f, 0.09519268572330475f, -0.44212448596954346f);
  const P v44713 = f(-0.1377098560333252f, 0.09577274322509766f, -0.44178807735443115f);
  const P v44715 = f(-0.1366426795721054f, 0.09626388549804688f, -0.44098952412605286f);
  const P v45776 = f(-0.13655509054660797f, 0.09528160095214844f, -0.44099748134613037f);
  const P v45798 = f(-0.13673026859760284f, 0.09724617004394531f, -0.44098156690597534f);
  const Tri a{v44712, v44713, v44715}, b{v44712, v45776, v45798};
  const auto hit = contact::intersect(a, b, anime.strict);
  check(hit.kind == contact::EdgeFaceContact && hit.span > 1e-3, "saved fold is an edge-face contact along 1.76 mm");
  check(!legacy_inset_overlap(a, b), "engine inset test misses the saved fold");
  check(!contact::acceptable(a, {44712, 44713, 44715}, b, {44712, 45776, 45798}, anime), "guard rejects the saved fold");
}

// R3: collapse moving apex 90118 -> 83945 (round 29) rotates a face of one
// separated sheet through its sister sheet around their shared edge position.
// The two faces meet only along that edge line, so no pair test can see it.
void saved_sheet_star() {
  const P p = f(-0.044608745723962784f, 0.07178102433681488f, 0.26233145594596863f);
  const P q = f(-0.04264328256249428f, 0.07189784198999405f, 0.26251861453056335f);
  const P a1 = f(-0.045185089111328125f, 0.07041554152965546f, 0.26293718814849854f);
  const P a2 = f(-0.04449513927102089f, 0.07273545861244202f, 0.261444091796875f);
  const P b1 = f(-0.04217294603586197f, 0.07174549996852875f, 0.264037162065506f);
  const P before = f(-0.042360030114650726f, 0.0728052482008934f, 0.2621530592441559f);
  const P after = f(-0.04266633465886116f, 0.07274895906448364f, 0.261696457862854f);
  const std::vector<int> sheet{0, 0, 1, 1};
  check(contact::star_class(p, q, {a1, a2, b1, before}, sheet, anime.strict) == 0, "saved star before the collapse: sheets touch");
  check(contact::star_class(p, q, {a1, a2, b1, after}, sheet, anime.strict) == 1, "saved star after the collapse: sheets cross");
  // Faces (q,p,a2) of sheet A and (p,q',after) of sheet B share the edge position.
  check(contact::acceptable({q, p, a2}, {83938, 81401, 82710}, {p, q, after}, {81401, 83946, 83945}, anime),
        "the crossing is invisible to pair tests (edge contact only)");
}

using Faces = std::vector<int32_t>;
size_t boundary_edges(const Faces& faces) {
  std::map<std::pair<int32_t, int32_t>, int> count;
  for (size_t i = 0; i < faces.size(); i += 3) for (int k = 0; k < 3; ++k) {
    const int32_t a = faces[i+size_t(k)], b = faces[i+size_t((k+1)%3)];
    ++count[{std::min(a, b), std::max(a, b)}];
  }
  size_t open = 0;
  for (const auto& [edge, n] : count) open += n == 1;
  return open;
}

// R5: octahedron whose +z vertex is split into two ids at the same position,
// stitched by two zero-area triangles (the shape DC produced in three saved runs).
void zero_length_slit() {
  const std::vector<float> verts{1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 1};
  const Faces slit{4, 0, 2, 4, 2, 1, 6, 1, 3, 6, 3, 0, 5, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 0, 4, 6, 4, 1, 6};
  check(boundary_edges(slit) == 0, "slit input is closed");
  auto deleted = slit; auto positions = verts;
  const auto old = remove_exact_degenerate(positions, deleted);
  check(old.removed_faces == 2 && boundary_edges(deleted) == 4, "deleting the zero-area faces opens a 4-edge slit");
  auto zipped = slit;
  const auto r = collapse_zero_length_edges(verts, zipped);
  check(r.zero_length_edges == 1 && r.collapsed == 1 && r.faces_removed == 2 && r.skipped == 0, "zero-length edge collapsed");
  check(zipped.size() == 24 && boundary_edges(zipped) == 0, "collapse keeps the surface closed");
  auto again = zipped; positions = verts;
  check(remove_exact_degenerate(positions, again).removed_faces == 0, "no zero-area face remains");
  // A closed tetrahedron with a zero-length edge must not become two copies of one triangle.
  const std::vector<float> tv{0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
  Faces tetra{0, 1, 2, 0, 3, 1, 1, 3, 2, 0, 2, 3};
  const auto t = collapse_zero_length_edges(tv, tetra);
  check(t.collapsed == 0 && t.skipped == 1 && tetra.size() == 12, "tetrahedron link condition");
}

void cube(std::vector<float>& v, Faces& f, float lo, float hi, bool inward) {
  const int32_t o = int32_t(v.size() / 3);
  for (int i = 0; i < 8; ++i) { v.push_back(i & 1 ? hi : lo); v.push_back(i & 2 ? hi : lo); v.push_back(i & 4 ? hi : lo); }
  const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  for (const auto& q : quads) {
    Faces t{q[0], q[1], q[2], q[0], q[2], q[3]};
    if (inward) for (size_t i = 0; i < t.size(); i += 3) std::swap(t[i+1], t[i+2]);
    for (int32_t x : t) f.push_back(o + x);
  }
}

// R6: only closed, negative shells verified inside a positive solid go.
void inner_shells() {
  std::vector<float> v; Faces f;
  cube(v, f, -1, 1, false);          // outer solid
  cube(v, f, -.5f, .5f, true);       // sealed inner offset shell
  cube(v, f, -.2f, .2f, false);      // positive island inside it: kept
  cube(v, f, 3, 4, true);            // inverted shell outside everything: kept
  const auto r = remove_enclosed_inner_shells(v, f);
  check(r.components == 4 && r.removed_shells == 1 && r.removed_faces == 12 && r.kept_negative_closed == 1, "inner shell removal");
  check(f.size() == 108 && v.size() == 72, "three components kept, unreferenced positions dropped");
  check(std::abs(r.removed_volume + 1.0) < 1e-9 && std::abs(r.removed_area - 6.0) < 1e-9, "removed shell measured");
}

// Brute-force audit of UNSAFE pair contacts using the audit's rules.
size_t unsafe_pairs(const std::vector<float>& v, const Faces& f, const contact::Tolerances& eps) {
  const auto tri = [&](size_t i) {
    Tri t; for (int k = 0; k < 3; ++k) { const size_t x = size_t(f[3*i+size_t(k)]); t[size_t(k)] = {v[3*x], v[3*x+1], v[3*x+2]}; }
    return t;
  };
  size_t bad = 0;
  for (size_t i = 0; i < f.size() / 3; ++i) for (size_t j = i + 1; j < f.size() / 3; ++j) {
    const Tri a = tri(i), b = tri(j);
    const auto hit = contact::intersect(a, b, eps.strict);
    if (hit.kind < contact::EdgeFaceContact) continue;
    const contact::Shared s = contact::shared(a, {f[3*i], f[3*i+1], f[3*i+2]}, b, {f[3*j], f[3*j+1], f[3*j+2]});
    if (s.ids == 2 && hit.kind != contact::CoplanarOverlap) continue;  // indexed shared edge (audit rule)
    ++bad;
  }
  return bad;
}

// Thin closed plate, top and bottom 0.004 apart: the shape of a sword blade or
// a cloak. Unguarded topology-preserving QEM folds the sheets through each
// other; the guard must not. Bumps of 5x the thickness leave few safe
// collapses (the guard refuses the rest); bumps the size of the thickness must
// still simplify well.
void thin_plate_qem(float bumps, bool expect_unguarded_defect, double min_reduction) {
  const int n = 24;
  const float gap = .004f;
  std::mt19937 random(7);
  std::uniform_real_distribution<float> noise(-bumps, bumps), shift(-.004f, .004f);
  // Top and bottom share each grid point's x/y jitter and height: the sheets
  // are a strict vertical offset of one triangulation and cannot meet. The
  // jitter keeps the side walls from being exactly coplanar (see README).
  std::vector<float> height(size_t((n+1)*(n+1))), jx(height.size()), jy(height.size());
  for (size_t i = 0; i < height.size(); ++i) { height[i] = noise(random); jx[i] = shift(random); jy[i] = shift(random); }
  std::vector<float> v; Faces f;
  const auto id = [&](int layer, int i, int j) { return int32_t(layer*(n+1)*(n+1) + i*(n+1) + j); };
  for (int layer = 0; layer < 2; ++layer) for (int i = 0; i <= n; ++i) for (int j = 0; j <= n; ++j) {
    const size_t g = size_t(i*(n+1)+j);
    v.push_back(float(i) / n + jx[g]); v.push_back(float(j) / n + jy[g]);
    v.push_back(height[g] + (layer ? gap / 2 : -gap / 2));
  }
  for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) {
    const int32_t a = id(1, i, j), b = id(1, i+1, j), c = id(1, i+1, j+1), d = id(1, i, j+1);
    f.insert(f.end(), {a, b, c, a, c, d});
    const int32_t e = id(0, i, j), g = id(0, i+1, j), h = id(0, i+1, j+1), k = id(0, i, j+1);
    f.insert(f.end(), {e, h, g, e, k, h});
  }
  for (int s = 0; s < n; ++s) {  // side walls close the plate
    const int32_t t0 = id(1, s, 0), t1 = id(1, s+1, 0), b0 = id(0, s, 0), b1 = id(0, s+1, 0);
    f.insert(f.end(), {t0, b0, b1, t0, b1, t1});
    const int32_t u0 = id(1, s, n), u1 = id(1, s+1, n), c0 = id(0, s, n), c1 = id(0, s+1, n);
    f.insert(f.end(), {u0, u1, c1, u0, c1, c0});
    const int32_t w0 = id(1, 0, s), w1 = id(1, 0, s+1), d0 = id(0, 0, s), d1 = id(0, 0, s+1);
    f.insert(f.end(), {w0, w1, d1, w0, d1, d0});
    const int32_t x0 = id(1, n, s), x1 = id(1, n, s+1), e0 = id(0, n, s), e1 = id(0, n, s+1);
    f.insert(f.end(), {x0, e0, e1, x0, e1, x1});
  }
  check(boundary_edges(f) == 0, "plate is closed");
  const auto eps = contact::tolerances(std::sqrt(2.0), 1.0);
  check(unsafe_pairs(v, f, eps) == 0, "plate input has no crossing");
  const int target = int(f.size() / 3) / 12;
  std::vector<float> ov; Faces of;
  GuardedQemOptions off; off.contact_guard = false;
  decimate_qem_guarded(v, f, target, ov, of, off);
  const size_t unguarded = unsafe_pairs(ov, of, eps);
  if (expect_unguarded_defect) check(unguarded > 0, "unguarded QEM folds the plate (defect reproduced)");
  // Default: the ladder stops at the shipped error level. These synthetic
  // bumps cost far more than that, so the plate is kept as it is.
  const auto capped = decimate_qem_guarded(v, f, target, ov, of);
  check(!capped.budget_met && capped.final_threshold <= 1.5e-5 && unsafe_pairs(ov, of, eps) == 0,
        "default threshold cap keeps faces instead of accepting high-error collapses");
  GuardedQemOptions on;
  on.max_threshold = 1e12f;  // exercise the guard alone
  const auto r = decimate_qem_guarded(v, f, target, ov, of, on);
  check(unsafe_pairs(ov, of, eps) == 0, "guarded QEM keeps the plate free of crossings");
  check(boundary_edges(of) == 0, "guarded QEM keeps the plate closed");
  check(double(r.output_faces) <= (1 - min_reduction) * double(f.size() / 3), "guarded QEM still simplifies");
  std::cout << "thin plate (bumps " << bumps << "): " << f.size() / 3 << " faces -> unguarded " << unguarded << " unsafe pairs; guarded "
            << r.output_faces << " faces, rejected contact=" << r.rejected_contact << " same-round=" << r.rejected_batch << "\n";
}
}  // namespace

int main() {
  audit_predicates();
  saved_fold();
  saved_sheet_star();
  zero_length_slit();
  inner_shells();
  thin_plate_qem(.02f, true, .1);
  thin_plate_qem(.004f, false, .6);
  if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
  std::cout << "PASS: audit predicates, saved fold, saved sheet star, zero-length slit, inner shells, thin plate QEM\n";
  return 0;
}
