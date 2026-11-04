// Offline TRELLIS geometry cleanup with intersection-safe simplification.
// Same command line and outputs as post_geometry_preview4.cpp; no model,
// sampler or GPU work. Differences, each traced to a measured defect source in
// saved runs (tools/trellis/README.md, "Geometry safety"):
//  - zero-length DC edges are collapsed (ids merged, no position changes)
//    instead of deleting their zero-area faces, which opened zero-width slits;
//  - closed inner offset shells sealed inside the solid are removed after sheet
//    separation, only when verified enclosed (inner_shells.hpp);
//  - QEM rejects any collapse that would add a crossing, edge-face contact,
//    coplanar overlap, crossing sheet star or other new contact
//    (decimate_qem_guarded.cpp). The DC surface itself has none of them.
// Remesh, welding, raw hole filling and the initial component filter are the
// unchanged engine stages linked from libtrellis_core.a.
#include "dual_grid.h"
#include "tri_bvh.h"
#include "remesh_dc.h"
#include "uv_bake.h"
#include "mesh_glb.h"
#include "mesh_topology.h"
#include "exact_degenerate.hpp"
#include "inner_shells.hpp"
#include "decimate_qem_guarded.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

// Diagnostic-only native checkpoints; never rewrite an input or prior output.
static void checkpoint(const std::vector<float>& verts, const std::vector<int32_t>& faces, const char* stage) {
    const char* directory = std::getenv("PIXEL3D_CLEANUP_CAPTURE");
    if (!directory || !*directory) return;
    const auto target = std::filesystem::path(directory) / (std::string(stage) + ".bin");
    if (std::filesystem::exists(target)) throw std::runtime_error("refusing checkpoint overwrite");
    const uint64_t V = verts.size() / 3, F = faces.size() / 3, bytes = 8 + 12 * (V + F);
    if (bytes > (128ull << 20) || std::filesystem::space(target.parent_path()).available < 4ull*1024*1024*1024)
        throw std::runtime_error("cleanup capture exceeds budget/disk reserve");
    std::unique_ptr<FILE, decltype(&fclose)> f(std::fopen(target.string().c_str(), "wb"), &fclose);
    const uint32_t counts[]{uint32_t(V), uint32_t(F)};
    if (!f || std::fwrite(counts, 4, 2, f.get()) != 2 || std::fwrite(verts.data(), 12, size_t(V), f.get()) != size_t(V) ||
        std::fwrite(faces.data(), 12, size_t(F), f.get()) != size_t(F)) throw std::runtime_error("cleanup capture write failed");
}

// Surface deviation of A from B: area-weighted samples on A, exact closest
// point on B. Deterministic (fixed seeds). Returns {mean, p99, max}.
static std::array<double, 3> deviation(const std::vector<float>& av, const std::vector<int32_t>& af,
                                       const std::vector<float>& bv, const std::vector<int32_t>& bf, int samples) {
    const auto tree = trellis::TriBvh::build(bv.data(), int64_t(bv.size() / 3), bf.data(), int64_t(bf.size() / 3));
    const size_t F = af.size() / 3;
    std::vector<double> area(F + 1, 0);
    const auto corner = [&](size_t f, int k) { return &av[3 * size_t(af[3*f + size_t(k)])]; };
    for (size_t f = 0; f < F; ++f) {
        const float *a = corner(f, 0), *b = corner(f, 1), *c = corner(f, 2);
        const double u[3] = {double(b[0]) - a[0], double(b[1]) - a[1], double(b[2]) - a[2]};
        const double w[3] = {double(c[0]) - a[0], double(c[1]) - a[1], double(c[2]) - a[2]};
        const double x = u[1]*w[2] - u[2]*w[1], y = u[2]*w[0] - u[0]*w[2], z = u[0]*w[1] - u[1]*w[0];
        area[f + 1] = area[f] + 0.5 * std::sqrt(x*x + y*y + z*z);
    }
    std::vector<double> d(static_cast<size_t>(samples));
    #pragma omp parallel for
    for (int i = 0; i < samples; ++i) {
        std::mt19937_64 random(uint64_t(i) * 0x9E3779B97F4A7C15ull + 1);
        std::uniform_real_distribution<double> unit(0, 1);
        const double pick = unit(random) * area[F];
        size_t f = size_t(std::upper_bound(area.begin(), area.end(), pick) - area.begin()) - 1;
        if (f >= F) f = F - 1;
        const double r1 = std::sqrt(unit(random)), r2 = unit(random);
        const float *a = corner(f, 0), *b = corner(f, 1), *c = corner(f, 2);
        float p[3];
        for (int k = 0; k < 3; ++k) p[k] = float((1 - r1) * a[k] + r1 * (1 - r2) * b[k] + r1 * r2 * c[k]);
        d[size_t(i)] = std::sqrt(double(tree.closest(p).dist2));
    }
    std::sort(d.begin(), d.end());
    double mean = 0;
    for (double x : d) mean += x;
    return {mean / samples, d[size_t(samples) * 99 / 100], d.back()};
}

int main(int argc, char** argv) {
    if (argc != 4 || std::strcmp(argv[3], "--preserve-topology") != 0) {
        std::fprintf(stderr, "usage: pixel3d-trellis-post-safe raw.ply clean.glb --preserve-topology\n"); return 1;
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const std::string report_path = std::string(argv[2]) + ".cleanup.json";
        if (std::filesystem::exists(argv[2]) || std::filesystem::exists(report_path))
            throw std::runtime_error("refusing to overwrite existing output");
        const auto start = std::chrono::steady_clock::now();
        auto stage_start = start;
        const auto finish_stage = [&](const char* name) {
            const auto end = std::chrono::steady_clock::now();
            std::printf("[post-time] %s=%.3fs\n", name, std::chrono::duration<double>(end - stage_start).count());
            stage_start = end;
        };
        trellis::Mesh mesh;
        std::unique_ptr<FILE, decltype(&fclose)> input(std::fopen(argv[1], "rb"), &fclose);
        if (!input) throw std::runtime_error("cannot open generated PLY");
        char line[256]; long long vertices = 0, faces = 0; bool binary = false, ended = false;
        while (std::fgets(line, sizeof line, input.get())) {
            if (std::strstr(line, "format binary_little_endian 1.0")) binary = true;
            std::sscanf(line, "element vertex %lld", &vertices);
            std::sscanf(line, "element face %lld", &faces);
            if (std::strstr(line, "property uchar")) throw std::runtime_error("expected geometry-only XYZ PLY");
            if (std::strstr(line, "end_header")) { ended = true; break; }
        }
        if (!binary || !ended || vertices <= 0 || faces <= 0 || vertices > 10000000 || faces > 25000000)
            throw std::runtime_error("invalid generated PLY header");
        mesh.verts.resize(size_t(vertices) * 3); mesh.faces.resize(size_t(faces) * 3);
        if (std::fread(mesh.verts.data(), 12, size_t(vertices), input.get()) != size_t(vertices))
            throw std::runtime_error("short PLY vertex payload");
        for (long long i = 0; i < faces; ++i) {
            unsigned char n = 0;
            if (std::fread(&n, 1, 1, input.get()) != 1 || n != 3 ||
                std::fread(mesh.faces.data() + 3*i, 4, 3, input.get()) != 3)
                throw std::runtime_error("invalid PLY triangle payload");
            for (int j = 0; j < 3; ++j) if (mesh.faces[3*i+j] < 0 || mesh.faces[3*i+j] >= vertices)
                throw std::runtime_error("invalid triangle index");
        }
        input.reset();
        finish_stage("read-ply");
        std::printf("[post] input V=%d F=%d\n", mesh.V(), mesh.F());
        trellis::weld_vertices(mesh.verts, mesh.faces, nullptr, 1.0f / (512.0f * 8));
        finish_stage("weld-raw");
        trellis::fill_small_holes(mesh.faces);
        finish_stage("fill-raw");
        std::printf("[post] welded V=%d F=%d; building BVH\n", mesh.V(), mesh.F());
        trellis::Mesh remeshed;
        {
            auto bvh = trellis::TriBvh::build(mesh.verts.data(), mesh.V(), mesh.faces.data(), mesh.F());
            finish_stage("bvh");
            remeshed = trellis::remesh_narrow_band_dc(mesh.verts.data(), mesh.V(), mesh.faces.data(), mesh.F(), bvh, 512, 1);
            finish_stage("remesh");
        }
        if (!remeshed.F()) throw std::runtime_error("upstream remesh produced no surface");
        mesh = {};  // release the raw multi-million-face mesh before simplification
        trellis::clean_mesh(remeshed.V(), remeshed.faces);
        finish_stage("clean-winding");
        trellis::drop_small_components(remeshed.verts, remeshed.faces, 0.02f);
        finish_stage("drop-remeshed-components");
        checkpoint(remeshed.verts, remeshed.faces, "dc");

        const size_t dc_faces = size_t(remeshed.F());
        const auto slits = p3dtrellis::collapse_zero_length_edges(remeshed.verts, remeshed.faces);
        const auto degenerate = p3dtrellis::remove_exact_degenerate(remeshed.verts, remeshed.faces);
        std::printf("[post] zero-length edges=%zu collapsed=%zu (faces removed=%zu) skipped=%zu; remaining exact-zero-area faces removed=%zu\n",
                    slits.zero_length_edges, slits.collapsed, slits.faces_removed, slits.skipped, degenerate.removed_faces);
        if (!remeshed.F()) throw std::runtime_error("DC surface contains no positive-area faces");
        finish_stage("zero-length-edges");
        const auto sheets = trellis_topology::repair_sheets(remeshed.verts, remeshed.faces);
        std::printf("[post] separated sheets: F=%zu->%zu, deleted=0, fallback-edges=%zu, max-plane-error=%.9g\n",
                    sheets.input_faces, sheets.output_faces, sheets.fallback_edges, sheets.max_parent_plane_distance);
        finish_stage("separate-sheets");
        const auto shells = p3dtrellis::remove_enclosed_inner_shells(remeshed.verts, remeshed.faces);
        std::printf("[post] inner offset shells removed=%zu faces=%zu area=%.9g kept-unverified=%zu components=%zu->%zu\n",
                    shells.removed_shells, shells.removed_faces, shells.removed_area, shells.kept_negative_closed,
                    shells.components, shells.components - shells.removed_shells);
        finish_stage("inner-shells");
        checkpoint(remeshed.verts, remeshed.faces, "sheets");

        std::printf("[post] remeshed V=%zu F=%zu; QEM target=150000\n", remeshed.verts.size() / 3, remeshed.faces.size() / 3);
        std::vector<float> simplified_verts; std::vector<int32_t> simplified_faces;
        const auto qem = p3dtrellis::decimate_qem_guarded(remeshed.verts, remeshed.faces, 150000, simplified_verts, simplified_faces);
        if (!qem.budget_met)
            std::printf("[post] face budget not met below the shipped error threshold: %zu faces kept to preserve detail\n", qem.output_faces);
        std::printf("[post] qem rounds=%zu applied=%zu rejected: contact=%zu same-round=%zu sheet-star=%zu coincident=%zu degenerate=%zu duplicate=%zu guard=%.1fs\n",
                    qem.rounds, qem.applied, qem.rejected_contact, qem.rejected_batch, qem.rejected_star,
                    qem.rejected_coincident, qem.rejected_degenerate, qem.rejected_link, qem.guard_seconds);
        finish_stage("qem");
        // Fidelity record: the simplified surface against the separated DC
        // surface it came from (inner shells already gone), both directions.
        const auto to_dc = deviation(simplified_verts, simplified_faces, remeshed.verts, remeshed.faces, 200000);
        const auto from_dc = deviation(remeshed.verts, remeshed.faces, simplified_verts, simplified_faces, 200000);
        const double cell = (512.0 + 3.0) / 512.0 / 512.0;  // DC voxel edge, remesh_narrow_band_dc(res 512, band 1)
        std::printf("[post] deviation output->DC p99=%.3f max=%.3f voxels; DC->output p99=%.3f max=%.3f voxels\n",
                    to_dc[1] / cell, to_dc[2] / cell, from_dc[1] / cell, from_dc[2] / cell);
        finish_stage("fidelity");
        remeshed = {};
        // Splits are deliberate; proximity welding would join the sheets again.
        trellis_topology::validate_manifold_mesh(simplified_verts, simplified_faces);
        finish_stage("validate-topology");
        checkpoint(simplified_verts, simplified_faces, "final");

        std::unique_ptr<FILE, decltype(&fclose)> report(std::fopen(report_path.c_str(), "wb"), &fclose);
        if (!report) throw std::runtime_error("cannot save cleanup repair report");
        std::fprintf(report.get(),
            "{\"stage\":\"post-DC\",\"cleanup\":\"pixel3d-trellis-post-safe\",\"input_faces\":%zu,"
            "\"zero_length_edges\":%zu,\"zero_length_edges_collapsed\":%zu,\"zero_length_edges_skipped\":%zu,"
            "\"zero_area_faces_removed_by_collapse\":%zu,\"exact_zero_area_faces_removed\":%zu,\"repeated_index_faces_removed\":%zu,"
            "\"positive_area_faces_removed\":0,\"positions_changed\":0,"
            "\"sheet_separation\":{\"input_faces\":%zu,\"output_faces\":%zu,\"fallback_edges\":%zu},"
            "\"inner_offset_shells\":{\"components_before\":%zu,\"removed\":%zu,\"removed_faces\":%zu,\"removed_area\":%.9g,"
            "\"removed_signed_volume\":%.9g,\"kept_unverified\":%zu},"
            "\"qem\":{\"target_faces\":150000,\"budget_met\":%s,\"input_faces\":%zu,\"output_faces\":%zu,\"rounds\":%zu,\"final_threshold\":%.3g,\"max_threshold\":1e-05,\"endpoint_placements\":%zu,"
            "\"collapses_applied\":%zu,\"contact_guard\":\"exact\",\"rejected_contact\":%zu,\"rejected_same_round\":%zu,"
            "\"rejected_sheet_star\":%zu,\"rejected_coincident\":%zu,\"rejected_degenerate\":%zu,\"rejected_duplicate\":%zu,"
            "\"guard_seconds\":%.3f},"
            "\"fidelity\":{\"reference\":\"separated DC surface before QEM\",\"voxel\":%.9g,"
            "\"output_to_dc\":{\"mean\":%.6g,\"p99\":%.6g,\"max\":%.6g},\"dc_to_output\":{\"mean\":%.6g,\"p99\":%.6g,\"max\":%.6g}},"
            "\"audit_still_required\":true}\n",
            dc_faces, slits.zero_length_edges, slits.collapsed, slits.skipped, slits.faces_removed,
            degenerate.removed_faces, degenerate.repeated_index_faces,
            sheets.input_faces, sheets.output_faces, sheets.fallback_edges,
            shells.components, shells.removed_shells, shells.removed_faces, shells.removed_area, shells.removed_volume,
            shells.kept_negative_closed,
            qem.budget_met ? "true" : "false", qem.input_faces, qem.output_faces, qem.rounds, qem.final_threshold,
            qem.endpoint_placements, qem.applied, qem.rejected_contact,
            qem.rejected_batch, qem.rejected_star, qem.rejected_coincident, qem.rejected_degenerate, qem.rejected_link,
            qem.guard_seconds, cell, to_dc[0], to_dc[1], to_dc[2], from_dc[0], from_dc[1], from_dc[2]);
        report.reset();

        if (!trellis::write_glb(argv[2], simplified_verts.data(), int(simplified_verts.size() / 3), simplified_faces.data(),
                                int(simplified_faces.size() / 3), nullptr, 42))
            throw std::runtime_error("cannot write postprocessed GLB");
        finish_stage("write-glb");
        std::printf("[post] output V=%zu F=%zu elapsed=%.1fs\n", simplified_verts.size() / 3, simplified_faces.size() / 3,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "[post] %s\n", e.what()); return 1; }
}
