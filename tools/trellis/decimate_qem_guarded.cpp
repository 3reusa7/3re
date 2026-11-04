// Topology-preserving QEM with an exact contact guard (see decimate_qem_guarded.hpp).
//
// Cost, collapse target, threshold ladder, independent-set selection, link
// test and exact-duplicate guard are the engine's TRELLIS_QEM_PRESERVE_TOPOLOGY
// path (trellis.cpp src/decimate_qem.cpp, Dev2 revision), with identical F32
// arithmetic. Only the collision test differs. The engine's opt-in guard
// compared triangles shrunk by 1e-7 towards their centroids against the
// pre-round mesh. Measured on saved TRELLIS meshes that misses three cases:
//  1. folds through the collapsed vertex (two of its new triangles touching
//     along a segment that starts at the shared vertex: an edge-face contact);
//  2. a face rotating through a separated sister sheet around a shared edge
//     position (a crossing sheet star; the faces only meet along the edge line);
//  3. new triangles of two collapses of the same round meeting each other.
// The guard below tests every new triangle with the audit's own predicates
// (qem_contact_guard.hpp) and rejects the collapse; it never deletes, welds or
// moves anything else. A rejected collapse is simply retried in a later round.
#include "decimate_qem_guarded.hpp"
#include "qem_contact_guard.hpp"
#include "tri_bvh.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace p3dtrellis {
namespace {

struct QEM {
    float e[10];
    void zero() { for (int i = 0; i < 10; ++i) e[i] = 0.f; }
    void add_plane(float a, float b, float c, float d) {
        e[0]+=a*a; e[1]+=a*b; e[2]+=a*c; e[3]+=a*d;
        e[4]+=b*b; e[5]+=b*c; e[6]+=b*d;
        e[7]+=c*c; e[8]+=c*d; e[9]+=d*d;
    }
    float evaluate(float x, float y, float z) const {
        return e[0]*x*x + 2*e[1]*x*y + 2*e[2]*x*z + 2*e[3]*x
             + e[4]*y*y + 2*e[5]*y*z + 2*e[6]*y
             + e[7]*z*z + 2*e[8]*z    + e[9];
    }
};

struct V3 { float x, y, z; };
inline V3 sub(const V3& a, const V3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline V3 cross(const V3& a, const V3& b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
inline float dot(const V3& a, const V3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float nrm2(const V3& a) { return dot(a, a); }
inline uint64_t pack_cost(int id, float c) { uint32_t b; std::memcpy(&b, &c, 4); return ((uint64_t)b << 32) | (uint32_t)id; }

using FaceKey = std::array<std::array<float,3>,3>;
inline FaceKey face_key(const std::vector<float>& v, const std::vector<int32_t>& f, size_t face) {
    FaceKey key;
    for (int k = 0; k < 3; ++k) { const auto i = size_t(f[3*face+k])*3; key[k] = {v[i], v[i+1], v[i+2]}; }
    std::sort(key.begin(), key.end()); return key;
}
struct ProposalFace { FaceKey key; int face; int owner; };
// Engine exact-duplicate guard: collapse owners introducing a NEW exact
// coincident triangle pair. Original coincident sheets are retained.
std::vector<int> duplicate_owners(std::vector<ProposalFace>& proposed, const std::vector<float>& v,
                                  const std::vector<int32_t>& f) {
    std::sort(proposed.begin(), proposed.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    std::vector<int> reject;
    for (size_t first = 0; first < proposed.size();) {
        size_t end = first + 1;
        while (end < proposed.size() && proposed[end].key == proposed[first].key) ++end;
        for (size_t a = first; a < end; ++a) for (size_t b = a + 1; b < end; ++b) {
            const auto& x = proposed[a]; const auto& y = proposed[b];
            if (x.owner < 0 && y.owner < 0) continue;
            if (face_key(v, f, size_t(x.face)) == face_key(v, f, size_t(y.face))) continue;
            if (x.owner >= 0) reject.push_back(x.owner);
            if (y.owner >= 0) reject.push_back(y.owner);
        }
        first = end;
    }
    std::sort(reject.begin(), reject.end()); reject.erase(std::unique(reject.begin(), reject.end()), reject.end());
    return reject;
}

using contact::P;
using contact::Tri;
using Ids = std::array<int32_t, 3>;
struct NewTriangle { Tri tri; Ids ids; };

// Position key for exact coincidence lookups.
struct PositionKey {
    std::array<float,3> p;
    bool operator==(const PositionKey& o) const { return p == o.p; }
};
struct PositionHash {
    size_t operator()(const PositionKey& k) const {
        uint32_t b[3]; std::memcpy(b, k.p.data(), 12);
        uint64_t h = 1469598103934665603ull;
        for (uint32_t x : b) { h ^= x; h *= 1099511628211ull; }
        return size_t(h);
    }
};

struct Guard {
    const std::vector<float>& verts;
    const std::vector<int32_t>& faces;
    const std::vector<int>& off;
    const std::vector<int>& v2f;
    contact::Tolerances eps;
    trellis::TriBvh tree;
    std::vector<int32_t> by_position;  // vertex ids sorted by exact position
    std::vector<uint8_t> twin;         // another vertex has exactly the same position

    Guard(const std::vector<float>& v, const std::vector<int32_t>& f, const std::vector<int>& o,
          const std::vector<int>& vf, contact::Tolerances t)
        : verts(v), faces(f), off(o), v2f(vf), eps(t),
          tree(trellis::TriBvh::build(v.data(), int64_t(v.size()/3), f.data(), int64_t(f.size()/3))) {
        struct Keyed { std::array<float,3> p; int32_t id; };
        std::vector<Keyed> keyed(v.size()/3);
        for (size_t i = 0; i < keyed.size(); ++i) keyed[i] = {at(int32_t(i)), int32_t(i)};
        std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) { return a.p < b.p || (a.p == b.p && a.id < b.id); });
        by_position.resize(keyed.size());
        for (size_t i = 0; i < keyed.size(); ++i) by_position[i] = keyed[i].id;
        twin.assign(by_position.size(), 0);
        for (size_t i = 1; i < by_position.size(); ++i)
            if (at(by_position[i]) == at(by_position[i-1])) twin[size_t(by_position[i])] = twin[size_t(by_position[i-1])] = 1;
    }
    std::array<float,3> at(int32_t i) const { return {verts[3*size_t(i)], verts[3*size_t(i)+1], verts[3*size_t(i)+2]}; }
    static P point(const std::array<float,3>& p) { return {p[0], p[1], p[2]}; }
    // Vertex ids at exactly position p in the pre-round mesh.
    std::pair<const int32_t*, const int32_t*> at_position(const std::array<float,3>& p) const {
        const auto lo = std::lower_bound(by_position.begin(), by_position.end(), p,
            [&](int32_t i, const std::array<float,3>& q) { return at(i) < q; });
        auto hi = lo;
        while (hi != by_position.end() && at(*hi) == p) ++hi;
        return {by_position.data() + (lo - by_position.begin()), by_position.data() + (hi - by_position.begin())};
    }

    // New triangles of collapsing e1 into e0 at `target` (pre-round geometry).
    void proposals(int e0, int e1, const std::array<float,3>& target, std::vector<NewTriangle>& out) const {
        out.clear();
        for (int endpoint : {e0, e1}) {
            for (int j = off[size_t(endpoint)]; j < off[size_t(endpoint)+1]; ++j) {
                const int f = v2f[size_t(j)];
                bool has0 = false, has1 = false;
                for (int k = 0; k < 3; ++k) { has0 |= faces[3*size_t(f)+k] == e0; has1 |= faces[3*size_t(f)+k] == e1; }
                if (has0 && has1) continue;            // removed by the collapse
                if (endpoint == e1 && has0) continue;  // already listed
                NewTriangle t;
                for (int k = 0; k < 3; ++k) {
                    int32_t id = faces[3*size_t(f)+k];
                    if (id == e1) id = e0;
                    t.ids[size_t(k)] = id;
                    t.tri[size_t(k)] = point(id == e0 ? target : at(id));
                }
                out.push_back(t);
            }
        }
    }
    void bounds(const Tri& t, float lo[3], float hi[3]) const {
        for (int k = 0; k < 3; ++k) {
            const double a = std::min({t[0][k], t[1][k], t[2][k]}) - eps.scale;
            const double b = std::max({t[0][k], t[1][k], t[2][k]}) + eps.scale;
            lo[k] = std::nextafter(float(a), -std::numeric_limits<float>::infinity());
            hi[k] = std::nextafter(float(b), std::numeric_limits<float>::infinity());
        }
    }

    // 0 = acceptable against the pre-round mesh, else the rejection reason.
    enum Verdict : uint8_t { Ok = 0, Degenerate = 1, Contact = 2 };
    Verdict check(int e0, int e1, const std::array<float,3>& target, std::vector<NewTriangle>& scratch) const {
        proposals(e0, e1, target, scratch);
        for (const auto& t : scratch) if (contact::degenerate(t.tri)) return Degenerate;
        for (size_t a = 0; a < scratch.size(); ++a)
            for (size_t b = a + 1; b < scratch.size(); ++b)
                if (!contact::acceptable(scratch[a].tri, scratch[a].ids, scratch[b].tri, scratch[b].ids, eps)) return Contact;
        struct Query { const Guard* guard; const NewTriangle* t; int e0, e1; bool hit; };
        for (const auto& t : scratch) {
            float lo[3], hi[3]; bounds(t.tri, lo, hi);
            Query q{this, &t, e0, e1, false};
            const bool complete = tree.visit_overlapping(lo, hi, [](void* pointer, int32_t face) -> bool {
                auto& q = *static_cast<Query*>(pointer);
                const auto& f = q.guard->faces;
                Ids ids{f[3*size_t(face)], f[3*size_t(face)+1], f[3*size_t(face)+2]};
                for (int32_t id : ids) if (id == q.e0 || id == q.e1) return true;  // replaced by proposals
                Tri other{point(q.guard->at(ids[0])), point(q.guard->at(ids[1])), point(q.guard->at(ids[2]))};
                if (contact::acceptable(q.t->tri, q.t->ids, other, ids, q.guard->eps)) return true;
                q.hit = true; return false;
            }, &q);
            if (q.hit || !complete) return Contact;
        }
        return Ok;
    }
};

// Same-round state: collapses already accepted in this round, applied
// tentatively so later collapses are checked against the geometry they create.
struct RoundState {
    const Guard& guard;
    std::vector<int32_t> merged_into, merged_from;  // e1 -> e0, e0 -> e1 of accepted collapses
    std::unordered_map<int32_t, std::array<float,3>> moved;
    std::unordered_set<int32_t> dead_faces;
    std::unordered_set<PositionKey, PositionHash> new_points;
    double cell = 1;
    std::unordered_map<uint64_t, std::vector<int>> grid;  // cell -> accepted collapse slots
    std::vector<std::vector<NewTriangle>> accepted;

    RoundState(const Guard& g, size_t vertices, double cell_size)
        : guard(g), merged_into(vertices, -1), merged_from(vertices, -1), cell(cell_size) {}

    std::array<float,3> position(int32_t id) const {
        const auto it = moved.find(id);
        return it == moved.end() ? guard.at(id) : it->second;
    }
    void cell_range(const std::vector<NewTriangle>& ts, std::array<int64_t,3>& lo, std::array<int64_t,3>& hi) const {
        lo.fill(std::numeric_limits<int64_t>::max()); hi.fill(std::numeric_limits<int64_t>::min());
        for (const auto& t : ts) {
            float a[3], b[3]; guard.bounds(t.tri, a, b);
            for (int k = 0; k < 3; ++k) {
                lo[size_t(k)] = std::min(lo[size_t(k)], int64_t(std::floor(a[k] / cell)));
                hi[size_t(k)] = std::max(hi[size_t(k)], int64_t(std::floor(b[k] / cell)));
            }
        }
    }
    static uint64_t key(int64_t x, int64_t y, int64_t z) {
        return (uint64_t(x & 0x1fffff) << 42) | (uint64_t(y & 0x1fffff) << 21) | uint64_t(z & 0x1fffff);
    }

    // Triangles whose boxes are apart by more than the strict tolerance cannot
    // meet (the exact test would return Empty).
    bool boxes_meet(const Tri& a, const Tri& b) const {
        for (int k = 0; k < 3; ++k) {
            const double alo = std::min({a[0][k], a[1][k], a[2][k]}), ahi = std::max({a[0][k], a[1][k], a[2][k]});
            const double blo = std::min({b[0][k], b[1][k], b[2][k]}), bhi = std::max({b[0][k], b[1][k], b[2][k]});
            if (alo > bhi + guard.eps.strict || blo > ahi + guard.eps.strict) return false;
        }
        return true;
    }
    bool batch_conflict(const std::vector<NewTriangle>& ts) const {
        std::array<int64_t,3> lo, hi; cell_range(ts, lo, hi);
        std::vector<int> seen;
        for (int64_t x = lo[0]; x <= hi[0]; ++x) for (int64_t y = lo[1]; y <= hi[1]; ++y) for (int64_t z = lo[2]; z <= hi[2]; ++z) {
            const auto it = grid.find(key(x, y, z));
            if (it == grid.end()) continue;
            for (int slot : it->second) {
                if (std::find(seen.begin(), seen.end(), slot) != seen.end()) continue;
                seen.push_back(slot);
                for (const auto& a : ts) for (const auto& b : accepted[size_t(slot)])
                    if (boxes_meet(a.tri, b.tri) && !contact::acceptable(a.tri, a.ids, b.tri, b.ids, guard.eps)) return true;
            }
        }
        return false;
    }

    // The new point must not land exactly on another vertex: that would create
    // a coincidence (and contacts) the input did not have.
    bool new_coincidence(int e0, int e1, const std::array<float,3>& target) const {
        if (new_points.count({target})) return true;
        const auto [first, last] = guard.at_position(target);
        for (auto it = first; it != last; ++it) {
            const int32_t id = *it;
            if (id == e0 || id == e1) continue;
            if (guard.at(id) == guard.at(e0) || guard.at(id) == guard.at(e1)) continue;  // existing coincidence
            return true;
        }
        return false;
    }

    // Current form of pre-round face f with this collapse (e0,e1,target) applied
    // on top of the accepted ones. False when the face no longer exists.
    bool current_face(int32_t f, int e0, int e1, const std::array<float,3>& target, Ids& ids, std::array<std::array<float,3>,3>& p) const {
        if (dead_faces.count(f)) return false;
        bool has0 = false, has1 = false;
        for (int k = 0; k < 3; ++k) {
            int32_t id = guard.faces[3*size_t(f)+k];
            has0 |= id == e0; has1 |= id == e1;
            if (id == e1) id = e0;
            if (merged_into[size_t(id)] >= 0) id = merged_into[size_t(id)];
            ids[size_t(k)] = id;
            p[size_t(k)] = id == e0 ? target : position(id);
        }
        return !(has0 && has1);
    }
    // Vertices currently at position q (coincident sheet copies), with this
    // collapse applied tentatively.
    void vertices_at(const std::array<float,3>& q, int e0, int e1, const std::array<float,3>& target, std::vector<int32_t>& out) const {
        out.clear();
        const auto [first, last] = guard.at_position(q);
        for (auto it = first; it != last; ++it) {
            const int32_t id = *it;
            if (id == e1 || merged_into[size_t(id)] >= 0) continue;
            const auto current = id == e0 ? target : position(id);
            if (current == q) out.push_back(id);
        }
        if (target == q && std::find(out.begin(), out.end(), e0) == out.end()) out.push_back(e0);
    }

    // Edge stars around the new triangles' edges where separated sheets share
    // the edge position. Reject when the sheets would cross.
    bool star_conflict(const std::vector<NewTriangle>& ts, int e0, int e1, const std::array<float,3>& target) const {
        std::vector<int32_t> at_u, at_w;
        // Accepted collapses never create a coincidence (new_coincidence), so
        // only pre-round twins, or e0 landing on an endpoint's twin position,
        // can be shared.
        const auto may_share = [&](int32_t id) {
            if (id != e0) return guard.twin[size_t(id)] != 0;
            return (target == guard.at(e0) && guard.twin[size_t(e0)]) || (target == guard.at(e1) && guard.twin[size_t(e1)]);
        };
        for (const auto& t : ts) for (int k = 0; k < 3; ++k) {
            const int32_t u = t.ids[size_t(k)], w = t.ids[size_t((k+1)%3)];
            // A sheet star needs a second indexed edge at the same positions:
            // copies at one end suffice (the other end may be one shared id).
            if (!may_share(u) && !may_share(w)) continue;
            const auto pu = u == e0 ? target : position(u), pw = w == e0 ? target : position(w);
            vertices_at(pu, e0, e1, target, at_u);
            vertices_at(pw, e0, e1, target, at_w);
            if (at_u.size() + at_w.size() < 3) continue;
            std::vector<int32_t> around;
            std::vector<P> apex;
            std::vector<int> sheet;
            std::vector<std::pair<int32_t,int32_t>> sheets;
            for (int32_t a : at_u) {
                for (int32_t endpoint : {a, a == e0 ? e1 : merged_from[size_t(a)]}) {
                    if (endpoint < 0) continue;
                    for (int j = guard.off[size_t(endpoint)]; j < guard.off[size_t(endpoint)+1]; ++j) {
                        const int32_t f = guard.v2f[size_t(j)];
                        if (std::find(around.begin(), around.end(), f) != around.end()) continue;
                        Ids ids; std::array<std::array<float,3>,3> p;
                        if (!current_face(f, e0, e1, target, ids, p)) continue;
                        int iu = -1, iw = -1;
                        for (int m = 0; m < 3; ++m) {
                            if (p[size_t(m)] == pu && iu < 0) iu = m;
                            else if (p[size_t(m)] == pw && iw < 0) iw = m;
                        }
                        if (iu < 0 || iw < 0) continue;
                        around.push_back(f);
                        const int apex_index = 3 - iu - iw;
                        apex.push_back(Guard::point(p[size_t(apex_index)]));
                        const auto pair = std::minmax(ids[size_t(iu)], ids[size_t(iw)]);
                        auto found = std::find(sheets.begin(), sheets.end(), std::pair<int32_t,int32_t>(pair.first, pair.second));
                        if (found == sheets.end()) { sheets.emplace_back(pair.first, pair.second); found = sheets.end() - 1; }
                        sheet.push_back(int(found - sheets.begin()));
                    }
                }
            }
            if (around.size() <= 2) continue;
            const P p = Guard::point(pu), q = Guard::point(pw);
            if (contact::star_class(p, q, apex, sheet, guard.eps.strict) != 0 ||
                contact::star_class(p, q, apex, sheet, guard.eps.scale) != 0) return true;
        }
        return false;
    }

    void accept(int e0, int e1, const std::array<float,3>& target, std::vector<NewTriangle>&& ts) {
        moved[e0] = target;
        merged_into[size_t(e1)] = e0; merged_from[size_t(e0)] = e1;
        for (int j = guard.off[size_t(e0)]; j < guard.off[size_t(e0)+1]; ++j) {
            const int32_t f = guard.v2f[size_t(j)];
            for (int k = 0; k < 3; ++k) if (guard.faces[3*size_t(f)+k] == e1) dead_faces.insert(f);
        }
        new_points.insert({target});
        std::array<int64_t,3> lo, hi; cell_range(ts, lo, hi);
        const int slot = int(accepted.size());
        accepted.push_back(std::move(ts));
        for (int64_t x = lo[0]; x <= hi[0]; ++x) for (int64_t y = lo[1]; y <= hi[1]; ++y) for (int64_t z = lo[2]; z <= hi[2]; ++z)
            grid[key(x, y, z)].push_back(slot);
    }
};

// One simplify round; engine semantics with preserve_topology on.
void simplify_round(std::vector<float>& verts, int& V, std::vector<int32_t>& faces, int& F,
                    float lam_len, float lam_skinny, float thresh, const GuardedQemOptions& options,
                    contact::Tolerances eps, GuardedQemReport& report) {
    auto vat = [&](int i) -> V3 { return {verts[3*size_t(i)], verts[3*size_t(i)+1], verts[3*size_t(i)+2]}; };

    std::vector<int> off(size_t(V) + 1, 0);
    for (int f = 0; f < F; ++f) for (int k = 0; k < 3; ++k) off[size_t(faces[3*size_t(f)+k]) + 1]++;
    for (int i = 0; i < V; ++i) off[size_t(i)+1] += off[size_t(i)];
    std::vector<int> v2f(size_t(off[size_t(V)]));
    { std::vector<int> cur(off.begin(), off.end() - 1);
      for (int f = 0; f < F; ++f) for (int k = 0; k < 3; ++k) { int v = faces[3*size_t(f)+k]; v2f[size_t(cur[size_t(v)]++)] = f; } }

    std::unordered_map<uint64_t,int> ecount;
    ecount.reserve((size_t)F * 2);
    auto ekey = [](int a, int b) -> uint64_t { if (a > b) { int t=a; a=b; b=t; } return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; };
    for (int f = 0; f < F; ++f) {
        int a = faces[3*size_t(f)], b = faces[3*size_t(f)+1], c = faces[3*size_t(f)+2];
        ecount[ekey(a,b)]++; ecount[ekey(b,c)]++; ecount[ekey(c,a)]++;
    }
    std::vector<uint8_t> boundary((size_t)V, 0);
    std::vector<uint64_t> edges; edges.reserve(ecount.size());
    for (auto& kv : ecount) {
        int a = (int)(kv.first >> 32), b = (int)(kv.first & 0xffffffffu);
        if (kv.second == 1) { boundary[size_t(a)] = boundary[size_t(b)] = 1; }
        edges.push_back(kv.first);
    }
    const int E = (int)edges.size();

    auto valid_link = [&](int a, int b, uint64_t key, const V3& replacement) {
        if (ecount.at(key) != 2) return false;
        if (boundary[size_t(a)] && boundary[size_t(b)]) return false;
        auto neighbors = [&](int vertex) {
            std::vector<int> result;
            result.reserve((size_t)(off[size_t(vertex)+1] - off[size_t(vertex)]) * 2);
            for (int j = off[size_t(vertex)]; j < off[size_t(vertex)+1]; ++j) {
                const int f = v2f[size_t(j)];
                for (int k = 0; k < 3; ++k) {
                    const int other = faces[3*size_t(f)+k];
                    if (other != vertex) result.push_back(other);
                }
            }
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        };
        auto left = neighbors(a), right = neighbors(b);
        size_t i = 0, j = 0;
        int common = 0;
        while (i < left.size() && j < right.size()) {
            if (left[i] == right[j]) { ++common; ++i; ++j; }
            else if (left[i] < right[j]) ++i;
            else ++j;
            if (common > 2) return false;
        }
        if (common != 2) return false;
        std::vector<std::array<int32_t, 3>> surviving;
        surviving.reserve((size_t)(off[size_t(a)+1] - off[size_t(a)] + off[size_t(b)+1] - off[size_t(b)]));
        for (int endpoint : {a, b}) {
            for (int j2 = off[size_t(endpoint)]; j2 < off[size_t(endpoint)+1]; ++j2) {
                const int f = v2f[size_t(j2)];
                std::array<int32_t, 3> triangle{faces[3*size_t(f)], faces[3*size_t(f)+1], faces[3*size_t(f)+2]};
                const bool has_a = triangle[0] == a || triangle[1] == a || triangle[2] == a;
                const bool has_b = triangle[0] == b || triangle[1] == b || triangle[2] == b;
                if (has_a && has_b) continue;
                for (int32_t& v : triangle) if (v == b) v = a;
                std::sort(triangle.begin(), triangle.end());
                surviving.push_back(triangle);
            }
        }
        std::sort(surviving.begin(), surviving.end());
        if (std::adjacent_find(surviving.begin(), surviving.end()) != surviving.end()) return false;
        std::vector<std::array<std::array<float,3>,3>> geometric;
        geometric.reserve(surviving.size());
        for (const auto& triangle : surviving) {
            std::array<std::array<float,3>,3> points;
            for (int k = 0; k < 3; ++k) {
                const V3 p = triangle[size_t(k)] == a ? replacement : vat(triangle[size_t(k)]);
                points[size_t(k)] = {p.x,p.y,p.z};
            }
            std::sort(points.begin(), points.end());
            geometric.push_back(points);
        }
        std::sort(geometric.begin(), geometric.end());
        return std::adjacent_find(geometric.begin(), geometric.end()) == geometric.end();
    };

    std::vector<QEM> qem((size_t)V);
    for (auto& q : qem) q.zero();
    for (int f = 0; f < F; ++f) {
        const int ia = faces[3*size_t(f)], ib = faces[3*size_t(f)+1], ic = faces[3*size_t(f)+2];
        V3 a = vat(ia), b = vat(ib), c = vat(ic);
        V3 n = cross(sub(b,a), sub(c,a));
        float ln = std::sqrt(nrm2(n));
        if (ln > 1e-20f) { n.x/=ln; n.y/=ln; n.z/=ln; }
        const float d = -dot(n, a);
        qem[size_t(ia)].add_plane(n.x, n.y, n.z, d);
        qem[size_t(ib)].add_plane(n.x, n.y, n.z, d);
        qem[size_t(ic)].add_plane(n.x, n.y, n.z, d);
    }

    auto process = [&](int keep, int other, const V3& vn, float& skinny, int& ntri) -> bool {
        for (int j = off[size_t(keep)]; j < off[size_t(keep)+1]; ++j) {
            int f = v2f[size_t(j)]; int x = faces[3*size_t(f)], y = faces[3*size_t(f)+1], z = faces[3*size_t(f)+2];
            if (x == other || y == other || z == other) continue;
            V3 a = vat(x), b = vat(y), c = vat(z);
            V3 na = (x==keep)?vn:a, nb = (y==keep)?vn:b, nc = (z==keep)?vn:c;
            V3 on = cross(sub(b,a), sub(c,a));
            V3 ne1 = sub(nb,na), ne2 = sub(nc,na), nn = cross(ne1, ne2);
            if (dot(on, nn) < 0.f) return false;
            float narea = 0.5f * std::sqrt(nrm2(nn));
            float denom = nrm2(sub(nc,nb)) + nrm2(ne1) + nrm2(ne2);
            if (denom < 1e-12f) denom = 1e-12f;
            float sm = 4.0f * 1.7320508f * narea / denom;
            skinny += 1.0f - std::min(std::max(sm, 0.0f), 1.0f);
            ntri++;
        }
        return true;
    };

    std::vector<float> cost((size_t)E);
    std::vector<V3> vnew((size_t)E);
    // Each edge's cost is a pure function of the pre-round mesh: evaluating
    // them in parallel gives the same values as the engine's serial loop.
    #pragma omp parallel for schedule(dynamic, 4096)
    for (int t = 0; t < E; ++t) {
        int e0 = (int)(edges[size_t(t)] >> 32), e1 = (int)(edges[size_t(t)] & 0xffffffffu);
        V3 v0 = vat(e0), v1 = vat(e1);
        float w0 = 0.5f;
        if (boundary[size_t(e0)] && !boundary[size_t(e1)]) w0 = 1.0f;
        else if (!boundary[size_t(e0)] && boundary[size_t(e1)]) w0 = 0.0f;
        V3 v = { v0.x*w0 + v1.x*(1-w0), v0.y*w0 + v1.y*(1-w0), v0.z*w0 + v1.z*(1-w0) };
        vnew[size_t(t)] = v;
        float el2 = nrm2(sub(v1, v0));
        const auto& q0 = qem[size_t(e0)].e; const auto& q1 = qem[size_t(e1)].e;
        float c = (QEM{{q0[0]+q1[0], q0[1]+q1[1], q0[2]+q1[2], q0[3]+q1[3], q0[4]+q1[4],
                        q0[5]+q1[5], q0[6]+q1[6], q0[7]+q1[7], q0[8]+q1[8], q0[9]+q1[9]}}).evaluate(v.x, v.y, v.z);
        c += lam_len * el2;
        float skinny = 0.f; int ntri = 0; bool ok = true;
        ok = process(e0, e1, v, skinny, ntri) && process(e1, e0, v, skinny, ntri);
        if (!ok) { cost[size_t(t)] = std::numeric_limits<float>::infinity(); continue; }
        if (ntri > 0) skinny /= ntri;
        c += lam_skinny * skinny * el2;
        if (c <= thresh && !valid_link(e0, e1, edges[size_t(t)], v)) {
            cost[size_t(t)] = std::numeric_limits<float>::infinity();
            continue;
        }
        cost[size_t(t)] = c;
    }

    std::vector<uint64_t> prop((size_t)F, UINT64_MAX);
    for (int t = 0; t < E; ++t) {
        uint64_t p = pack_cost(t, cost[size_t(t)]);
        int e0 = (int)(edges[size_t(t)] >> 32), e1 = (int)(edges[size_t(t)] & 0xffffffffu);
        for (int j = off[size_t(e0)]; j < off[size_t(e0)+1]; ++j) { int f = v2f[size_t(j)]; if (p < prop[size_t(f)]) prop[size_t(f)] = p; }
        for (int j = off[size_t(e1)]; j < off[size_t(e1)+1]; ++j) { int f = v2f[size_t(j)]; if (p < prop[size_t(f)]) prop[size_t(f)] = p; }
    }

    std::vector<uint8_t> vdead((size_t)V, 0), fdead((size_t)F, 0);
    std::vector<int> winners;
    for (int t = 0; t < E; ++t) {
        if (!(cost[size_t(t)] <= thresh)) continue;
        uint64_t p = pack_cost(t, cost[size_t(t)]);
        int e0 = (int)(edges[size_t(t)] >> 32), e1 = (int)(edges[size_t(t)] & 0xffffffffu);
        bool own = true;
        for (int j = off[size_t(e0)]; j < off[size_t(e0)+1] && own; ++j) if (prop[size_t(v2f[size_t(j)])] != p) own = false;
        for (int j = off[size_t(e1)]; j < off[size_t(e1)+1] && own; ++j) if (prop[size_t(v2f[size_t(j)])] != p) own = false;
        if (!own) continue;
        winners.push_back(t);
    }
    report.candidates += winners.size();

    std::vector<uint8_t> permitted(winners.size(), 1);
    // Placement of each winner: the engine's midpoint/boundary rule, or an
    // endpoint chosen by the guard below.
    std::vector<V3> chosen(winners.size());
    for (size_t i = 0; i < winners.size(); ++i) chosen[i] = vnew[size_t(winners[i])];
    auto target_of = [&](size_t i) { const V3 v = chosen[i]; return std::array<float,3>{v.x, v.y, v.z}; };
    auto endpoints = [&](int t) { return std::pair<int,int>((int)(edges[size_t(t)] >> 32), (int)(edges[size_t(t)] & 0xffffffffu)); };

    // Engine exact-duplicate guard, iterated to a fixed point. Rejects
    // collapses, never deletes surfaces.
    auto duplicate_pass = [&]() -> bool {
        bool changed = false;
        std::vector<int> owner((size_t)V, -1);
        for (size_t i = 0; i < winners.size(); ++i) {
            const auto edge = edges[size_t(winners[i])];
            owner[size_t(edge >> 32)] = int(i); owner[size_t(edge & 0xffffffffu)] = int(i);
        }
        for (;;) {
            std::vector<ProposalFace> proposed; proposed.reserve(size_t(F));
            for (int f = 0; f < F; ++f) {
                int collapse = -1, touched = 0;
                for (int k = 0; k < 3; ++k) { const int c = owner[size_t(faces[3*size_t(f)+k])]; if (c >= 0 && permitted[size_t(c)]) { collapse = c; ++touched; } }
                if (touched == 2) continue;
                FaceKey key;
                for (int k = 0; k < 3; ++k) {
                    const int vertex = faces[3*size_t(f)+k], c = owner[size_t(vertex)];
                    const V3 p = c >= 0 && permitted[size_t(c)] ? chosen[size_t(c)] : vat(vertex);
                    key[size_t(k)] = {p.x, p.y, p.z};
                }
                std::sort(key.begin(), key.end()); proposed.push_back({key, f, collapse});
            }
            const auto rejected = duplicate_owners(proposed, verts, faces);
            if (rejected.empty()) break;
            std::printf("  qem exact-duplicate guard: rejected collapses=%zu\n", rejected.size());
            for (const int c : rejected) permitted[size_t(c)] = 0;
            report.rejected_link += rejected.size();
            changed = true;
        }
        return changed;
    };

    if (options.contact_guard && !winners.empty()) {
        const auto start = std::chrono::steady_clock::now();
        const Guard guard(verts, faces, off, v2f, eps);
        // Per-collapse checks against the pre-round mesh are independent.
        std::vector<uint8_t> verdict(winners.size(), 0);
        double extent_sum = 0;
        size_t endpoint_placements = 0;
        // Cost of collapsing e1 into e0 placed at p, with the engine's flip
        // test; infinity when a face would flip.
        auto cost_at = [&](int e0, int e1, const V3& p) {
            float skinny = 0.f; int ntri = 0;
            if (!(process(e0, e1, p, skinny, ntri) && process(e1, e0, p, skinny, ntri))) return std::numeric_limits<float>::infinity();
            if (ntri > 0) skinny /= ntri;
            const float el2 = nrm2(sub(vat(e1), vat(e0)));
            const auto& q0 = qem[size_t(e0)].e; const auto& q1 = qem[size_t(e1)].e;
            return (QEM{{q0[0]+q1[0], q0[1]+q1[1], q0[2]+q1[2], q0[3]+q1[3], q0[4]+q1[4],
                         q0[5]+q1[5], q0[6]+q1[6], q0[7]+q1[7], q0[8]+q1[8], q0[9]+q1[9]}}).evaluate(p.x, p.y, p.z)
                   + lam_len * el2 + lam_skinny * skinny * el2;
        };
        #pragma omp parallel
        {
            std::vector<NewTriangle> scratch;
            #pragma omp for schedule(dynamic, 256) reduction(+:extent_sum, endpoint_placements)
            for (long long i = 0; i < (long long)winners.size(); ++i) {
                const auto [e0, e1] = endpoints(winners[size_t(i)]);
                verdict[size_t(i)] = guard.check(e0, e1, target_of(size_t(i)), scratch);
                // Refused at the midpoint: try keeping one endpoint (a
                // half-edge collapse). The new fan then uses existing points
                // only. Same flip test, threshold, link test and guard; never
                // on boundary vertices, whose placement rule is the engine's.
                if (verdict[size_t(i)] != Guard::Ok && !boundary[size_t(e0)] && !boundary[size_t(e1)]) {
                    for (const V3 alt : {vat(e0), vat(e1)}) {
                        if (!(cost_at(e0, e1, alt) <= thresh) || !valid_link(e0, e1, edges[size_t(winners[size_t(i)])], alt)) continue;
                        if (guard.check(e0, e1, {alt.x, alt.y, alt.z}, scratch) != Guard::Ok) continue;
                        chosen[size_t(i)] = alt; verdict[size_t(i)] = Guard::Ok; ++endpoint_placements; break;
                    }
                }
                float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
                for (const auto& t : scratch) { float a[3], b[3]; guard.bounds(t.tri, a, b);
                    for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], a[k]); hi[k] = std::max(hi[k], b[k]); } }
                extent_sum += std::max({hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2], 0.f});
            }
        }
        for (size_t i = 0; i < winners.size(); ++i) {
            if (verdict[i] == Guard::Degenerate) { permitted[i] = 0; ++report.rejected_degenerate; }
            else if (verdict[i] == Guard::Contact) { permitted[i] = 0; ++report.rejected_contact; }
        }
        report.endpoint_placements += endpoint_placements;
        const double cell = std::max(extent_sum / double(winners.size()), 1e-12);
        // Sequential pass in winner order: each accepted collapse is applied
        // tentatively, so later ones are checked against what it creates.
        // Then the engine's duplicate guard on the surviving set. A collapse it
        // withdraws restores pre-round faces (already checked in parallel) but
        // may change what later collapses saw: redo the pass in that case.
        for (;;) {
            RoundState state(guard, size_t(V), cell);
            std::vector<NewTriangle> ts;
            for (size_t i = 0; i < winners.size(); ++i) {
                if (!permitted[i]) continue;
                const auto [e0, e1] = endpoints(winners[i]);
                const auto target = target_of(i);
                guard.proposals(e0, e1, target, ts);
                size_t* reason = nullptr;
                if (state.new_coincidence(e0, e1, target)) reason = &report.rejected_coincident;
                else if (state.batch_conflict(ts)) reason = &report.rejected_batch;
                else if (state.star_conflict(ts, e0, e1, target)) reason = &report.rejected_star;
                if (reason) { permitted[i] = 0; ++*reason; continue; }
                state.accept(e0, e1, target, std::move(ts));
                ts = {};
            }
            if (!duplicate_pass()) break;
        }
        report.guard_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    } else if (!winners.empty()) {
        duplicate_pass();
    }

    for (size_t i = 0; i < winners.size(); ++i) {
        if (!permitted[i]) continue;
        ++report.applied;
        const int t = winners[i];
        const int e0 = (int)(edges[size_t(t)] >> 32), e1 = (int)(edges[size_t(t)] & 0xffffffffu);
        verts[3*size_t(e0)] = chosen[i].x; verts[3*size_t(e0)+1] = chosen[i].y; verts[3*size_t(e0)+2] = chosen[i].z;
        vdead[size_t(e1)] = 1;
        for (int j = off[size_t(e0)]; j < off[size_t(e0)+1]; ++j) { int f = v2f[size_t(j)]; int* ff = &faces[3*size_t(f)]; if (ff[0]==e1||ff[1]==e1||ff[2]==e1) fdead[size_t(f)] = 1; }
        for (int j = off[size_t(e1)]; j < off[size_t(e1)+1]; ++j) { int f = v2f[size_t(j)]; int* ff = &faces[3*size_t(f)]; for (int k = 0; k < 3; ++k) if (ff[k]==e1) ff[k] = e0; }
    }

    std::vector<int> vmap((size_t)V, -1); int nV = 0;
    for (int i = 0; i < V; ++i) if (!vdead[size_t(i)]) vmap[size_t(i)] = nV++;
    std::vector<float> nv((size_t)nV * 3);
    for (int i = 0; i < V; ++i) if (vmap[size_t(i)] >= 0) { nv[3*size_t(vmap[size_t(i)])] = verts[3*size_t(i)]; nv[3*size_t(vmap[size_t(i)])+1] = verts[3*size_t(i)+1]; nv[3*size_t(vmap[size_t(i)])+2] = verts[3*size_t(i)+2]; }
    std::vector<int32_t> nf; nf.reserve(faces.size());
    for (int f = 0; f < F; ++f) {
        if (fdead[size_t(f)]) continue;
        int a = vmap[size_t(faces[3*size_t(f)])], b = vmap[size_t(faces[3*size_t(f)+1])], c = vmap[size_t(faces[3*size_t(f)+2])];
        if (a < 0 || b < 0 || c < 0 || a == b || b == c || a == c) continue;
        nf.push_back(a); nf.push_back(b); nf.push_back(c);
    }
    V = nV; F = (int)nf.size() / 3;
    verts.swap(nv); faces.swap(nf);
}

}  // namespace

GuardedQemReport decimate_qem_guarded(const std::vector<float>& in_verts, const std::vector<int32_t>& in_faces,
                                      int target_faces, std::vector<float>& ov, std::vector<int32_t>& of,
                                      const GuardedQemOptions& options) {
    if (in_verts.size() % 3 || in_faces.size() % 3) throw std::invalid_argument("guarded QEM: invalid storage");
    std::vector<float> verts = in_verts;
    std::vector<int32_t> faces = in_faces;
    int V = int(verts.size() / 3), F = int(faces.size() / 3);
    GuardedQemReport report;
    report.input_faces = size_t(F);
    // Audit tolerances from the input extent (the simplified mesh's extent can
    // only shrink, so these are at least as large as the audit's).
    double lo[3] = {1e300,1e300,1e300}, hi[3] = {-1e300,-1e300,-1e300}, max_abs = 0;
    for (size_t i = 0; i < verts.size(); ++i) {
        const double x = verts[i];
        lo[i%3] = std::min(lo[i%3], x); hi[i%3] = std::max(hi[i%3], x); max_abs = std::max(max_abs, std::abs(x));
    }
    const double diagonal = verts.empty() ? 0 : std::sqrt((hi[0]-lo[0])*(hi[0]-lo[0]) + (hi[1]-lo[1])*(hi[1]-lo[1]) + (hi[2]-lo[2])*(hi[2]-lo[2]));
    auto eps = contact::tolerances(diagonal, max_abs);
    eps.allow_touching = options.allow_touching;

    float thresh = 1e-8f;
    if (F > target_faces) {
        const float lam_len = 1e-2f, lam_skinny = 1e-3f;
        int prevF = F, stalls = 0;
        for (int round = 0; round < 400 && F > target_faces; ++round) {
            simplify_round(verts, V, faces, F, lam_len, lam_skinny, thresh, options, eps, report);
            ++report.rounds;
            if (options.after_round) options.after_round(options.context, round, verts, faces);
            if (F <= target_faces) break;
            int removed = prevF - F;
            if (removed <= 0) { if (++stalls >= 2) { thresh *= 10.0f; stalls = 0; } }
            else { stalls = 0; if ((float)removed / prevF < 1e-2f) thresh *= 10.0f; }
            prevF = F;
            if (thresh > 1e12f) break;
            // Allow float drift of the x10 ladder (1e-8f * 1000 > 1e-5f).
            if (options.contact_guard && thresh > options.max_threshold * 1.5f) { thresh /= 10.0f; break; }
        }
    }
    std::vector<int> used((size_t)V, -1); int nV = 0;
    for (int f = 0; f < F; ++f) for (int k = 0; k < 3; ++k) { int v = faces[3*size_t(f)+k]; if (used[size_t(v)] < 0) used[size_t(v)] = nV++; }
    ov.assign((size_t)nV * 3, 0.f);
    for (int i = 0; i < V; ++i) if (used[size_t(i)] >= 0) { ov[3*size_t(used[size_t(i)])] = verts[3*size_t(i)]; ov[3*size_t(used[size_t(i)])+1] = verts[3*size_t(i)+1]; ov[3*size_t(used[size_t(i)])+2] = verts[3*size_t(i)+2]; }
    of.resize((size_t)F * 3);
    for (int f = 0; f < F; ++f) for (int k = 0; k < 3; ++k) of[3*size_t(f)+k] = used[size_t(faces[3*size_t(f)+k])];
    report.output_faces = size_t(F);
    report.final_threshold = thresh;
    report.budget_met = F <= target_faces;
    std::printf("  decimate_qem_guarded(target=%d): V %zu->%d, F %zu->%d (thresh=%.1e) guard=%s\n", target_faces,
                in_verts.size()/3, nV, report.input_faces, F, (double)thresh, options.contact_guard ? "exact" : "off");
    std::fflush(stdout);
    return report;
}

}  // namespace p3dtrellis
