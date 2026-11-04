# Game fidelity criterion (agent milestone 2, A1) — thresholds fixed 2026-10-30, before the M2 benchmark

For game targets (Godot, Unity, Unreal) the question is not "does the surface touch or cross itself" — engines
draw touching and crossing layers without trouble — but **does the reduced model look like the original, and is
the file sound**. Crossings are still measured (exact audit) and listed in every report; with
`geometry.intersections = "report"` (default for game engines) they never block. `"block"` keeps the milestone-1
rule (3D printing, booleans): no new crossing is accepted.

Worker: `pixel3d-mesh-fidelity ORIGINAL.glb REDUCED.glb OUT.json` (the same code runs inside the agent:
`apps/agent/agent_fidelity.cpp`). Tests: `tests/agent/test_agent.cpp`, cases `fidelity: *`.

## What is measured

| Measure | How | Why |
|---|---|---|
| Surface distance mean / p95 / max | 60,000 area-weighted samples on each surface, exact closest point on the other (BVH), symmetric, % of the original's bounding-box diagonal | overall shape; **max** catches local spikes and lost parts that p95 hides |
| Silhouette IoU | unlit orthographic renders, 8 yaws (0, 45, …, 315) at elevation 10 + top, 256 px, one frame (the original's box) for both; worst view judged | outline a player sees |
| Normal difference | mean angle between the front-most surface normals on pixels both cover, two-sided | shading / faceting |
| Colour difference | unlit base colour, Oklab ΔE, on **8×8-pixel block means** of pixels both cover (per-pixel ΔE is reported too) | a region painted another colour counts; a fine line moved by one pixel does not (that is re-baking, not a visible change) |
| Topology | boundary edges, holes (boundary loops), overfull edges, edges with inconsistent winding, degenerate triangles — the reduced mesh may not add any | the file is sound (no new holes, flipped faces, slivers). Untextured meshes are read on their stored indices (touching sheets stay separate); textured ones on exact positions (UV seams are not holes) |

## Thresholds

| Measure | Limit |
|---|---|
| distance p95 | ≤ 0.25 % of the diagonal |
| distance mean | ≤ 0.10 % |
| distance max | ≤ 1.5 % |
| silhouette IoU, worst of 9 views | ≥ 0.97 |
| normal difference, mean | ≤ 12° |
| colour, 8×8 block mean ΔE | ≤ 0.05 (only when both meshes are textured) |
| topology | nothing added |

A spec may tighten or loosen them (`fidelity` object, same names); the report always prints the limits used.

## Calibration (numbers measured before choosing the limits)

Original = the milestone-1 decimator input (GeometrySafe mesh). "M1 guarded" = what milestone 1 delivered;
"no-guard" = plain QEM (`pixel3d-decimate-guarded --no-guard`) to the budget, to ¼ and to ⅒ of it (deliberately too
far). Renders compared by eye: `the maintainer's private evaluation folder\agent-m2\fidcal\rogue-geometry.png`.

| Asset (budget) | Mesh | tris | p95 % | mean % | max % | IoU min | normals ° | by eye |
|---|---|---|---|---|---|---|---|---|
| sword (8,000) | M1 guarded | 7,678 | 0.062 | 0.022 | 0.35 | 0.986 | 6.7 | same |
| | no-guard budget | 7,656 | 0.056 | 0.021 | 0.35 | 0.984 | 6.5 | same |
| | no-guard ¼ | 1,982 | 0.164 | 0.065 | 0.56 | 0.944 | 10.4 | blade outline faceted |
| | no-guard ⅒ | 800 | 0.335 | 0.136 | 1.75 | 0.914 | 15.0 | coarse |
| the anime-style character (30,000) | M1 guarded | 43,250 | 0.166 | 0.056 | **3.82** | 0.980 | 9.4 | close, local spikes |
| | no-guard budget | 29,830 | 0.106 | 0.046 | 0.68 | 0.986 | 9.6 | same |
| | no-guard ¼ | 7,486 | 0.536 | 0.201 | 5.48 | 0.909 | 20.5 | visibly coarse |
| rogue (30,000) | M1 guarded | 56,034 | 0.106 | 0.036 | **3.60** | **0.967** | 7.3 | cloak edges moved |
| | no-guard budget | 29,612 | 0.087 | 0.034 | 0.66 | 0.988 | 7.8 | same (nose flatter, as M1) |
| | no-guard ¼ | 7,480 | 0.840 | 0.279 | 10.2 | 0.776 | 18.1 | face and hands destroyed |

Colour (the anime-style character, textured, against the wizard's texture on the full mesh): M1 re-texture block ΔE 0.035, with B4
material match 0.038 (both look the same as the wizard), a TRELLIS candidate that paints the blue boots black at
the back 0.067, a mottled candidate 0.050. Per-pixel ΔE for the look-alikes is 0.066 (line work shifting by a
pixel), which is why the block measure is judged.

Known limit of the criterion: small local details (the rogue's nose) can flatten while every global number stays
well inside the limits; the max distance catches only what moves more than 1.5 % of the diagonal. Close-up
renders stay part of the benchmark for that reason.

## Decimation candidates (A2)

For a game target the agent tries, in order, and keeps the first that meets **budget and fidelity**:
1. guarded QEM (milestone 1: no new self-contact);
2. QEM without the crossing guard (`--no-guard`; crossings are measured and reported, never welded);
3. only if both fail fidelity on a thin shape: a permissive remesher (Instant Meshes, BSD-3) — evaluated only if
   the benchmark shows such a case.
Every rejected candidate is logged with its numbers.

### Third candidate evaluated on the case where both QEM candidates fail (vampire, shredded cloak, budget 30,000)

Source: the vampire's GeometrySafe mesh, 176,580 triangles. Same fidelity worker, same limits.

| Candidate | Triangles | p95 % | max % | IoU min | normals ° | Topology added | Verdict |
|---|---|---|---|---|---|---|---|
| guarded QEM (M1) | stops above budget | 0.055 | 1.80 | 0.987 | 4.6 | none | over budget; max spike |
| QEM without the crossing guard | 29,918 | 0.180 | 2.55 | 0.950 | 16.0 | none (1,230 crossings, reported) | **fails fidelity** (outline, normals) |
| meshoptimizer 0.24 simplify (MIT) | 29,944 | 0.121 | 0.57 | 0.986 | 13.3 | 267 holes, 875 non-manifold edges, 1,622 zero-area triangles | rejected: unsound file |
| Instant Meshes (BSD-3, official 2019 Windows build, triangle mode) | 10,518 | 1.044 | 8.73 | 0.728 | 32.0 | 59 holes, 713 non-manifold edges | rejected |
| Instant Meshes, larger target | 38,096 (over budget) | 0.441 | — | 0.858 | 22.5 | — | rejected even over budget |

None reaches 30,000 triangles within the limits: the cloak is hundreds of thin closed strips, and every
simplifier either flattens them (outline and normals change) or breaks the file. The agent delivers the most
faithful in-budget mesh and reports fidelity FAIL with these reasons; a person decides between a higher budget
and the visible loss. (Instant Meshes was downloaded for this test only — `instant-meshes-windows.zip`,
SHA-256 c9f6217e…068d — and is not shipped.)
