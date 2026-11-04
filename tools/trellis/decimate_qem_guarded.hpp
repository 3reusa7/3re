#pragma once
// Topology-preserving QEM simplification with an exact contact guard.
// Fork of the TRELLIS engine's CPU decimate_qem (trellis.cpp, MIT; itself a port
// of CuMesh's QEM). With the guard off it reproduces the engine's
// TRELLIS_QEM_PRESERVE_TOPOLOGY path bit for bit; see tools/trellis/README.md.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace p3dtrellis {

struct GuardedQemOptions {
    // Reject every collapse whose new triangles would add a crossing, an
    // edge-face contact, a coplanar overlap, a crossing sheet star or any other
    // strict-tolerance contact not explained by shared vertices/edges
    // (qem_contact_guard.hpp). Off = engine behaviour.
    bool contact_guard = true;
    // Guarded mode only: the threshold ladder stops here instead of rising to
    // 1e12. Refused collapses make the engine's ladder escalate until it accepts
    // high-error collapses that erase thin features; 1e-5 is the highest final
    // threshold Dev2 needed on any of the 20 saved runs. When the face budget
    // cannot be met below it, the extra faces stay (budget_met = false).
    float max_threshold = 1e-5f;
    // Asset agent only: accept new point/edge contacts (audit REVIEW), still refuse every UNSAFE
    // contact (qem_contact_guard.hpp, Tolerances::allow_touching). Off = shipped behaviour.
    bool allow_touching = false;
    // Optional observer after every round (tests and diagnostics).
    void (*after_round)(void* context, int round, const std::vector<float>& verts,
                        const std::vector<int32_t>& faces) = nullptr;
    void* context = nullptr;
};

struct GuardedQemReport {
    size_t input_faces = 0, output_faces = 0, rounds = 0;
    size_t candidates = 0;           // collapses that won their neighbourhood
    size_t applied = 0;
    size_t endpoint_placements = 0;  // refused at the midpoint, applied keeping one endpoint
    size_t rejected_link = 0;        // exact-duplicate guard (engine rule)
    size_t rejected_degenerate = 0;  // a new triangle would have zero area
    size_t rejected_contact = 0;     // a new triangle would touch/cross the surface
    size_t rejected_batch = 0;       // conflicts with another collapse of the same round
    size_t rejected_star = 0;        // separated sheets would cross along a shared edge
    size_t rejected_coincident = 0;  // the new point would coincide with another vertex
    double final_threshold = 0;
    bool budget_met = false;
    double guard_seconds = 0;
};

GuardedQemReport decimate_qem_guarded(const std::vector<float>& verts, const std::vector<int32_t>& faces,
                                      int target_faces, std::vector<float>& out_verts,
                                      std::vector<int32_t>& out_faces, const GuardedQemOptions& options = {});

}  // namespace p3dtrellis
