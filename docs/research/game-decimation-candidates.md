# Research: decimation for game targets — game fidelity instead of "no self-intersection" (agent M2, A1/A2)

Status: **adopted** (fidelity criterion, QEM without the crossing guard as second candidate); meshoptimizer and
Instant Meshes as third candidate: **negative result** on the case that needed it.

## 1. Problem

Milestone 1: 6 of 9 assets FAIL on budget because the guarded QEM refuses collapses that would create
self-intersections on thin/layered TRELLIS shapes (rogue: 5,230 refused collapses). Engines draw crossing layers;
what matters is that the reduced model looks like the original and the file is sound.

## 2. Study

* QEM (Garland & Heckbert, SIGGRAPH 1997) — our guarded variant adds an exact contact guard
  (`tools/trellis/decimate_qem_guarded.cpp`); `--no-guard` is the same QEM without it.
* meshoptimizer 0.24 `simplifyWithAttributes` (https://github.com/zeux/meshoptimizer, MIT): attribute-aware QEM,
  seam- and border-preserving; vendored for foreign GLBs.
* Instant Meshes (Jakob et al., SIGGRAPH Asia 2015; https://github.com/wjakob/instant-meshes, BSD-3): field-aligned
  remeshing; official Windows build (2019).
* Fidelity measures: symmetric surface distance (Hausdorff-style, mean/p95/max), silhouette IoU over views, normal
  difference, image-space colour difference — standard in mesh simplification evaluation (e.g. Metro, Cignoni et
  al. 1998; image-driven simplification, Lindstrom & Turk, TOG 2000).

## 3. Feasibility

CPU, seconds per candidate (≤ 15 s on 190 k triangles). Instant Meshes: 7–15 s, 1.4 MB download (test only).

## 4. Licence

QEM ours; meshoptimizer MIT (vendored); Instant Meshes BSD-3 (not shipped).

## 5. Criteria (written before the benchmark: `docs/agent/FIDELITY.md`, commit 8f5ae77) and results

Limits calibrated on sword/the anime-style character/rogue (deliberately over-reduced meshes vs M1's, checked by eye), then fixed:
distance p95 ≤ 0.25 %, mean ≤ 0.10 %, max ≤ 1.5 %, IoU ≥ 0.97, normals ≤ 12°, block colour ΔE ≤ 0.05, no new
topology defects. Benchmark (11 assets): budget met 10/11 (M1 3/9), budget **and** fidelity 6/11. Third candidate on
the vampire: meshoptimizer breaks the file (267 holes, 875 non-manifold edges), Instant Meshes IoU 0.73–0.86 →
rejected, numbers in FIDELITY.md.

## 6. Risks

The fidelity measures are global: a small local loss (the rogue's nose) passes; close-up renders stay in the
benchmark. The limits were fitted on three assets.
