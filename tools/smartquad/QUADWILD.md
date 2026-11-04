## 2026-10-26 evening: self-intersection audit in the worker (Claude, continuing codex/retopo-finish 1ee1eaa)

Codex's v4 engine (`work/retopo-preserve-folds-v4`: tracing refuses to erase unresolved patches) gives the
sword 3,215 quads, 1 component, 0 open edges - but its own last check found **211 transverse crossings**
(original: 0). Diagnosis: the crossings sit where the blade is thinner than a quad (facing surface median
3.0 mm away vs ~8 mm quad edges at 4k faces; 102 crossings at 4,256 quads, 66 at 8,346, 22 at 24,347).
Vertices are already on the surface (reprojection moves them < 1e-6); local Laplacian/tangential relaxing
of the folded faces made it worse (102 -> 117 folds, degenerate faces) and was dropped. QuadWild has one
global quad size, no local density, and no new solver is authorized, so this is reported, not hidden:
`pixel3d-retopo remesh` now runs 3re's `pixel3d-geometry-audit` on every result (`intersection_status`
CLEAR / CROSSING / NOT_TESTED, `self_intersections`, `safety` CLEAR / UNSAFE) and the workspace shows
"Unsafe: self-intersections" with the reason, or "Shape kept · no intersections".
In-app e2e (dev build, v4 engine via PIXEL3D_QUADWILD_DIR): organic fixture 1,360 quads, 0 open edges,
CLEAR, max deviation 0.24%, saved + re-imported, 22 s; sword 3,215 quads, 0 open edges, 211 crossings ->
UNSAFE, max deviation 0.93%, saved + re-imported, 125 s. dist/3re not changed in this step.

## 2026-10-23 isolated finish checkpoint (Codex)

Work continues in `codex/retopo-finish`, baseline 846da71. Claude's 22 pending files are backed up with SHA256 evidence before copying; main/dist/Claude remain unchanged. See `<maintainer workspace>/2026-10-14/files-pasted-by-the-user-trellis/outputs/validation/analysis/retopo-finish/REPORT.md` and checkpoint.json.

Automatic outer-shell closing and implicit simplification are disabled in worker routing. Topology changes are rejected; textures never silently disappear. Matched-atlas Base Color/MR transfer and material preservation are implemented but NOT runtime-validated. Intersections remain unverified. Density/local editing remain Coming Soon.

Build attempts v1 and v2 stopped at the unchanged 768 MiB free-RAM reserve. v2 stopped after 383.10 s, peak RSS 539.25 MiB, free RAM 0.7402 GiB. Reuse build/release; do not repeat configure. No new tests or release have passed. Source was edited during v2: the next incremental build must also rebuild shared mesh_asset after its optional mipmap-reader signature update. Default caller behavior remains unchanged.

# QuadWild-BiMDF integration — Smart Retopology in 3re

## 2026-10-22 evening: engine fixed, native workspace in 3re (Claude, continuing the Codex work)

Branch `claude/reptology-chatgpt-continue-6eb42d` = `codex/3re-quadwild` (incl. the Codex WIP commit
637bc7a) merged onto main ff5726b, plus the work below. `dist/3re` was NOT touched.

**Engine (QuadWild-BiMDF 0.0.2, built from source with MinGW 13.1).** Built without the self-imposed
1.5 GiB compiler cap (one job, ~2.8 GiB free; quadwild.cpp needed more than 1.5 GiB). Remaining build
fixes, all compiler-level, recorded in `prepare_quadwild_build.py` / `local-build-fixes.patch`:
- lp_solve: `INLINE` is `__inline` on `_WIN32`, a C99 inline definition never emitted out of line ->
  undefined `presolve_roundrhs`. One plain `extern` declaration in lp_presolve.c (a `-fgnu89-inline` attempt
  gave duplicate definitions of the header inlines and was reverted).
- quad_from_patches: `-O0` (from the 1.5 GiB cap) removed; with the build's -O1 the quadrangulation of a
  generated character takes ~90 s instead of > 6 min. The exe went from 16 MB to 3.6 MB.
- **The `0x80000003` crash is gone**: Codex's saved organic case (4,992 triangles) -> quadwild.exe exit 0
  (7 s), quad_from_patches.exe exit 0 (41 s), 3,078 faces, all quads. (The missing `return 0` was the cause.)
Runtime folder (exes + MinGW runtime DLLs + upstream `config/`):
`<maintainer workspace>/worktrees/3re-quadwild/.toolchain/quadwild-bimdf/0.0.2/runtime-fixed`.

**Why generated models failed in the engine, and the fix.** TRELLIS meshes are closed and consistently
oriented but carry hundreds of tiny tunnels (hoodie mascot: 244,844 triangles, genus 1,202) and internal
sheets. QuadWild sizes its own remesh from sphericity (volume vs area): KScale 0.0019 -> a ~1M-face remesh,
> 6 min; with its remesh off, patch tracing fails around the tunnels (45 non-disk patches, 193 unsolved
partitions). The worker therefore, for a mesh with more than 16 handles, builds the **outer closed shell**
(morphological closing at diag/200: outside flood-filled without coming within tau of the surface, dilated
surface as a Manifold level set, then moved back in by tau): hoodie 1,202 -> 69 handles, KScale 0.35.
Then a quadric simplification to 40k triangles (QuadWild re-samples to ~25k anyway). Results are always
measured against the full model, and for shell inputs also against the outer shell.

**Worker `pixel3d-retopo` (apps/backends/retopo_cli.cpp, retopo_engine.*)** - no Python, no Blender:
`remesh` (engine input, shell/simplify when needed, quadwild.exe step 2 + quad_from_patches.exe in a job
object with a memory cap and time limits, nonzero exit = rejected, quad-size calibration x2.5 plus one
correction pass when the count is > 35% off, result.obj + retopo.json with counts, open/non-manifold edges,
two-sided surface distance); `export` (new UV atlas, base colour from the closest point of the source
surface, GLB via the Auto Texture writer, OBJ with the quads, GLB read back and triangle-compared);
`inspect`.

**3re Retopology workspace** (replaces "Coming Soon"): Use Generated Model / Open GLB-OBJ, target polygons
(slider + box), Organic / Hard surface, Retopologize + Cancel with progress, Result card (before/after
counts, quad share, surface distance, open edges, "Shape kept" / "Check shape" chip), Save GLB (reports
the re-import), Show in Folder. Center: Codex's RetopoMeshEditor, now with "Retopology / Original model"
before/after (original textured), quad-edge wireframe (new `Scene::wire_edges`), vertex/edge/face
select + move/rotate/scale drag, Undo/Redo/Apply/Revert. Advanced: texture size, engine folder; Density
brush and Local re-retopology are marked **Coming Soon** (the pinned CLI has only a global size).
Engine lookup: `PIXEL3D_QUADWILD_DIR`, Settings value `retopo/engine`, `<app>/retopo`.
End-to-end check: `3re-studio --e2e-retopo MODEL --e2e-out J --e2e-shot PREFIX [--retopo-faces N]`.

**Still open:** packaging the engine into `dist/3re` (GPL-3.0: ship COPYING, the source + patch, build
notes beside the binaries; user decision), holes (open edges) QuadWild leaves where a patch fails, speed
(quadwild.exe is still -O1; a generated character takes ~5-6 min on this PC).

## Codex resume: 2026-10-22 (historical)

**Concurrent release change detected at final read-only verification:**
`dist/3re/RELEASE.json` is now **2.4.0-alpha**, release commit **4e71f80**,
packaged `2026-10-22T10:13:47`. It was 2.3.1-alpha/0d54637 at this turn's
initial read. This task did not write any dist/main files. Current launcher
SHA256 is `0aaa8e41118966717834275a04a5678d7ce409556943b158b2699ce20be1bda9`;
Studio SHA256 is `afe7d227a4b803b57a3bf689f42f15dacfbd7be24a9ff218c618340e580d8b61`.
Keep the isolated changes, but reconcile them against the exact 2.4.0 release
source before any application build or packaging. Do not deploy an old
0d54637-based application over this newer release. Main HEAD is independently
`f110596466ab89e317ad0bdc98036a062ed8498d`; do not assume main HEAD equals the
installed package's source. Previously recorded executable hashes below are
historical, not the current installed release.

The user expanded the goal to a native interactive Retopology/mesh editor:
vertex/edge/face transforms, Undo/Redo, Apply/Revert/Save, live original/result
wireframe and counts, and GLB export. Advanced features must remain explicitly
unavailable until implemented. Do not turn a density brush into a decorative
control: the pinned CLI currently exposes a global edge-size multiplier, not a
verified painted spatial-density input. No new solver is authorized.

The user explicitly requires fixing and validating the engine first. That gate
is still open; no new editing UI has been connected or advertised as working.
Existing `Viewport`, `PreviewBar`, picking and highlight APIs can be reused, but
the old editor is tied to drawing/voxel sessions. It is not a general imported
vertex/edge/face editor. Existing imported-preview code also has material and
scene restrictions; unsupported data must not be silently discarded.

The final workflow must not require users to operate Blender. The previous
Smart Quad worker uses background Blender for UV/PBR transfer; it is not proof
of a Blender-independent native implementation. Native atlas code already
exists in `apps/backends/texture_atlas.cpp` and `auto_texture_*`; assess reuse
after the engine gate, preserving all existing generation functionality.

### Actual build progress

Reused the configured source and the EXISTING CPU build policy from
`work/gate_a_memory_run.py`: one compilation job, 1536 MiB job-commit cap,
768 MiB physical reserve, 900-second bounded invocation. Neural inference
limits are untouched. No large downloads or neural/mesh generation occurred.

1. `quadwild-serial-build-v2`: ran **455.372 s**; FAIL at a real MinGW portability
   issue: `lp_lib.h:220` included Unix `dlfcn.h` because strict C++20 MinGW
   defines `_WIN32`, not the legacy `WIN32` macro used by that header. Compile
   reached QuadRetopology pattern code; completed object files were preserved.
   Peak job commit **1008.254 MiB**, sampled peak RSS **1036.258 MiB**;
   minimum available physical RAM **1.205 GiB**. This was NOT a memory failure.
2. Added a Windows-only INTERFACE `WIN32` compile definition to the existing
   lp_solve target, recorded in `prepare_quadwild_build.py` and
   `.toolchain/quadwild-bimdf/0.0.2/local-build-fixes.patch`. No numerical or
   topology algorithm was changed. The correction still needs compile proof
   at the previously failing file; do not report it runtime-verified.
3. `quadwild-serial-build-v3`: incremental configure succeeded and compilation
   resumed. **STOPPED by memory guard at 98.896 s**: free physical RAM reached
   **0.74285 GiB**, below the unchanged 768 MiB reserve. Peak job commit
   **1010.391 MiB**, sampled peak RSS **1037.367 MiB**. The terminated parent
   reports exit 0, but the monitor explicitly marks FAIL; this is not a
   successful build. No engine executable was completed.

Evidence directory:
`<maintainer workspace>/2026-10-14/files-pasted-by-the-user-trellis/outputs/validation/analysis/quadwild-integration`
contains separate stdout/stderr and JSON monitor reports for both attempts.
`work/quadwild_build_guard.py` resumes the existing build under the same guard,
with a new evidence name supplied as its sole argument. Never overwrite logs.
`work/quadwild_saved_case.py` prepares a bounded test on a new copy of a saved
OBJ, to run only after the corrected binaries have been built and staged in
`runtime-fixed` alongside their config and required runtimes.

**Next:** provide sufficient stable free RAM (about 3 GiB recommended), resume
the incremental serial build, validate engine exit and saved meshes, then
implement/verify native workspace editing and material-preserving export.
Do not restart the build tree, repeat inference, install a new release, or
claim a functioning Retopology editor from the passing adapter unit tests.

## Earlier setup and verification

Application baseline: 3re 2.3.1-alpha, commit
`0d546378ab0bf1b8361624a9b98a5f2ccd2f9717`, matching `dist/3re/RELEASE.json`.
Worktree: `<maintainer workspace>/worktrees/3re-quadwild`, branch
`codex/3re-quadwild`. The main checkout, installed release and other worktrees
have not been modified. The Studio Retopology placeholder is deliberately still
present until the engine and acceptance tests pass.

## Engine and provenance

- Upstream: https://github.com/cgg-bern/quadwild-bimdf
- Tag v0.0.2 resolves to `eef4fc390f071c4194196b0bea3b6fc243893adf`.
- Official Windows ZIP SHA256:
  `006c98d5e52d4992b70e929c5b95237efaa35aae8eb293bb0e6c3b933e569424`.
- Exact dependency commits, URLs and archive hashes are in
  `.toolchain/quadwild-bimdf/0.0.2/dependency-sources.json`.
- RWTH source URLs use the documented migrated `gitlab.vci.rwth-aachen.de:9000`
  host; TLS verification remains enabled. No dependency was substituted with
  an arbitrary latest revision. GMM 5.4.2 retains upstream's SHA224 check.
- CMake configure PASSED using the existing MinGW 13.1 toolchain.
  `SATSUMA_ENABLE_BLOSSOM5=OFF`, `QUADRETOPOLOGY_WITH_GUROBI=OFF`,
  `COMISO_ENABLE_GUROBI=OFF`, `CMAKE_DISABLE_FIND_PACKAGE_Gurobi=TRUE`.
  No Blossom V/Gurobi source or binary was downloaded.

The published Windows engine prepared the saved organic mesh successfully,
then `quad_from_patches.exe` exited with `0x80000003` after writing outputs.
Those outputs are diagnostic artifacts, not accepted results. Its pinned
`actual_main()` is missing a successful return; the local patch adds `return 0`
(also present in current upstream). This is a confirmed C++ defect and a
plausible crash cause, but the causal fix is UNVERIFIED until rebuilt and rerun.
Other source edits only make compiler definitions portable and use HTTPS for
the same hash-pinned GMM archive. No solver changes.

## Worker contract

`pipeline.py` accepts explicit `backend: "quadwild-bimdf"` and `remesher_exe`
pointing to `quadwild.exe`; sibling `quad_from_patches.exe` and upstream config
are required. The existing Qt QProcess/Windows Job containment will supervise
the worker tree. The new path is not yet exposed/enabled in the Studio UI.

The adapter processes a triangulated copy per existing component, checks Lemon
configuration, logs directly to disk, imposes stage timeouts, refuses stale
outputs and any nonzero engine exit. Density is an approximate target derived
from surface area and prepared average edge length, not a promise of final
face counts. Existing projection, topology/intersection, silhouette, material
bake and GLB round-trip gates remain in force. QuadWild failure never silently
falls back to triangle pairing. Open/material-boundary cases currently reject
explicitly pending validated preservation support. Legacy paths are retained.

## License / distribution gate

The upstream repository contains GPL version 3; source headers permit GPL-3.0
or later. Shipping its executables requires the applicable notices/license and
corresponding source (including the modifications, pinned dependencies and
build instructions) under GPL, using a compliant distribution mechanism. Do
not ship just a ZIP executable plus a GitHub link. Dependency notices also need
to accompany the package. QProcess separation is an engineering boundary, not
an automatic exemption from GPL obligations for a combined distribution.
No new engine is distributed or integrated into the installed app yet.

## Verification and remaining work

- 8 deterministic adapter tests PASS in 0.158 s (no models, Blender or remeshing).
  Initial test setup failed because sandbox TEMP permissions prevented fixture
  writes; rerun used a dedicated worktree temp directory, preserving that log.
- Published-engine probe: FAIL exit status; 16.10 s, peak job commit 50.05 MiB,
  sampled peak working set 71.52 MiB, minimum system free 4.15 GiB. Global GPU
  allocation stayed 356 MiB (other applications; not attributed to this CPU test).
- Corrected-engine compile: NOT STARTED; 2.5 GiB admission failed. Follow-up
  measurement was 0.466 GiB free physical RAM, also below the existing 768 MiB
  CPU-build reserve. No compiler was launched and no safety limit was lowered.
- Full organic/hard-surface quality comparison, UV/PBR bake, GUI QProcess,
  cancellation, wireframe, export/reimport: NOT RUN for this engine.
- No backup/swap of `dist/3re` has been attempted; both executable hashes still
  match the installed release. Create and verify the full backup before any
  future release replacement, and replace only after all acceptance gates pass.

Next: when memory is sufficient, build only `quad_from_patches` and `quadwild`
serially from the already-configured isolated build, under the existing bounded
supervisor. Verify normal exit first, then reuse the saved organic reference
and a clean saved hard-surface mesh for the existing full quality gates. Compare
against the saved AutoRemesher result before enabling or shipping the UI.
