#pragma once
// Exact contact rules for guarded QEM edge collapses.
//
// A collapse may only be applied when none of its new triangles adds a surface
// contact that the read-only audit (tools/smartquad/geometry_audit.cpp) reports
// as UNSAFE: transverse crossing, edge-face contact, coplanar area overlap or a
// crossing sheet star. The audit remains a separate, unchanged program; this
// header restates its predicates (same plane sections, coplanar clipping,
// tolerances and star alternation) so the simplifier can test a collapse before
// it happens. Nothing here moves, welds or deletes geometry.
//
// Sheet separation leaves distinct vertex ids at identical positions. Contacts
// through such shared positions already exist in the input; they are accepted
// only in their existing form (a point at a shared point, an edge along a shared
// edge whose surrounding sheets do not alternate).
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <vector>

namespace p3dtrellis::contact {

using P = std::array<double, 3>;
using Tri = std::array<P, 3>;

inline P sub(P a, P b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
inline P add(P a, P b) { return {a[0]+b[0], a[1]+b[1], a[2]+b[2]}; }
inline P mul(P a, double s) { return {a[0]*s, a[1]*s, a[2]*s}; }
inline double dot(P a, P b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
inline P cross(P a, P b) { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
inline double norm(P a) { return std::sqrt(dot(a, a)); }
inline P unit(P a) { return mul(a, 1 / norm(a)); }
inline P normal(const Tri& t) { return unit(cross(sub(t[1], t[0]), sub(t[2], t[0]))); }
inline bool degenerate(const Tri& t) { return norm(cross(sub(t[1], t[0]), sub(t[2], t[0]))) == 0; }

enum Kind { Empty, PointContact, EdgeContact, EdgeFaceContact, Crossing, CoplanarOverlap };
struct Hit { Kind kind = Empty; P point{}; double span = 0, area = 0; };

// Audit tolerances: strict = 1e-12 of the bounding-box diagonal; scale also
// covers F32 coordinate rounding.
// allow_touching (opt-in, asset agent budgets): a NEW point or edge contact is accepted (the audit
// rates those REVIEW); crossings, edge-face contacts, coplanar overlaps and crossing sheet stars are
// still refused (UNSAFE). Off (default) = the shipped cleanup behaviour, unchanged.
struct Tolerances { double strict = 0, scale = 0; bool allow_touching = false; };
inline Tolerances tolerances(double diagonal, double max_abs_coordinate) {
    return {std::max(diagonal * 1e-12, 1e-30),
            std::max({4 * double(FLT_EPSILON) * max_abs_coordinate, diagonal * 1e-10, 1e-30})};
}

namespace detail {
inline bool interior(P p, const Tri& t, double eps) {
    const P n = normal(t);
    for (int k = 0; k < 3; ++k) {
        const P e = sub(t[(k+1)%3], t[k]);
        if (dot(n, cross(e, sub(p, t[k]))) <= eps * norm(e)) return false;
    }
    return true;
}
inline int section(const Tri& t, const std::array<double, 3>& d, double eps, P out[6]) {
    int count = 0;
    for (int k = 0; k < 3; ++k) {
        const int j = (k+1) % 3;
        if (std::abs(d[k]) <= eps) out[count++] = t[k];
        if ((d[k] < -eps && d[j] > eps) || (d[k] > eps && d[j] < -eps))
            out[count++] = add(t[k], mul(sub(t[j], t[k]), d[k] / (d[k] - d[j])));
    }
    return count;
}
}  // namespace detail

// Same classification as the audit's intersect(): plane sections for
// transverse pairs, polygon clipping for coplanar pairs.
inline Hit intersect(const Tri& a, const Tri& b, double eps) {
    using namespace detail;
    const P an = normal(a), bn = normal(b);
    std::array<double, 3> da{}, db{};
    for (int k = 0; k < 3; ++k) { da[k] = dot(bn, sub(a[k], b[0])); db[k] = dot(an, sub(b[k], a[0])); }
    const auto separated = [&](const std::array<double, 3>& d) {
        return *std::min_element(d.begin(), d.end()) > eps || *std::max_element(d.begin(), d.end()) < -eps;
    };
    if (separated(da) || separated(db)) return {};
    double max_distance = 0, local = 0;
    for (int k = 0; k < 3; ++k) {
        max_distance = std::max({max_distance, std::abs(da[k]), std::abs(db[k])});
        local = std::max({local, norm(sub(a[k], a[(k+1)%3])), norm(sub(b[k], b[(k+1)%3]))});
    }
    P direction = cross(an, bn);
    const double angle = norm(direction);
    if (max_distance <= eps && angle <= eps / std::max(local, eps)) {
        int drop = 0;
        for (int k = 1; k < 3; ++k) if (std::abs(an[k]) > std::abs(an[drop])) drop = k;
        const int u = (drop+1) % 3, v = (drop+2) % 3;
        const auto orient = [&](P p, P q, P r) { return (q[u]-p[u])*(r[v]-p[v]) - (q[v]-p[v])*(r[u]-p[u]); };
        const double sign = orient(b[0], b[1], b[2]) >= 0 ? 1 : -1;
        std::vector<P> polygon(a.begin(), a.end());
        for (int k = 0; k < 3 && !polygon.empty(); ++k) {
            const P p = b[k], q = b[(k+1)%3];
            const double length = std::hypot(q[u]-p[u], q[v]-p[v]);
            std::vector<P> next;
            for (size_t j = 0; j < polygon.size(); ++j) {
                const P s = polygon[j], t = polygon[(j+1) % polygon.size()];
                const double ds = sign * orient(p, q, s), dt = sign * orient(p, q, t);
                const bool in_s = ds >= -eps * length, in_t = dt >= -eps * length;
                if (in_s) next.push_back(s);
                if (in_s != in_t && ds != dt) next.push_back(add(s, mul(sub(t, s), (ds + eps*length) / (ds - dt))));
            }
            polygon.swap(next);
        }
        if (polygon.empty()) return {};
        Hit r;
        double projected = 0, perimeter = 0;
        for (size_t j = 0; j < polygon.size(); ++j) {
            const P p = polygon[j], q = polygon[(j+1) % polygon.size()];
            r.point = add(r.point, p);
            projected += p[u]*q[v] - p[v]*q[u];
            perimeter += norm(sub(q, p));
        }
        r.point = mul(r.point, 1.0 / double(polygon.size()));
        r.area = std::abs(projected) * .5 / std::abs(an[drop]);
        for (P p : polygon) for (P q : polygon) r.span = std::max(r.span, norm(sub(p, q)));
        if (r.area > 4 * eps * perimeter) { r.kind = CoplanarOverlap; return r; }
        r.kind = r.span > 4 * eps ? EdgeContact : PointContact;
        return r;
    }
    if (angle <= 1e-15) return {};
    direction = mul(direction, 1 / angle);
    P sa[6], sb[6];
    const int na = section(a, da, eps, sa), nb = section(b, db, eps, sb);
    if (!na || !nb) return {};
    const auto range = [&](const P* s, int n) {
        std::array<double, 2> r{1e300, -1e300};
        for (int i = 0; i < n; ++i) { const double t = dot(s[i], direction); r[0] = std::min(r[0], t); r[1] = std::max(r[1], t); }
        return r;
    };
    const auto ra = range(sa, na), rb = range(sb, nb);
    const double lo = std::max(ra[0], rb[0]), hi = std::min(ra[1], rb[1]);
    if (hi < lo - eps) return {};
    Hit r;
    r.span = std::max(0.0, hi - lo);
    r.point = add(sa[0], mul(direction, (lo + hi) * .5 - dot(sa[0], direction)));
    const bool ia = interior(r.point, a, eps), ib = interior(r.point, b, eps);
    r.kind = ia && ib ? Crossing : ia || ib ? EdgeFaceContact : r.span > 4*eps ? EdgeContact : PointContact;
    return r;
}

// Features two triangles share: vertex ids, and exactly equal positions (which
// include every shared id).
struct Shared { int ids = 0; int points = 0; std::array<P, 3> at{}; };
inline Shared shared(const Tri& a, const std::array<int32_t, 3>& ia, const Tri& b, const std::array<int32_t, 3>& ib) {
    Shared s;
    for (int i = 0; i < 3; ++i) {
        bool id = false, point = false;
        for (int j = 0; j < 3; ++j) { id |= ia[i] == ib[j]; point |= a[i] == b[j]; }
        s.ids += id;
        if (point) s.at[size_t(s.points++)] = a[i];
    }
    return s;
}

// True when a hit is fully explained by the shared features:
// - an indexed shared edge: anything except a coplanar area overlap (audit rule);
// - a shared edge position between separated sheets: contact along that edge;
// - one shared point (id or position): a point contact at that point (audit rule);
// - nothing shared: no contact at all.
inline bool expected(const Hit& hit, const Shared& s, double eps) {
    if (hit.kind == Empty) return true;
    if (s.points >= 3) return false;
    if (s.points == 2) return s.ids == 2 ? hit.kind != CoplanarOverlap : hit.kind <= EdgeContact;
    if (s.points == 1)
        return hit.kind <= EdgeContact && hit.span <= 4 * eps && norm(sub(hit.point, s.at[0])) <= 4 * eps;
    return false;
}

// Same decision as the audit: the strict-tolerance hit decides. A pair that is
// strictly disjoint can only be a "near_contact_only" at the F32 scale
// tolerance, which the audit never counts toward REVIEW or UNSAFE. Saved inputs
// already hold tens of thousands of such pairs where two surfaces lie within
// ~2e-7 of each other (Gemini sheets: 69,873); refusing every collapse there
// stalled simplification at 3.5x the face budget without changing any status.
inline bool acceptable(const Tri& a, const std::array<int32_t, 3>& ia, const Tri& b,
                       const std::array<int32_t, 3>& ib, const Tolerances& eps) {
    const Hit strict = intersect(a, b, eps.strict);
    if (eps.allow_touching && strict.kind <= EdgeContact) return true;
    return strict.kind == Empty || expected(strict, shared(a, ia, b, ib), eps.strict);
}

// Faces around one geometric edge p-q. `sheet` identifies the indexed edge each
// face uses. Mirrors the audit's edge-star rule: 0 = sheets do not alternate,
// 1 = crossing (alternating) sheets, 2 = angular tie, degenerate or not a
// two-sheet star.
inline int star_class(P p, P q, const std::vector<P>& apex, const std::vector<int>& sheet, double eps) {
    const size_t n = apex.size();
    int sheets = 0;
    for (size_t i = 0; i < n; ++i) {
        bool seen = false;
        for (size_t j = 0; j < i; ++j) seen |= sheet[j] == sheet[i];
        sheets += !seen;
    }
    if (n != 4 || sheets != 2) return 2;
    const P axis = unit(sub(q, p));
    const P basis = unit(cross(axis, std::abs(axis[0]) < .8 ? P{1, 0, 0} : P{0, 1, 0}));
    const P basis2 = cross(axis, basis);
    std::array<double, 4> angles{};
    double altitude = 1e300;
    for (size_t i = 0; i < 4; ++i) {
        P ray = sub(apex[i], p);
        ray = sub(ray, mul(axis, dot(ray, axis)));
        altitude = std::min(altitude, norm(ray));
        double angle = std::atan2(dot(ray, basis2), dot(ray, basis));
        if (angle < 0) angle += 2 * std::acos(-1.0);
        angles[i] = angle;
    }
    if (!(altitude > eps)) return 2;
    const double angular_eps = eps / altitude;
    std::array<int, 4> order{0, 1, 2, 3};
    std::sort(order.begin(), order.end(), [&](int a, int b) { return angles[size_t(a)] < angles[size_t(b)]; });
    for (int k = 0; k < 4; ++k) {
        double d = angles[size_t(order[size_t((k+1)%4)])] - angles[size_t(order[size_t(k)])];
        if (k == 3) d += 2 * std::acos(-1.0);
        if (d <= angular_eps) return 2;
    }
    bool alternate = true;
    for (int k = 0; k < 4; ++k) alternate &= sheet[size_t(order[size_t(k)])] != sheet[size_t(order[size_t((k+1)%4)])];
    return alternate ? 1 : 0;
}

}  // namespace p3dtrellis::contact
