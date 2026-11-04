# Vendored third-party code

All dependencies are vendored at pinned versions so Pixel3D builds offline and reproducibly.
Local modifications are listed per entry.

| Library | Version / commit | Licence | Upstream | Used for |
|---|---|---|---|---|
| Manifold | v3.5.4 | Apache-2.0 | https://github.com/elalish/manifold | Robust mesh booleans (TRUE SOLID prism intersection/union), triangulation |
| Clipper2 | 46f639177fe418f9689e8ddb74f08a870c71f5b4 (the commit Manifold v3.5.4 pins) | BSL-1.0 | https://github.com/AngusJohnson/Clipper2 | 2D polygon union/intersection (contours, atom regions) |
| nlohmann/json | v3.12.0 (`single_include/nlohmann/json.hpp`) | MIT | https://github.com/nlohmann/json | Reports, project files, OpResult |
| stb_image / stb_image_write | 2c980bb59875b0d32144a71867fbdebb2f77cd20 | MIT / public domain | https://github.com/nothings/stb | PNG decode/encode |
| miniz | 3.1.2 (amalgamated release `miniz.c/.h`) | MIT | https://github.com/richgel999/miniz | `.p3d` ZIP container |
| tinygltf | v2.9.7 (`tiny_gltf.h`) | MIT | https://github.com/syoyo/tinygltf | Independent GLB reload check only (our GLB writer is our own) |
| doctest | v2.5.3 (`doctest.h`) | MIT | https://github.com/doctest/doctest | Unit and integration tests |
| meshoptimizer | 0.24 (`meshoptimizer.h`, `simplifier.cpp` only; the copy vendored by trellis.cpp 610c90e) | MIT | https://github.com/zeux/meshoptimizer | UV-preserving decimation of foreign multi-mesh GLBs (agent) |
| libwebp | 1.6.0 (release archive, GPG-verified) | BSD-3-Clause + patent grant | https://chromium.googlesource.com/webm/libwebp | WebP source images (decode only) |
| xatlas | (`xatlas.cpp/.h`) | MIT | https://github.com/jpcy/xatlas | Texture atlas packing (auto texture) |
| Monocypher | 4.0.3 (`monocypher.c/.h`, `optional/monocypher-ed25519.c/.h`, `LICENCE.md`, `AUTHORS.md`; tarball SHA-512 `40904ada…8f8965` checked against monocypher.org's published digest) | CC0-1.0 or BSD-2-Clause | https://monocypher.org/ | Ed25519 + BLAKE2b: minisign signatures of update manifests (`apps/update`) |

Local modifications:

- `manifold/`: removed `test/`, `samples/`, `extras/`, `docs/`, `scripts/`, packaging and editor files; `bindings/` keeps only its `CMakeLists.txt` (all bindings are disabled).
- `libwebp/`: decode only. Kept from the 1.6.0 release archive: `src/webp/*.h`, `src/dec/`, the decoder DSP files of
  `src/dsp/` (C, SSE2, SSE4.1, AVX2 lossless), the decoder utils of `src/utils/`, `src/demux/{demux,anim_decode}.c`,
  `COPYING`, `PATENTS`, `AUTHORS`, `README.md`. Removed: the encoder, mux, NEON/MIPS/MSA code, examples, tools, docs,
  build files (Pixel3D builds the kept files itself, `cmake/ThirdParty.cmake`). No file was changed. Verification and
  the exact file list: `libwebp/README.p3d.md`; `tools/fetch_libwebp.ps1` repeats the download and checks.
- `Clipper2/`: only `CPP/` (minus `Tests/`, `Examples/`, `Utils/`, `BenchMark/`) and `LICENSE`. Manifold's iostream carry-patch is not applied (not needed with `MANIFOLD_NO_IOSTREAM=OFF`).
