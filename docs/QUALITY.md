# Quality gates and evaluation

3re measures its results and says what it measured. A result is **PASS** only when every check passes,
**REVIEW** when something cannot be decided from the input (it is shown, never hidden), **FAIL** when a check fails.
Defects 3re cannot repair are kept and reported; fidelity to your picture or drawing is never traded for a clean
report.

## Gates

### Pixel Art Builder and Low-Poly Studio

| Check | PASS means |
|---|---|
| Silhouette per view | the model rendered from each drawn view matches the drawing pixel for pixel (and the 90°/180°/270° rotations) |
| Solid | watertight, manifold, no self-intersections (Manifold booleans) |
| Depth | the model has real depth where the drawings say so; estimated hidden parts are marked as estimated |
| Export | the GLB is reloaded by an independent reader (tinygltf) and gives the same mesh; Low-Poly exports pass the Khronos glTF-Validator with 0 errors |

### Image to 3D (TRELLIS.2)

| Check | What it measures |
|---|---|
| Memory admission | free RAM before inference (about 3.3 GiB) — a run that would not fit does not start |
| GeometrySafe cleanup | decimation and cleanup never create self-intersections (verified binary `pixel3d-trellis-post-safe.exe`) |
| Geometry audit | components, boundary edges, non-manifold edges, intersections of the final mesh |
| Texture | registration of the picture on the mesh; a hidden-side gate reports backs that do not look like the seen side |
| Export | Khronos glTF-Validator, 0 errors |

### Agent

The Game-Ready Asset Agent works from a spec (input pictures, budget, platform) and accepts a result only when the
spec's gates pass: triangle budget, fidelity to the picture (`docs/agent/FIDELITY.md`), validator, and optional
engine import. Its decisions and measurements are logged per run (`run.jsonl`).

### Self-test of a release folder

`3re.exe --self-test [--deep] [--json FILE]` checks that a release folder is complete and self-contained: config,
helper programs, engine checksums, no foreign absolute paths, the models (sizes; `--deep`: SHA-256) and SAM loading.
Models that are not downloaded yet are a warning; a damaged file fails `--deep`. The updater runs it after every
update and goes back to the previous version if it fails.

## Evaluation

- Every model or method change is studied first (`docs/research/<topic>.md`: problem with evidence, sources,
  feasibility on a 4 GB GPU, licence, a hypothesis with acceptance criteria written before the experiment) and adopted
  only when the pre-written criteria are met. Negative results are documented and not merged (examples:
  `docs/research/agent-chat-brain.md`, the Pixal3D geometry evaluation).
- Regression runs use fixed seeds and the same gates before and after; tools are in `tools/geometry_eval/` and
  `tools/agent/`.
- The pictures of the maintainer's regression set are **not** in this repository: several are third-party pictures
  that cannot be redistributed. Results that depend on them are summarised without the pictures. A public evaluation
  set made only of CC0 or self-made pictures is an open task ([good first issues](good-first-issues.md)).
- Unit and integration tests: `ctest` (about 20 test programs, run on every pull request by CI).
