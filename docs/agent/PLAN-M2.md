# 3re Game-Ready Asset Agent — milestone 2 (what was built, rulings)

Branch `claude/3re-agent-milestone-2-d5f3ed` from `main` 6ef7be3. Still deterministic (no LLM). Results:
`docs/reports/agent-m2/README.md`.

| Part | Built | Where |
|---|---|---|
| B1 texture diagnosis | 17 visible defects catalogued with images and their source stage (picture projection, TRELLIS texture, calibration, …) | `TEXTURE-DIAGNOSIS.md`, `tools/agent/texdiag/` |
| B2 best-of-N TRELLIS texture | engine fork: texture session + texture-only replay (verified byte-identical); agent replays N−1 seeds, re-textures, keeps the candidate whose TRELLIS colours agree best with the picture on the seen side | `TEXTURE-CANDIDATES.md`, `tools/trellis/engine-patch/`, `agent_select.cpp` |
| B3 picture upscaling | Real-ESRGAN ncnn-vulkan measured (licences, size, VRAM, time) on three assets; **not adopted** (halos, lost line work on some close-ups) | `docs/reports/agent-m2/README.md` (B3) |
| B4 per-material colour match | hidden part of each picture material moved onto the seen mean of that material; reported per material | `auto_texture_bake.cpp`, `--no-material-match` |
| A1 game fidelity | `geometry.intersections` report/block; fidelity worker (distance, IoU, normals, block colour, topology); thresholds fixed before the benchmark | `FIDELITY.md`, `agent_fidelity.cpp`, `pixel3d-mesh-fidelity` |
| A2 decimation candidates | guarded QEM → QEM without the crossing guard; meshoptimizer and Instant Meshes evaluated and rejected on the failing case; "what the shape needs" probe | `FIDELITY.md`, `agent_select.cpp` |
| A3 foreign GLBs | multi-mesh world-space audit; UV-preserving per-mesh decimation by area with the GLB's own materials | `agent_gltf_decimate.cpp`, `pixel3d-decimate-gltf` |
| A4 front | the picture's registered front turned onto +Z (positions, normals, tangents; quaternion on root-node files) | `agent_glb.cpp` |
| A5 presets | mobile_character, mobile_prop, pc_character, pc_prop, hero_character with sources | `BUDGETS.md` |
| C | M2 specs fixed before running (`specs-m2/`), benchmark, package | report |

Rulings:
* Crossings never block a game target; soundness does (overfull/flipped/duplicate/zero-area → UNSAFE, open
  boundary → REVIEW). Printing keeps `block`.
* A reduction that fails fidelity is reported FAIL with its reasons and the triangle count the shape needs; it is
  never relabelled.
* Texture candidates: gate verdict first, then seen-side agreement; the run's own unless beaten by > 0.005.
  24 flow steps tested, not adopted.
* Upscaling stays out until a model shows clean close-ups (the brief's rule).
* The release's generation engine is the fork (same outputs, verified on 8 runs) so fresh generations get a
  texture session; the pinned 7a80638a store is kept untouched.
