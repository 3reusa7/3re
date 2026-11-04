# TRELLIS.2 native integration — preview build

This module adds a separate offline Image to 3D workflow to the existing Qt
application. Open **Generate from One Image** on the start screen or **Tools →
Image to 3D**. Visual Hull, Low-Poly Studio, drawing projects, editing, MCP and
CLI continue to use their existing Session and export code. Smart Quad is
preserved as Phase 2 and disabled by default (`PIXEL3D_ENABLE_SMARTQUAD=OFF`).

The backend launches the already-built local `trellis-cli.exe`, followed by
the accepted DC/QEM archive through `pixel3d-trellis-post-preview4.exe --preserve-topology`.
Only the offline cleanup entry is compiled; neither TRELLIS nor GGML is rebuilt.
The pinned configuration is 512 geometry,
12 steps, Q4 weights, CUDA GPU 0, `--no-fa`, and `--low-vram-4gb`. Optional bounded
SS/SLAT/decoder capture uses the existing implementation. The corrected image
crop, layouts, memory optimizations and DC/QEM fixes are unchanged.

## Local runtime

Place a machine-local JSON setup file beside the executable as
`trellis-backend.local.json`, set `PIXEL3D_TRELLIS_CONFIG`, or choose it in Backend
Setup. The development default is `.toolchain/trellis/backend.json` (ignored by
Git). It identifies `runtime_dir`, `models_dir`, `post_executable`,
`weights_manifest`, `engine_sha256` and `post_sha256`.

The existing verified runtime and five geometry weights are referenced in
place. Before inference, checksums are verified asynchronously using bounded
streaming reads. No weights, services, Python or Blender are downloaded or
required by this feature. Keep the original runtime's adjacent CUDA/GGML and
MinGW DLLs. Model/runtime licenses remain with those external dependencies;
no model files are bundled into this source tree.

## Images and outputs

Use a prepared transparent PNG for any isolated photograph, illustration,
character or pixel-art object. The optional white-background method removes
only exterior-connected pixels with each RGB channel at least 248; enclosed
white highlights survive. Complex backgrounds require an external prepared
cutout. Users review the actual alpha preview. Opaque RGB-to-RGBA conversion is
never treated as successful background removal. Inputs are bounded to 4096².
Normalization and crop are delegated to the verified TRELLIS preprocessor.

Every run creates a new directory. It retains the reviewed input, raw GLB/PLY,
cleaned geometry GLB, optional stage captures, full logs, memory samples and
validation report. Interactive preview uses the existing renderer with a
display-only centering/scale transform; exported coordinates are unchanged.
The generated-asset contract is separate from drawing documents and explicitly
declares geometry-only/PBR-unvalidated capabilities for future Smart Quad use.

Source-projected colors remain the default, explicitly labeled approximate.
Supported saved generated UV/PBR materials are retained on import/export; native
preview displays base color only. Generated texture fidelity remains experimental
and is not promoted. Saved-result inspection accepts one static, identity-transform
mesh (up to two million vertices, four million triangles and a 256 MiB GLB).
Unsupported material features, instanced, rigged or animated inputs fail explicitly.
Original files are never rewritten.

## Preview4 recovery and resolution limits

The raw character's valid 8,182,014-index accessor exceeded the old six-million
index cap. Recovery now shares the explicit 2M-vertex/4M-face profile with the
isolated auditor; invalid strides, offsets and index layouts still fail.

Before the unchanged sheet repair, the cleanup entry removes only exact-zero-area
faces, measured with F64 cross products on F32 positions. It changes no positions,
keeps every positive-area triangle (including arbitrarily thin ones), never welds
or fills holes, and records removal counts in `geometry.glb.cleanup.json`.
Subsequent DC/QEM topology and intersection audits remain mandatory. The saved
character recovered in this pass has 477 crossings and eight boundary edges:
it remains UNSAFE. This fixes recovery, not those geometric defects.

Build the offline entry with `PIXEL3D_TRELLIS_SOURCE_ROOT` pointing to the existing
pinned backend source and `build-cuda-prebuilt/libtrellis_core.a`. No weights or
new solver dependencies are needed. Optional `PIXEL3D_CLEANUP_CAPTURE` points to
an existing empty diagnostic folder; each native checkpoint is capped at 128 MiB
and keeps the 4 GiB disk reserve. It is disabled during normal operation.

True 1024 requires separate 1024 flow weights and the upstream cascade. Those
weights are absent and the pinned `--low-vram-4gb` runtime explicitly rejects
the cascade. The 1024 High Detail entry stays disabled with this explanation.
No 512 substitution, subdivision, download or new inference is performed.

## Validation and safety

Inspection runs outside the GUI process. It reuses Pixel3D topology checks and
the verified read-only TRELLIS BVH/plane-section intersection auditor. The
auditor has an 8-million-candidate/60-second bound. Incomplete, malformed or
unknown reports never pass. Crossings, overlaps and invalid topology report
**UNSAFE**; open boundaries, tolerance-sensitive or separate contacts report
**REVIEW**; unavailable checks report **UNKNOWN**. Intentional open surfaces
are not automatically repaired, welded, capped or deleted. Export with issues
requires explicit acknowledgment and makes a checked, non-destructive copy.

Generation requires 3 GiB available physical RAM; saved inspection requires
2.5 GiB. The worker retains a 1 GiB physical reserve, 4 GiB initial disk space
and 1 GiB running disk reserve. Windows Job Objects contain workers before they
resume, cap aggregate commit at 6 GiB and terminate descendants on cancellation.
The existing internal VRAM policy caps managed allocations at 3584 MiB. WDDM
PID memory is sampled every second; dedicated usage above 3900 MiB, shared GPU
usage above 512 MiB, or three telemetry failures stop generation. Total runtime
is bounded to 20 minutes. Dependency hashing, image preparation, model loading,
generation, cleanup, inspection, preview preparation and GLB copy use background
jobs/processes. Closing the feature preserves partial outputs and cancels its
workers without touching unrelated applications.

The saved banana remains coherent in shape but contains 7,272 independently
detected crossings. DC/QEM topology repairs do not certify self-intersection
freedom. Q4/512 generation can also contain slabs or disconnected surfaces.
Successful inference is never presented as a clean-mesh certification.

## Geometry safety (pixel3d-trellis-post-safe)

`pixel3d-trellis-post-safe` replaces `pixel3d-trellis-post-dev2` with the same
command line (`raw.ply out.glb --preserve-topology`) and outputs. The defect
sources were measured by replaying saved runs stage by stage (no inference) and
auditing every checkpoint with `pixel3d-geometry-audit`:

| Defect in shipped GLBs | Measured source | Fix |
|---|---|---|
| Transverse crossings (22-1,566 per asset) | Not in the DC surface (0 crossings at 1.7M faces). Created by QEM: its flip test checks each face's own normal, nothing else. The engine's opt-in collision guard was off and, when on, took 1,939 s on a 9.3M-face input and still left 23 crossings | `decimate_qem_guarded.cpp`: every new triangle is tested with the audit's own predicates (`qem_contact_guard.hpp`) and the collapse is refused, never repaired afterwards |
| Edge-face contacts | QEM folds through the collapsed vertex; the engine guard shrank triangles by 1e-7 before testing, which hides contacts that start at a shared vertex | Exact test of the new fan against itself |
| Crossing sheet stars | QEM rotating a face through its separated sister sheet around a shared edge position; the faces only meet on the edge line, so no pair test sees it | Edge-star test (same alternation rule as the audit) on every new edge with coincident copies |
| 4-8 open boundary edges | Preview4 deleted the zero-area faces between two DC vertices at exactly the same position: a zero-width slit | `collapse_zero_length_edges`: merge the two ids (positions unchanged); the slit closes |
| 3-95 components, mostly 4-face inverted shells | The DC remesh extracts the boundary of an eps-band around the generated surface; thick parts get an inner offset shell facing inward, sealed inside the solid. Sheet separation detaches those attached by pinches; QEM spends faces on them | `remove_enclosed_inner_shells`: closed, negative volume and verified inside a positive solid by ray parity. Nothing else is removed |

Visual fidelity comes before the face budget. A collapse refused at the
midpoint is retried keeping one endpoint (the new fan then uses existing
points only), with the same flip test, threshold, link test and guard. The
threshold ladder stops at 1e-5, the highest final threshold Dev2 needed on any
of the 20 saved runs: without that cap, refused collapses made the ladder climb
to 1e13 and accept collapses that erased thin features (Warrior: p99 deviation
from the DC surface 0.42 -> 0.85 voxel). When the budget cannot be met below
the cap, the remaining faces are kept and the cleanup report says
`"budget_met": false`.

Simplification still refuses only what the audit counts: a strictly disjoint
pair within the F32 scale tolerance (`near_contact_only`) is allowed, because
saved inputs already hold tens of thousands of them. The audit's coplanar test
reports coplanar triangles sharing one vertex as an overlap when their epsilon
bands meet; the guard mirrors that and stays conservative.

With the guard off, `decimate_qem_guarded` reproduces the shipped Dev2 output
bit for bit (positions and indices, verified on the saved the anime-style character run). The
per-edge cost loop runs in parallel; values are unchanged. What remains
REVIEW-level after the fix is genuine: pinch points where DC fused two parts of
the surface (thin gaps under two voxels) touch at a point or along an edge.
Moving them apart would change the shape, so they are reported, not hidden.

Tests: `trellis-geometry-safety` (saved fold and sheet-star coordinates, slit,
inner shells, thin plates with and without the guard). Before/after evidence
for every saved run: `docs/reports/geometry-safety/`.

## Verification status

Runtime and exported assets from earlier standalone runs are preserved evidence,
not a substitute for a new integrated end-to-end test. Build, Qt integration,
regression, saved-mesh replay, export/reimport and one guarded real generation
must pass before the integration can be called verified. The task evidence
directory records actual execution status; RAM-preflight blocks do not count
as successful tests or a completed build.
