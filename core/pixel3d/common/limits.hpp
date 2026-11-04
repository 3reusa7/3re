#pragma once
// Hard resource limits for untrusted input (source PNGs, .p3d archives, client
// requests). Pixel art is small; these bounds are generous for real drawings and
// keep a hostile file from exhausting memory or overflowing size arithmetic.
#include <cstdint>

namespace pixel3d::limits {

// Source images.
inline constexpr uint64_t kMaxImageFileBytes = 64ull << 20;  // one PNG file on disk or in a project
inline constexpr int kMaxImageSide = 8192;                   // decoded width or height
inline constexpr int64_t kMaxImagePixels = 16ll << 20;       // decoded width × height (64 MiB RGBA)
// Layered sources (.aseprite, .psd): layers read, and their pixels summed over all layers.
inline constexpr int kMaxLayers = 1024;
inline constexpr int64_t kMaxLayerPixelsTotal = 4 * kMaxImagePixels;

// Model after alignment (pixels = world units).
inline constexpr int kMaxModelSide = 512;               // W, H or D (texture visibility renders at 4×)
inline constexpr int64_t kMaxModelCells = 64ll << 20;   // W × H × D analysis voxels

// Rendering requests.
inline constexpr int kMaxRenderSide = 4096;  // rendered image width or height
inline constexpr int kMaxTurntableFrames = 360;

// .p3d project archives.
inline constexpr uint64_t kMaxProjectFileBytes = 512ull << 20;
inline constexpr uint32_t kMaxArchiveEntries = 1024;
inline constexpr uint64_t kMaxArchiveTotalBytes = 1ull << 30;  // sum of uncompressed sizes
inline constexpr uint64_t kMaxProjectJsonBytes = 16ull << 20;
inline constexpr uint64_t kMaxMeshCacheBytes = 256ull << 20;
inline constexpr uint64_t kMaxLogBytes = 32ull << 20;
inline constexpr uint64_t kMaxMetaBytes = 1ull << 20;
inline constexpr uint64_t kMaxCompressionRatio = 200;  // per entry beyond the first MiB (PNG ~1, JSON ~20)
inline constexpr int kMaxJsonDepth = 64;
inline constexpr int kMaxCheckpoints = 4096;
inline constexpr int kMaxDecisions = 100000;
inline constexpr int kMaxDepthConstraints = 4096;
inline constexpr int kMaxPolygonPoints = 1 << 20;  // all rings of one region
inline constexpr int kMaxTextureAssignments = 4096;
inline constexpr int kMaxAssignedTriangles = 1 << 22;
inline constexpr int kMaxLogEntries = 1 << 20;

}  // namespace pixel3d::limits
