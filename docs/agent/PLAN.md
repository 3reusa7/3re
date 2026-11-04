# 3re Game-Ready Asset Agent — MVP plan (milestone 1)

Branch `claude/3re-asset-agent-audit-e06c7d` from `main` 308d4c7. No LLM in this milestone: every decision
is a deterministic rule over measured numbers, logged with its reason. Audit and baseline: `AUDIT.md`.

Tool note: ECC is installed as skills only (no `/plan`, `/tdd` commands, no ECC agents). Equivalents used:
this plan (instead of `/plan`), the `tdd-workflow` skill (instead of `/tdd`, RED → GREEN per component with
doctest + ctest), the `feature-dev:code-reviewer` agent (instead of `code-reviewer`), the `security-review`
skill (subprocesses, paths).

## Command

`3re-agent.exe spec.json [--out DIR] [--config 3re.config.json] [--godot PATH]` — a separate console program
in `app\` (the studio is a WIN32 program without a console; a console program shows the live log, which the
demo needs). `3re.exe --agent spec.json` forwards to it (launcher change, one line).

## Components (all under `apps/agent/`, tests in `tests/agent/`)

| # | Component | What it does | Tests first |
|---|---|---|---|
| 1 | `AssetSpec` | JSON spec: input (picture / glb / reuse_run + native), engine (godot), type, triangle budget, texture size, scale in metres (axis), acceptance, limits, output dir. Strict validation, every error named. | parse valid/invalid specs |
| 2 | Capability probe | RAM, disk, GPU name + VRAM (DXGI), workers present + SHA-256 (engine and post-safe against the config pins, TRELLIS weights against the manifest), validator, Godot version. Writes the tool registry. | registry from a fake folder, tampered hash -> unavailable |
| 3 | Planner | stages from spec + capabilities: GLB input skips generation; reuse_run reuses; outdated cleanup -> re-run pinned GeometrySafe; picture without alpha -> use the run's cutout / the wizard's background removal; missing generation capability -> BLOCKED with the reason. | plan tables per case |
| 4 | Executor | runs real workers in a Windows job object (memory limit, time limit, kill on close), records exit code, duration, peak memory; every step appends one line to `run.jsonl`. | child that allocates / sleeps / exits N |
| 5 | Evaluator | triangles, AABB/scale/orientation, materials/UV presence (GLB JSON), topology + crossings (existing `pixel3d-asset-inspect` + `pixel3d-geometry-audit` + `classify_validation`), texture verdict + numbers from the worker's `.texture.json`. | GLB fixtures, verdict compare |
| 6 | Recovery controller | bounded rules, each decision with its reason: over budget -> guarded QEM to the budget -> re-texture (picture + native.post) -> compare texture checks; worse -> gentler target (1.5x budget) once -> REVIEW. OOM -> lower setting (texture size halves to 1024) / wait-and-retry generation once. UNSAFE -> re-run pinned cleanup (saved runs) then regenerate with seed+1 (max 2). Texture REVIEW -> alternative candidate (picture-only vs hybrid) -> human review. Per-rule caps + total time budget. | pure state machine |
| 7 | Delivery validator | Godot preset GLB: root node scaled to the spec's metres and grounded (feet at y = 0, centred), vertices untouched; Khronos validator 0 errors; independent reload (`pixel3d-glbcheck`, tinygltf); real Godot 4.7 headless import checking triangles, AABB/scale, up axis, materials. No Godot -> UNVERIFIED. | transform maths, no-Godot path |
| 8 | Report | `report.json` + `report.md`: plan, decisions + reasons, before/after numbers, paths, SHA-256 of deliverables, what needs a human. | report from a fake run |

New worker: `pixel3d-decimate-guarded in.glb out.glb --target N [--max-threshold T]` — the shipped
`decimate_qem_guarded` (exact contact guard: no new crossings) on a GLB, with a surface-deviation measure
(original vertices to decimated surface, `tri_bvh`, % of bbox diagonal). The cleanup default stops at threshold
1e-5 (fidelity first); a game budget is an explicit user request, so the agent raises the threshold ladder
but keeps the contact guard, and reports the deviation; deviation p99 > 1 % of the diagonal -> REVIEW.

`--e2e-seed N` added to the studio's end-to-end driver so a regeneration can use another seed through the real
wizard path.

## Proof (step 3)

Fault injection (each through the real executor and workers): simulated OOM (job memory limit on the texture
worker), over-budget mesh, opaque picture, tampered weight manifest, missing Godot. Then the 7 regression
assets (saved runs), one fresh generation, one GLB not from 3re; table baseline vs agent. Reviews, all tests,
report, CHANGELOG, VERSION, `tools\package.ps1`, commit, local merge.
