# Research: per-material colour match of hidden TRELLIS colours (agent M2, B4)

Status: **adopted** in 2.7.0-alpha (texture worker default; `--no-material-match` off). Order note as in
`trellis-texture-candidates.md` (brief before the project rule).

## 1. Problem

Backs duller or browner than the same material on the front — the anime-style character's orange gi (D1), dark backs on the pixel hood and
rogue (D7, D11). Evidence: `docs/reports/agent-m2/diag/anime-stages.jpg` — TRELLIS's raw back orange is brighter than
what 3re delivered, so the darkening is 3re's calibration, not TRELLIS. Worker's own numbers before the change:
The anime-style character gi orange ΔE 0.042 front/back (lightness −0.034, chroma ×0.88), skin 0.080, wristband 0.118.

## 2. Study

The 2.2–2.4 pipeline (`docs/superpowers/specs/2026-10-20-hybrid-texture-design.md`, texture v2 report) fits one
colour map (Reinhard / L-quantile / LUT, chosen by held-out ΔE) from TRELLIS to the picture over all seen points, then
the material lock clamps hidden colours into each picture material's colour box. A single global map cannot correct a
material whose TRELLIS colour is off differently from the others; the box clamp keeps the colour inside the material
but anywhere in its range (often the shaded end). Per-material mean transfer (Reinhard-style mean shift within a
segment) is the standard fix in colour transfer (Reinhard et al., "Color Transfer between Images", IEEE CG&A 2001;
local/region-based variants e.g. Tai et al., CVPR 2005). Assumption: the material lock's chains already assign hidden
vertices to the right picture material (2.3 evidence: lock fixes "orange in black hair").

## 3. Feasibility

CPU only, inside the existing worker; no measurable time change (the anime-style character re-texture 20.6 s vs 21 s). No download.

## 4. Licence

Our code.

## 5. Hypothesis and criteria (written before implementing, as the test)

`tests/gui/auto_texture_tests.cpp` block 11, written first (RED): a material painted duller on the hidden side must
come within ΔE < 0.03 and < 50 % of its unmatched difference; colours the picture never shows must move < 0.01;
per-material numbers must be reported. → **met** (0.047 → 0.015; unseen 0.000). Benchmark (worker's own measure,
10 assets): mean front/back ΔE 0.009–0.040 → 0.000–0.004 (`docs/reports/agent-m2/README.md`, B4 table); texture
verdicts unchanged or better on all regression assets.

## 6. Risks

Lit pictures: matching the picture's mean also carries its lighting onto the back (accepted: the request is "backs not
duller than the front"). A wrong material assignment would now be moved to the wrong colour more strongly; none seen
on the 11 assets.
