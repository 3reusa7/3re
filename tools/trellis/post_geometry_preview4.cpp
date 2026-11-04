// Offline reuse of upstream geometry postprocessing; no models or sampler runs.
#include "dual_grid.h"
#include "tri_bvh.h"
#include "remesh_dc.h"
#include "uv_bake.h"
#include "mesh_glb.h"
#include "mesh_topology.h"
#include "exact_degenerate.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <chrono>
#include <cstdlib>
#include <filesystem>

// Diagnostic-only native checkpoints; never rewrite an input or prior output.
static void checkpoint(const trellis::Mesh& mesh,const char* stage) {
    const char* directory=std::getenv("PIXEL3D_CLEANUP_CAPTURE");
    if(!directory || !*directory)return;
    const auto target=std::filesystem::path(directory)/(std::string(stage)+".bin");
    if(std::filesystem::exists(target))throw std::runtime_error("refusing checkpoint overwrite");
    const uint64_t bytes=8+12ull*(uint64_t(mesh.V())+uint64_t(mesh.F()));
    if(bytes>(128ull<<20) || std::filesystem::space(target.parent_path()).available<4ull*1024*1024*1024)
        throw std::runtime_error("cleanup capture exceeds budget/disk reserve");
    std::unique_ptr<FILE,decltype(&fclose)> f(std::fopen(target.string().c_str(),"wb"),&fclose);
    const uint32_t counts[]{uint32_t(mesh.V()),uint32_t(mesh.F())};
    if(!f || std::fwrite(counts,4,2,f.get())!=2 || std::fwrite(mesh.verts.data(),12,size_t(mesh.V()),f.get())!=size_t(mesh.V()) ||
       std::fwrite(mesh.faces.data(),12,size_t(mesh.F()),f.get())!=size_t(mesh.F()))throw std::runtime_error("cleanup capture write failed");
}

int main(int argc, char** argv) {
    if (argc != 3 && (argc != 4 || std::strcmp(argv[3], "--preserve-topology") != 0)) {
        std::fprintf(stderr, "usage: post_geometry_4gb raw.ply clean.glb [--preserve-topology]\n"); return 1;
    }
    const bool preserve_topology = argc == 4;
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        if (preserve_topology) {
            if (std::filesystem::exists(argv[2])) throw std::runtime_error("refusing to overwrite existing output");
#ifdef _WIN32
            _putenv_s("TRELLIS_QEM_PRESERVE_TOPOLOGY", "1");
#else
            setenv("TRELLIS_QEM_PRESERVE_TOPOLOGY", "1", 1);
#endif
        }
        const auto start = std::chrono::steady_clock::now();
        auto stage_start = start;
        const auto finish_stage = [&](const char* name) {
            const auto end = std::chrono::steady_clock::now();
            std::printf("[post-time] %s=%.3fs\n", name,
                std::chrono::duration<double>(end - stage_start).count());
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
        mesh = {}; // release the raw multi-million-face mesh before simplification
        trellis::clean_mesh(remeshed.V(), remeshed.faces);
        finish_stage("clean-winding");
        trellis::drop_small_components(remeshed.verts, remeshed.faces, 0.02f);
        finish_stage("drop-remeshed-components");
        if (preserve_topology) {
            checkpoint(remeshed,"dc-before-degenerate-filter");
            const auto degenerate=p3dtrellis::remove_exact_degenerate(remeshed.verts,remeshed.faces);
            // Exact-zero faces carry no surface area. Keep all points and all
            // positive-area triangles, including arbitrarily thin triangles.
            const auto report_path=std::string(argv[2])+".cleanup.json";
            if(std::filesystem::exists(report_path))throw std::runtime_error("refusing cleanup report overwrite");
            std::unique_ptr<FILE,decltype(&fclose)> report(std::fopen(report_path.c_str(),"wb"),&fclose);
            if(!report)throw std::runtime_error("cannot save cleanup repair report");
            std::fprintf(report.get(),"{\"stage\":\"post-DC pre-sheet-repair\",\"input_faces\":%zu,\"exact_zero_area_faces_removed\":%zu,\"repeated_index_faces_removed\":%zu,\"positive_area_faces_removed\":0,\"positions_changed\":0,\"audit_still_required\":true}\n",
                         degenerate.input_faces,degenerate.removed_faces,degenerate.repeated_index_faces);
            report.reset();
            std::printf("[post] exact-zero-area faces removed=%zu/%zu; positive-area removed=0; positions changed=0; audit remains required\n",degenerate.removed_faces,degenerate.input_faces);
            checkpoint(remeshed,"dc-after-degenerate-filter");
            if(!remeshed.F())throw std::runtime_error("DC surface contains no positive-area faces");
            const auto repair = trellis_topology::repair_sheets(remeshed.verts, remeshed.faces);
            std::printf("[post] separated sheets: F=%zu->%zu, deleted=0, fallback-edges=%zu, max-plane-error=%.9g\n",
                        repair.input_faces, repair.output_faces, repair.fallback_edges, repair.max_parent_plane_distance);
            finish_stage("separate-sheets");
        }
        std::printf("[post] remeshed V=%d F=%d; QEM target=150000\n", remeshed.V(), remeshed.F());
        trellis::Mesh simplified;
        trellis::decimate_qem(remeshed.verts, remeshed.V(), remeshed.faces, remeshed.F(), 150000,
                             simplified.verts, simplified.faces);
        finish_stage("qem");
        remeshed = {};
        if (preserve_topology) {
            // Splits are deliberate; proximity welding would join the sheets
            // again. Keep open surfaces and all surviving surface components.
            trellis_topology::validate_manifold_mesh(simplified.verts, simplified.faces);
            finish_stage("validate-topology");
        } else {
            trellis::weld_vertices(simplified.verts, simplified.faces, nullptr, 1.0f / (512.0f * 8));
            finish_stage("weld-final");
            trellis::fill_small_holes(simplified.faces);
            finish_stage("fill-final");
            trellis::drop_small_components(simplified.verts, simplified.faces, 0.03f);
            finish_stage("drop-final-components");
        }
        if (!trellis::write_glb(argv[2], simplified.verts.data(), simplified.V(), simplified.faces.data(), simplified.F(), nullptr, 42))
            throw std::runtime_error("cannot write postprocessed GLB");
        finish_stage("write-glb");
        std::printf("[post] output V=%d F=%d elapsed=%.1fs\n", simplified.V(), simplified.F(),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "[post] %s\n", e.what()); return 1; }
}
