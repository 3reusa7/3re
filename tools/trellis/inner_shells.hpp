#pragma once
// Enclosed inner offset shells after sheet separation.
//
// The TRELLIS remesh extracts the boundary of an eps-band around the generated
// surface (f = UDF - eps, spec 27 section 4). Wherever the object is thicker
// than about two voxels, that boundary has a second, inner layer: a closed
// shell inside the solid whose normals face the solid's interior (negative
// signed volume). Pinches attach some of them to the outer surface until sheet
// separation detaches them; QEM then spends faces on surfaces that cannot be
// seen from any viewpoint, and the asset reports extra components.
//
// A component is removed only when ALL of these hold:
//  - it is closed (no boundary edge) with negative signed volume;
//  - every sampled vertex that is not a pinch (no other vertex shares its
//    position) is inside a closed positive-volume component, by majority of
//    three ray-parity tests.
// Anything else is kept and counted. Positive shells, open sheets and
// components that cannot be verified are never touched; no position moves.
#include "tri_bvh.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace p3dtrellis {

struct InnerShellReport {
    size_t components = 0, removed_shells = 0, removed_faces = 0;
    size_t kept_negative_closed = 0;  // negative closed shells not verified as enclosed
    double removed_area = 0, removed_volume = 0;
};

namespace inner_detail {
using D3 = std::array<double, 3>;
inline D3 at(const std::vector<float>& v, int32_t i) { return {v[3*size_t(i)], v[3*size_t(i)+1], v[3*size_t(i)+2]}; }
inline D3 sub(D3 a, D3 b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
inline D3 cross(D3 a, D3 b) { return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]}; }
inline double dot(D3 a, D3 b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

// Number of triangles of `faces` crossed by the ray q + t d, t > 0.
inline int crossings(const trellis::TriBvh& tree, const std::vector<float>& v, const std::vector<int32_t>& faces,
                     D3 q, D3 d, double length) {
    struct Query { const std::vector<float>* v; const std::vector<int32_t>* f; D3 q, d; int hits; };
    Query query{&v, &faces, q, d, 0};
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        const double a = q[size_t(k)], b = q[size_t(k)] + d[size_t(k)] * length;
        lo[k] = float(std::min(a, b)) - 1e-6f; hi[k] = float(std::max(a, b)) + 1e-6f;
    }
    tree.visit_overlapping(lo, hi, [](void* pointer, int32_t face) -> bool {
        auto& s = *static_cast<Query*>(pointer);
        const D3 a = at(*s.v, (*s.f)[3*size_t(face)]), b = at(*s.v, (*s.f)[3*size_t(face)+1]), c = at(*s.v, (*s.f)[3*size_t(face)+2]);
        const D3 e1 = sub(b, a), e2 = sub(c, a), p = cross(s.d, e2);
        const double det = dot(e1, p);
        if (det == 0) return true;
        const D3 t = sub(s.q, a);
        const double u = dot(t, p) / det;
        if (u < 0 || u > 1) return true;
        const D3 qv = cross(t, e1);
        const double w = dot(s.d, qv) / det;
        if (w < 0 || u + w > 1) return true;
        if (dot(e2, qv) / det > 0) ++s.hits;
        return true;
    }, &query);
    return query.hits;
}
}  // namespace inner_detail

inline InnerShellReport remove_enclosed_inner_shells(std::vector<float>& verts, std::vector<int32_t>& faces) {
    using namespace inner_detail;
    if (verts.size() % 3 || faces.size() % 3) throw std::runtime_error("inner shells: invalid storage");
    const size_t V = verts.size() / 3, F = faces.size() / 3;
    InnerShellReport report;
    if (!F) return report;
    for (int32_t i : faces) if (i < 0 || size_t(i) >= V) throw std::runtime_error("inner shells: invalid index");

    std::vector<int32_t> parent(V);
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&](int32_t x) { while (parent[size_t(x)] != x) { parent[size_t(x)] = parent[size_t(parent[size_t(x)])]; x = parent[size_t(x)]; } return x; };
    for (size_t f = 0; f < F; ++f) for (int k = 1; k < 3; ++k) {
        const int32_t a = find(faces[3*f]), b = find(faces[3*f+size_t(k)]);
        if (a != b) parent[size_t(b)] = a;
    }
    std::unordered_map<int32_t, size_t> id;
    std::vector<size_t> face_component(F);
    for (size_t f = 0; f < F; ++f) face_component[f] = id.emplace(find(faces[3*f]), id.size()).first->second;
    const size_t C = id.size();
    report.components = C;
    std::vector<double> volume(C, 0), area(C, 0);
    std::vector<size_t> boundary(C, 0), count(C, 0);
    for (size_t f = 0; f < F; ++f) {
        const D3 a = at(verts, faces[3*f]), b = at(verts, faces[3*f+1]), c = at(verts, faces[3*f+2]);
        volume[face_component[f]] += dot(a, cross(b, c)) / 6;
        const D3 n = cross(sub(b, a), sub(c, a));
        area[face_component[f]] += std::sqrt(dot(n, n)) / 2;
        ++count[face_component[f]];
    }
    {
        std::vector<std::array<int32_t, 3>> edges;  // lo, hi, face
        edges.reserve(faces.size());
        for (size_t f = 0; f < F; ++f) for (int k = 0; k < 3; ++k) {
            const int32_t a = faces[3*f+size_t(k)], b = faces[3*f+size_t((k+1)%3)];
            edges.push_back({std::min(a, b), std::max(a, b), int32_t(f)});
        }
        std::sort(edges.begin(), edges.end());
        for (size_t i = 0; i < edges.size();) {
            size_t j = i + 1;
            while (j < edges.size() && edges[j][0] == edges[i][0] && edges[j][1] == edges[i][1]) ++j;
            if (j - i == 1) ++boundary[face_component[size_t(edges[i][2])]];
            i = j;
        }
    }
    std::vector<uint8_t> candidate(C, 0), solid(C, 0);
    bool any = false;
    for (size_t c = 0; c < C; ++c) {
        if (boundary[c]) continue;
        if (volume[c] < 0) { candidate[c] = 1; any = true; }
        else if (volume[c] > 0) solid[c] = 1;
    }
    if (!any) return report;

    std::vector<int32_t> solid_faces;
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (size_t f = 0; f < F; ++f) if (solid[face_component[f]]) for (int k = 0; k < 3; ++k) solid_faces.push_back(faces[3*f+size_t(k)]);
    for (size_t i = 0; i < verts.size(); ++i) { lo[i%3] = std::min(lo[i%3], double(verts[i])); hi[i%3] = std::max(hi[i%3], double(verts[i])); }
    const double length = 2 * std::sqrt((hi[0]-lo[0])*(hi[0]-lo[0]) + (hi[1]-lo[1])*(hi[1]-lo[1]) + (hi[2]-lo[2])*(hi[2]-lo[2])) + 1;
    if (solid_faces.empty()) { for (size_t c = 0; c < C; ++c) report.kept_negative_closed += candidate[c]; return report; }
    const auto tree = trellis::TriBvh::build(verts.data(), int64_t(V), solid_faces.data(), int64_t(solid_faces.size() / 3));

    // Pinch vertices (shared positions) can lie exactly on the outer surface.
    std::vector<int32_t> order(V);
    std::iota(order.begin(), order.end(), 0);
    const auto key = [&](int32_t i) { return std::array<float, 3>{verts[3*size_t(i)], verts[3*size_t(i)+1], verts[3*size_t(i)+2]}; };
    std::sort(order.begin(), order.end(), [&](int32_t a, int32_t b) { return key(a) < key(b); });
    std::vector<uint8_t> pinch(V, 0);
    for (size_t i = 1; i < V; ++i) if (key(order[i]) == key(order[i-1])) pinch[size_t(order[i])] = pinch[size_t(order[i-1])] = 1;

    // Irrational, non-axis directions: grazing edges and vertices are avoided.
    const D3 directions[3] = {{1, 0.0007071067811865476, 0.0005773502691896258},
                              {0.0004472135954999579, 1, 0.0003162277660168379},
                              {0.0002645751311064591, 0.0005567764362830022, 1}};
    std::vector<std::vector<int32_t>> samples(C);
    for (size_t f = 0; f < F; ++f) {
        const size_t c = face_component[f];
        if (!candidate[c]) continue;
        for (int k = 0; k < 3; ++k) {
            const int32_t v = faces[3*f+size_t(k)];
            if (pinch[size_t(v)] || samples[c].size() >= 8) continue;
            if (std::find(samples[c].begin(), samples[c].end(), v) == samples[c].end()) samples[c].push_back(v);
        }
    }
    std::vector<uint8_t> remove(C, 0);
    for (size_t c = 0; c < C; ++c) {
        if (!candidate[c]) continue;
        bool enclosed = !samples[c].empty();
        for (int32_t v : samples[c]) {
            int inside = 0;
            for (const D3& d : directions) inside += crossings(tree, verts, solid_faces, at(verts, v), d, length) % 2;
            enclosed &= inside >= 2;
        }
        if (!enclosed) { ++report.kept_negative_closed; continue; }
        remove[c] = 1;
        ++report.removed_shells; report.removed_faces += count[c];
        report.removed_area += area[c]; report.removed_volume += volume[c];
    }
    if (!report.removed_shells) return report;

    std::vector<int32_t> map(V, -1), kept;
    std::vector<float> positions;
    kept.reserve(faces.size());
    for (size_t f = 0; f < F; ++f) {
        if (remove[face_component[f]]) continue;
        for (int k = 0; k < 3; ++k) {
            const int32_t v = faces[3*f+size_t(k)];
            if (map[size_t(v)] < 0) {
                map[size_t(v)] = int32_t(positions.size() / 3);
                positions.insert(positions.end(), verts.begin() + 3*std::ptrdiff_t(v), verts.begin() + 3*std::ptrdiff_t(v) + 3);
            }
            kept.push_back(map[size_t(v)]);
        }
    }
    verts.swap(positions); faces.swap(kept);
    return report;
}

}  // namespace p3dtrellis
