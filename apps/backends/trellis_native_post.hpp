#pragma once
// Native texture run (spec 2026-10-20-hybrid-texture-design §1): the engine runs with texture enabled and
// stops after writing its POST dump (TRELLIS_DUMP_POST + TRELLIS_STOP_AFTER_POST_DUMP), before its own CPU
// bake. The dump holds the raw decoded mesh — the same mesh the geometry-only run writes — and the sparse
// PBR volume. This writes raw.ply / raw.glb from it in the engine's own formats (trellis.cpp write_ply /
// write_glb, no colours), so GeometrySafe and the audit see exactly what they see today.
#include <cstdint>
#include <filesystem>
#include <string>

namespace p3dasset {
struct NativePostSplit {
  int64_t vertices = 0, triangles = 0, voxels = 0;
  int resolution = 0;
};
// Throws std::runtime_error on a missing, truncated or inconsistent dump, or when an output exists.
NativePostSplit split_native_post(const std::filesystem::path& post, const std::filesystem::path& raw_ply,
                                  const std::filesystem::path& raw_glb, int64_t seed);
}  // namespace p3dasset
