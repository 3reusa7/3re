# Research: best-of-N TRELLIS.2 texture by texture-only replay (agent M2, B2)

Status: **adopted** in 2.7.0-alpha (agent default 4 candidates). Order note: this milestone's brief (2026-10-30) asked
for the method directly; the project rule requiring this document landed on main the same day (8815d53), during
the work. The study, measurements and experiment below happened before adoption; which criteria were written
before the runs and which were derived from them is stated in §5.

## 1. Problem

Hidden sides painted by TRELLIS's own texture carry visible defects that are TRELLIS's guess, not 3re's processing
(`docs/agent/TEXTURE-DIAGNOSIS.md`: D6 face-like blotch on the pixel hood's hood back, D10 pink blotches on the
vampire's cloak, D15 stripes and washed patches on the office chair's backrest back; images
`docs/reports/agent-m2/diag/*-stages.jpg`, row "trellis raw"). TRELLIS's texture stage is a stochastic flow sampler:
another noise draw paints the same geometry differently.

## 2. Study

* TRELLIS.2 (Microsoft, MIT): geometry and texture are separate structured-latent flows; the texture flow is
  conditioned on the shape latent and the image, its starting noise is the only random input
  (https://github.com/microsoft/TRELLIS.2; the port we run: https://github.com/pwilkin/trellis.cpp, MIT).
* Fact (our engine source, `src/trellis_cli.cpp`): one `std::mt19937` drives the sparse-structure, shape and texture
  noise in order; nothing of the texture stage's input is saved. So "another seed" for the texture alone needs
  (a) the shape latent, conditioning, subdivision masks and voxel coords saved, and (b) a replay entry point.
* Assumption (tested in §5): a candidate whose TRELLIS colours agree better with the picture on the seen side also
  paints the hidden side more plausibly (the model is consistent within one sample).

## 3. Feasibility (RTX 3050 4 GB, 16 GB RAM)

Measured: texture-only replay 8–75 s (12 or 24 flow steps), 3–10 min on the 2.5 M-voxel glass bowl; ~0.86 GiB VRAM
above the desktop (peak 939–1,030 MiB total); session file 5–20 MB. No download, no training.

## 4. Licence

trellis.cpp MIT, TRELLIS.2 weights as already used by 3re. The patch is ours (`tools/trellis/engine-patch/`).

## 5. Hypotheses, criteria, experiment

Written before running:
* H1 (engine): the fork must produce outputs byte-identical to the pinned engine; a replay with the run's own RNG
  state must reproduce the run's texture volume bit for bit; other seeds must not change one mesh byte.
  → **met**: 8/8 saved runs byte-identical; replay seed 0 bit-identical; mesh + voxel coords identical for seeds 1–3
  (re-verified after the review rebuild, engine 61fd983f).
* H2 (24 vs 12 flow steps): more steps give visibly better hidden sides. → **not met** (no visible difference on any of
  8 assets; agreement never better by > 0.005) → 24 steps not adopted.

Derived from the experiment (8 assets × 8 candidates, contact sheets `docs/reports/agent-m2/b2/`), not pre-written:
* the selection rule. Gate numbers alone picked colour-swapping candidates (the anime-style character's boots, the rogue's cloak); the
  seen-side agreement (held-out ΔE after calibration) matched the visual judgement on all 8. Applied unchanged to the
  two new pictures in the benchmark (both kept their own texture: no evidence either way there).
* Risk accepted and stated: the rule is fitted on 8 assets. `texture.candidates = 1` turns it off per spec.

## 6. Risks and cost

+2–15 min per asset (three replays and re-textures); the shipped generation engine is the fork (same outputs, verified),
the pinned 7a80638a store is untouched. Defects TRELLIS paints the same way in every sample (D3, D9, D11, D13, D14, D16)
are not addressed.
