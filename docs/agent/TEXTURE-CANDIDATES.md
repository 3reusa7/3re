# Best-of-N TRELLIS texture (agent milestone 2, B2)

## Can the engine repaint only the texture?

The pinned engine (`trellis-cli.exe` 7a80638a) cannot: its texture stage draws its noise from the same RNG as the
shape stages, and nothing of the texture stage's input is saved. Added in the engine fork
(`tools/trellis/engine-patch/texture-candidates.patch`, on the pinned 7a80638a source, MIT):

* `TRELLIS_TEX_SESSION=<file>` during a normal run writes the texture stage's inputs: shape SLAT (coords +
  normalized features), DINOv3 conditioning, the shape decoder's subdivision masks, the voxel coords and the RNG
  state at the start of the texture stage (5–20 MB). The run's own outputs are unchanged.
* `--tex-replay <session> --post-from native.post --dump-post out.post --seed N [--tex-steps 12|24]` runs **only**
  the texture flow + PBR decoder and copies the mesh and voxel coords byte for byte from the run's own dump.

Verification (RTX 3050 Laptop 4 GB):

| Check | Result |
|---|---|
| fork, same arguments as the 3re backend, vs the saved runs' `native.post` (8 assets: sword, the anime-style character, pixel hood, vampire, rogue, office chair, glass bowl, bike) | **byte-identical, 8 / 8** |
| replay with the run's own RNG state (`--seed 0`) vs `native.post` (sword) | **byte-identical** |
| replay with seeds 1–3 | mesh + voxel coords byte-identical, PBR different |
| time per replay, 12 / 24 flow steps | 8–48 s / 11–75 s; dense glass bowl (2.5 M voxels) 3–10 min |
| VRAM per replay (WDDM, above the desktop) | ~0.76 GiB (peak 939–972 MiB total) |

## Experiment: 8 candidates per asset, same mesh, same picture

Run's own texture (s0) + seeds 1–3, each at 12 and 24 flow steps, re-textured by the shipped worker on the
milestone-1 mesh; scored with the worker's gates plus a back-view detail measure; contact sheets
`docs/reports/agent-m2/b2/<asset>-candidates.jpg` (backs from the registered camera's 135/180/225°).

What a person sees, and what the numbers say:

| Asset | By eye | Gate (unsupported patches / front copy / chroma) | Seen-side agreement ΔE (lower = better) |
|---|---|---|---|
| the anime-style character | s0 best; s2 mottled dirty orange; s1/s3 paint the blue boots black at the back | all PASS, s3 marginally "best" on gates | **s0 0.165** (s1 0.169, s2 lightness-only, s3 0.167) |
| office chair | **s1/s2 better**: green backrest back instead of s0's washed grey patches (D15) | s1: unsupported 0.000 vs 0.015 | **s1 0.080** vs s0 0.098 |
| pixel hood | s3: darker hood back, less face-like (D6) | s0 REVIEW (front copy 0.46), s2/s3 PASS | s0 0.143 (REVIEW); best PASS **s3 0.153** |
| vampire | s1 removes the pink cloak blotches (D10) but paints the white hair dark at the back | s1 better chroma (0.96 vs 0.73) | **s0 0.117** (s1 0.141) |
| rogue | s1/s3 paint the cloak back red (not in the picture) | s1 better chroma | **s0 0.095** |
| sword | s1/s2 blotchy | s1–s2 REVIEW (unsupported 0.12–0.61) | **s0 0.183** |
| glass bowl, bike | no visible improvement | — | **s0** best (0.089, 0.205) |
| 24 vs 12 steps | no visible difference on any asset | ±0.01 | never better than its 12-step sibling by > 0.005 |

The gate numbers alone would pick candidates that swap a part's colour (the anime-style character's boots, the rogue's cloak); a
"continuity across the picture/TRELLIS border" prototype (`tools/agent/texcand/continuity.py`) did not separate
them either. **The seen-side agreement did, on every asset**: the held-out ΔE between the candidate's own TRELLIS
colours (after calibration) and the picture, on surface the picture sees. A texture model that paints the seen
side like the picture is the one that paints the hidden side right.

## Rule (agent, `agent_select.cpp`)

1. Best texture verdict first (PASS over REVIEW over FAIL).
2. Among those, the lowest seen-side agreement ΔE; the run's own texture stays unless another beats it by more
   than 0.005 (when the calibration falls back to lightness only, the uncalibrated ΔE is used).
3. N = 4 by default (`texture.candidates`), 12 flow steps (`texture.flow_steps`; 24 tested, not adopted:
   no visible gain for 1.5–2× the time). Skipped, and said so, without a session, the replay engine, a GPU of
   ≥ 2 GiB or under `limits.vram_mib` < 1.5 GiB; stops at 80 % of `limits.total_seconds`.
4. Every candidate is logged (verdict, agreement, gate numbers, replay time) with a back thumbnail
   (`<output>/candidates/*.png`).

Defects addressed: D15 (office chair backrest), D6 (pixel hood hood back, made less face-like — still TRELLIS's
guess). Not addressed by any candidate: D3 (the anime-style character's back hair), D9 (vampire's blob hair), D11, D13, D14, D16 —
every seed paints them similarly (TRELLIS's 512-voxel texture limit, see the M2 report, B5).
