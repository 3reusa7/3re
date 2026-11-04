# 3re changelog (formerly Pixel3D)

The one active release is `dist\3re\` (start `3re.exe`), updated by `tools\package.ps1`
(see `docs\RELEASING.md`); earlier builds are archived in `retile-archive\`.

## 2.10.0-alpha - 2026-11-03 — Godot delivery check in the installed app; no mirrored delivery; Home first

- **Godot found by itself.** The agent's delivery check (a real Godot 4 headless import) now finds an installed Godot
  without setup: `--godot`, `THREERE_GODOT`, the path in **Settings > Delivery check (Godot)**, then PATH, Program
  Files, WinGet, Scoop, Steam, Desktop / Downloads / Documents and their Godot folders (console builds and the newest
  4.x first; a GUI build is replaced by the console build next to it). Settings shows where it was found and checks it.
- **Clear message when Godot is missing**: the chat does not start a run that cannot PASS; it says why and how to fix
  it (Arabic or English) and starts on "start" once Godot is there. The agent run shows "Delivery check: Godot …" or
  "Delivery check unavailable" in its pre-flight (run viewer), with the reason.
- **Release self-test**: `delivery_verification` fails the release when Godot 4 is not available; `--deep` imports a
  one-triangle GLB headless in Godot.
- **No mirrored or negative scale in any delivery (G0)**: every delivered GLB is audited — mirrored (negative
  determinant), non-uniform or sheared node transforms, non-rigid part pivots, winding against normals, tangents —
  as the agent's `transform_hygiene` criterion (FAIL blocks PASS). Developer tools: `pixel3d-asset-quality hygiene|bake` and an external
  Blender check (`tools/asset_quality/blender_check.py`, Blender not shipped).
- **Navigation**: Home is the first page and the page 3re opens on; the Agent directly under it (Ctrl+1 Home, Ctrl+2
  Agent, …).

First public release: open source under Apache-2.0.

- **Updates from GitHub Releases** (Settings → Updates): stable or alpha channel, a daily check that can be turned
  off and sends nothing about you, "what's new" in plain words before anything downloads. Every update is verified
  (minisign signature of the manifest with the built-in release key, SHA-256 of every file), installed after you
  confirm, self-tested, and rolled back automatically if the self-test fails; "Go back" returns to the previous
  version. Your projects, settings and downloaded models are never touched (docs/UPDATES.md).
- **AI models on first use** (Settings → AI models): public releases no longer bundle the TRELLIS.2 weights, SAM 2.1
  or the GPU engine libraries. 3re lists each one's size, source and licence and asks before downloading; every file
  is pinned and checked by SHA-256; interrupted downloads resume. Pixel Art Builder needs none of them.
- `3re.exe --self-test`: models that are not downloaded yet are a warning, a damaged one fails `--deep`.
- README rewritten in plain language for visitors (English and Arabic), with a clear "Help me build 3re" section; full status and roadmap moved to `docs/STATUS.md`.
- Apache-2.0 licence, NOTICE, rewritten third-party notices; README in English and Arabic; CI on every pull request
  (Windows build and tests, lint, licence and secret scans) and a release workflow with checksums and a signed manifest.


## 2.9.1-alpha - 2026-11-01 — chat: re-texture with more or corrected pictures; brain evaluation

- Local language-model brain measured (Qwen3-VL-4B Q4 on CPU via llama.cpp, pre-registered evaluation):
  15/30 requests, 0 picture labels, 1/10 conversations, 49.8 s per turn, 3.5 GB RAM — **not adopted**; available only
  as `3re-chat --brain http:HOST:PORT:MODEL` (`docs/research/agent-chat-brain.md` §7). `3re-chat --eval-brain` runs the
  evaluation for any brain.
- Guard: in a turn started by the agent (a run event) no brain may answer the agent's question for the person.
- Re-texture with more pictures (`retexture_with_pictures`): pictures attached to a finished model, or the side the
  person names for a picture the registration rejected, paint the same generation again (label corrected, not
  duplicated). Measured on the hooded character: a wrongly named side picture rejected → asked → corrected → texture
  PASS, polished: yes. A finishing agent process no longer makes the next run wait (re-measured for up to 15 s).

## 2.9.0-alpha - 2026-10-31 — Game-Ready Asset Agent, milestone 4 (conversation, "problem here", scheduling)

- **Agent first.** The Agent is the first workspace and the page 3re opens on; Ctrl+1..8 follow the rail
  (Ctrl+1 Agent, Ctrl+2 Home, …). Every other page and shortcut is still there.
- **Chat with the agent** (Agent page, left): type in Arabic or English, attach pictures (PNG, JPG, WebP, Aseprite,
  PSD) or a GLB with "+" or by dragging them in. The brain asks only for what a spec needs and is missing (input,
  character or prop, platform or budget, size), then writes the spec, has the agent validate it, and starts the
  agent; the agent's questions and its measured result come back into the chat. Every message, tool call and tool
  result is logged in the conversation's `run.jsonl`. The brain is a deterministic **rules** brain (offline, no
  model); every PASS / REVIEW / FAIL it repeats is quoted from the agent's report and marked as measured. A language
  model brain is studied (`docs/research/agent-chat-brain.md`) but not adopted: its download waits for approval.
- **Unordered pictures**: drop several pictures in any order; turnaround sheets are split; side labels come from file
  names (English and Arabic) or are left to the texture worker's registration ("auto"), which verifies or rejects each
  picture on the mesh. The new spec field `input.views` paints all of them from the first texture on.
- **Requested style** (`requested_style`): recorded in the spec and the report, never applied — style transfer is not
  implemented, and the chat says so. Rigging requests are answered plainly: 3re does not rig generated characters.
- A saved generation of the same picture (same SHA-256, in a 3re project) is reused instead of running TRELLIS again.
- **"There is a problem here"** (`pixel3d-regional-fix`): mark a region on one of 8 views (app: click a view and drag a
  circle); the tool maps it to faces and texels, says what painted it (provenance: picture, edge blend, TRELLIS
  native, multi-angle…) and whether the picture sees it, tries at most 3 fixes taking only that region from other
  textures of the same mesh, re-checks each with the texture gate and against the picture, and measures that no face
  outside the region changed. It proposes; the person decides.
- **Scheduling**: one agent run at a time; free RAM measured before a run starts (generation ~3.3 GiB, reuse ~2 GiB);
  otherwise the run waits with the measured reason. Agent runs are detached and survive closing the app; the
  conversation and the run viewer reattach at the next start (`--agent-conversation DIR` reopens one).
- **Hooded back picture (W2)**: the front-copy check is judged only where the back is mostly inferred; with a real back
  picture both the full and the reduced mesh are now PASS (evidence: `docs/agent/W2-HOODED-BACK.md`).
- `3re-chat.exe` runs the same conversation engine headless (scripted demos); `tools\agent\m4_demo.sh` has a pre-flight
  check (RAM, VRAM, apps to close, Godot, disk). Results: `docs\reports\agent-m4\README.md`.

## 2.8.0-alpha - 2026-10-31 — Game-Ready Asset Agent, milestone 3 (human decisions, run viewer)

- The agent asks instead of failing silently. When no reduction meets the triangle budget within the fidelity
  limits it measures the smallest triangle count that does (ladder, then bisection; every probe logged) and asks:
  deliver at the budget with the measured loss, or at the count the shape needs — with renders of the original,
  the budget result and the needed count. When the hidden side is TRELLIS's guess and the texture checks find a
  defect of a catalogued kind (face copied to the back, flat or darker back, foreign patches, ...), it asks for a
  picture of the back or a side and, given one, re-textures with the multi-angle path, checks again and shows
  before / after.
- Who answers: `3re-agent spec.json --ask` waits for an answer from the app's run viewer or from
  `3re-agent --answer RUN q1 needed`; `--answers file.json` gives answers beforehand (re-runnable demos). Without
  `--ask` (benchmarks, scripts) a question is written to the report as **PENDING_DECISION** and never answered;
  the run keeps its measured result. An answer names who decided and when; the criterion it decided becomes
  ACCEPTED with the measured numbers kept. Waiting for a person does not count against the time budget.
- New **Agent** workspace in the app: the timeline of a run — plan, every tool step (time, RAM, GPU), decisions
  with reasons, decimation and texture candidates side by side with thumbnails (chosen / rejected), the
  "what the shape needs" probes, questions with answer buttons, and the final deliverable with its Godot import
  numbers and every acceptance criterion. Live while the agent runs (it reads `run.jsonl`); "Run a spec…" starts the
  agent in ask mode. Screenshots: `--agent-run DIR [--agent-scroll question|final] [--agent-full]`.
- Demo set (`docs\agent\demo`, `tools\agent\m3_demo.sh`): a clean PASS, a recovery, a human decision with a back
  picture (and a budget decision), each re-runnable from its saved generation with the same result.
- Results: `docs\reports\agent-m3\README.md`. The M2 budgets and the M2 benchmark table are unchanged.

## 2.7.0-alpha - 2026-10-31 — Game-Ready Asset Agent, milestone 2

- Game fidelity instead of "no self-intersections" for game targets (`geometry.intersections`: `report` by default,
  `block` for printing): the reduced model must look like the original (surface distance, silhouettes from 9 views,
  normals, base colour on 8x8 blocks) and the file must be sound (no new holes, flips, non-manifold edges,
  slivers); crossings are counted and reported. Limits written down before the benchmark: `docs\agent\FIDELITY.md`.
  New worker `pixel3d-mesh-fidelity`.
- Decimation candidates: guarded QEM, then QEM without the crossing guard (`pixel3d-decimate-guarded --no-guard`);
  the first that meets budget and fidelity is delivered, every candidate is logged with its numbers. When none
  does, the report says how many triangles the shape needs.
- Foreign GLBs with several meshes: audited in world space and decimated per mesh (budget shared by area) with
  their own UVs and materials (`pixel3d-decimate-gltf`, meshoptimizer 0.24, MIT).
- Best-of-N TRELLIS texture: the engine (a fork of the pinned engine with identical outputs) saves a texture
  session; the agent repaints only the texture with other seeds (8–75 s each, ~0.8 GiB VRAM), re-textures each and
  keeps the one whose TRELLIS colours agree best with the picture on the seen side (`texture.candidates`, default 4).
- Hidden sides per material: each picture material's hidden part takes the colour that material has where the
  picture shows it (the anime-style character's back is no longer duller/browner than the front); on by default in the texture
  worker, `--no-material-match` turns it off.
- The delivered asset faces +Z with the picture's front (`front`: `picture` | `keep`), measured from the texture
  registration; budget presets (`preset`: mobile_character, mobile_prop, pc_character, pc_prop, hero_character).
- The agent records the GPU memory each step adds (NVML) next to RAM and time.
- Texture diagnosis of the benchmark (17 catalogued defects with images and causes): `docs\agent\TEXTURE-DIAGNOSIS.md`.
  Real-ESRGAN upscaling of the picture was measured and not adopted (halos / lost line work on close-ups).
- Results, M1 vs M2: `docs\reports\agent-m2\README.md`.
## 2.6.0-alpha - 2026-10-30 — Game-Ready Asset Agent (milestone 1)

- New: `app\3re-agent.exe spec.json` — an autonomous, deterministic agent (no LLM) that turns a picture, a saved
  3re run or a GLB into a Godot-ready asset: it checks the machine and every tool (SHA-256 pins, TRELLIS weights
  manifest), plans, runs the existing workers in Windows job objects (memory and time limits), audits the
  geometry, brings the mesh to the triangle budget with guarded decimation (never adds self-intersections),
  re-textures the reduced mesh from the picture + TRELLIS's texture, scales it to the spec's metres and stands it
  on the floor, then validates with the Khronos validator, an independent reload and a real Godot headless import.
  Every step goes to `run.jsonl`; every recovery decision is logged with its reason; `report.json` / `report.md`
  list the numbers, the SHA-256 of the deliverable and what a person must still check. Nothing unmeasured is
  ever PASS (UNVERIFIED instead); an existing output folder is refused.
- New worker `pixel3d-decimate-guarded` (the shipped guarded QEM on a GLB, with surface deviation); an opt-in
  "touching allowed" pass for budgets (default cleanup unchanged).
- The delivered GLB is scaled and grounded in its vertices (one node), so it opens in Smart Retopology and
  3re's importer: agent first (triangle budget), then quads.
- The Khronos glTF validator (Apache-2.0) now ships in `app\`; `--e2e-generate` takes `--e2e-seed`.
- Results (7 regression assets + a fresh generation + a foreign GLB): `docs\reports\agent-mvp\README.md`;
  audit and baseline: `docs\agent\AUDIT.md`. 2.6.0 also contains Smart Retopology (2.5.0).
## 2.5.0-alpha - 2026-10-26 — Smart Retopology (Experimental)

- The Retopology page is real now (it was Coming Soon): use the model from Image to 3D or open a GLB/OBJ,
  choose the polygon target and Organic / Hard surface, press Retopologize. QuadWild-BiMDF builds clean quads
  on the CPU (no Blender, nothing to install); progress and Cancel in the page.
- Before/after in the view (new quad mesh or the original with its texture), quad wireframe, counts, quad
  share and the distance to the original surface. Every result is checked: open edges, broken topology and,
  new, faces crossing each other (3re's geometry audit). A result that crosses itself is shown as
  "Unsafe: self-intersections" with the reason: where a part is thinner than a quad (a sword blade) the
  engine's single quad size makes faces cut through; more polygons reduce it.
- Edit the result by hand: select vertices, edges or faces, move / rotate / scale by dragging, Undo / Redo,
  Apply / Revert.
- Save GLB: new UVs, the original's colours (and metal/roughness) moved onto the quads, read back and
  compared before success is shown; an OBJ with the quads is saved beside it.
  After saving, the view switches to "Saved with texture": the file itself, read back, with its texture (and
  the quads when the wireframe is on); editing again removes that view until the next save.
- Density brush and local re-retopology: Coming Soon.
- The engine (GPL-3.0) is in app\retopo, its licence in LICENSES\ and its complete source in
  Retopology-Sources\ (tools\smartquad\QUADWILD.md).

## 2.4.0-alpha - 2026-10-22 — sharper texture for every model, pictures from several angles

- No more brush-stroke smears where the picture lands on steep surfaces (shoulders, the sides of arms and
  trousers): the picture fades out there and TRELLIS's own texture of the object takes over.
- Faces, hands and emblems are sharper: characters get a 4096 texture when the computer has the memory, and
  the parts the picture shows in detail get up to twice the texture resolution; picture texels are taken
  exactly, with a sharper (ring-free) sampler.
- New: pictures from several angles. On the Import step add up to five more pictures of the same subject
  (back, sides, three-quarter views) or split a turnaround sheet; set or correct each picture's angle. The shape
  still comes from the front picture; every other picture is matched to the model (outline fit shown) and
  rejected with the reason when it does not fit or its angle label is wrong. Each surface point takes the
  pictures that see it best, colour and exposure matched, blended without seams; TRELLIS paints only what no
  picture shows. The checks count a back shown by a real picture as seen; the report says which picture
  paints how much of the surface. Projects store the pictures and their angles (older projects open unchanged).
- New: Settings → Saved models → "Re-texture all saved models" (also `3re.exe --retexture-all`): background,
  cancellable, writes `textured-v2.glb` next to each run (never overwriting) and the project opens with it.
- Opening a project shows its latest result.
- Orange streaks of the collar no longer run into black back hair: thin stripes of one material inside
  another take the surrounding material.
- Report: `docs\reports\multi-angle\README.md`.

## 2.3.2-alpha - 2026-10-22 — new 3re mark

- New logo: the isometric "three views" cube (Front, Side and Top faces drawn apart) whose Front face
  dissolves into pixels: its corner steps down like a pixel stair and three pixel squares break away,
  a picture or pixel drawing turning into a 3D solid. No background tile: the symbol is the icon, in a
  slate-blue ramp that holds on dark and light taskbars.
- Installed in the window / exe icon (small-size cut for 16-32 px), the splash, the Home page, About and the
  pixel-art start screen. The start screen loaded `:/pixel3d.png`, which no resource file contained, so its
  logo was blank.
- `branding/` holds the SVG masters (colour, light-surface, one-colour, small cuts, app icon, lockups), PNG
  renders and the usage guide; `tools/make_3re_icon.py` rebuilds all of it (and the application's PNG/ICO).

## 2.3.1-alpha - 2026-10-21 — no foreign tints from an ambiguous viewpoint

- When the picture's viewpoint is ambiguous (the model's outline fits more than one view), colour
  calibration could turn colour noise into a foreign tint: in the end-to-end check a black motorbike came out
  green. If no colour map beats TRELLIS's own colours on held-out points, only lightness is corrected now.
  The seven regression textures are unchanged (their calibration beats the uncalibrated colours by
  0.011-0.078 ΔE).

## 2.3.0-alpha - 2026-10-21 — Texture v2

- Hidden sides keep to the picture's materials: TRELLIS colours that belong to another part (orange
  streaks in the anime-style character's black back hair, pale blotches and a painted face on the vampire's cloak) are replaced
  by the surrounding material, keeping only TRELLIS's light/dark detail; unseen parts in colours the picture
  never shows are still painted as TRELLIS made them.
- Glass: objects TRELLIS makes translucent (median opacity < 0.8) export as transparent glass
  (alphaMode BLEND, opacity from TRELLIS's own texture); the 3re preview shows them opaque.
- Sharper textures: charts are packed by their outlines (texture space used 28-46 % -> 38-57 %).
- Preview: painted and photographic textures are filtered smoothly with mipmaps (no jagged close-ups);
  pixel art stays crisp.
- Regression set (7 assets, same geometry): vampire and glass bowl REVIEW -> PASS, none worse;
  `textured-v2.glb` beside each run. Generated back views (MV-Adapter) were measured to fit 4 GB but never
  won the hidden-side check, so they are not used. Report: `docs\reports\texture-v2\README.md`.

## 2.2.1-alpha - 2026-10-21 — stricter hidden-side check

- New "patch support" check: the back and back three-quarters must be made of patterns and colours
  the picture itself contains. Catches what 2.2.0 passed: a face and blotches TRELLIS painted on the
  vampire's cloak (11.5 %) and dark smudges on the glass bowl (25.6 %); good backs measure 0–5 %.
  REVIEW at 8 % or more; measure only, the texture is never changed.
- Report: `docs\reports\hybrid-texture\README.md` (Patch support section).

## 2.2.0-alpha - 2026-10-20 — Hybrid Texture

- TRELLIS paints the back, the picture owns the front: when the TRELLIS texture weights are
  installed (now part of the release), the same generation also runs TRELLIS's texture stage
  (+0.5–2 min, 1.1–1.2 GB VRAM). The geometry is byte-identical to a geometry-only run and goes
  through GeometrySafe and the audit unchanged.
- Hidden sides use TRELLIS's own texture of the whole object, colour-calibrated to the picture
  (it fixes the dark "Generated PBR" colours); the picture keeps everything it shows; pixel art
  keeps the picture's palette. No more camouflage patches spread from the visible edges.
- New hidden-side quality gate (back, sides, back three-quarters): textures extrapolated from
  the picture's edges FAIL; a front copied onto the back (Janus), flat patches and grey backs are
  flagged REVIEW. Report: `docs\reports\hybrid-texture\README.md`.
- If the textured run runs out of memory (very dense objects), it is retried geometry only.

## 2.1.0-alpha - 2026-10-20 — Auto Texture

- Every Image-to-3D result is textured automatically after GeometrySafe and the geometry audit
  (`pixel3d-auto-texture.exe`, CPU only, own memory/time limits): the picture's viewpoint is found
  from the outline (objects TRELLIS turned are now textured on the right faces), the picture is
  projected only onto surfaces it really shows, hidden sides are filled from nearby visible colour
  regions (an estimate, never a mirrored front), seams and gutters are clean.
- Quality checks for every texture (UV coverage/stretch/overlap, seams, bleeding, front-to-back
  duplication, colour error against the picture, darkness); the best verified result is kept, an
  embedded Generated PBR material only if it scores better. Report beside each result
  (`textured.glb.texture.json`); the Preview says how much of the surface the picture shows.
- Saved results: "Re-texture this mesh from the picture" (no regeneration), or the worker on the
  command line for batch retexturing.
- Report: `docs\reports\auto-texture\README.md`.

## 2.0.0-alpha - 2026-10-20 — 3re

- Pixel3D becomes **3re**: one portable application (`dist\3re\3re.exe`, `app\`, `models\`),
  one development line (`main`), one package command with self-test and atomic swap.
- New studio window: Home, Image to 3D, Smart Retopology (Coming Soon), Export, Rigging
  (Low-Poly Studio rig, Experimental), Animation (Coming Soon), Settings; calm dark design system.
- Image to 3D is a guided Import → Segment → Generate → Preview workflow over the existing
  TRELLIS.2 + GeometrySafe backend (same 4 GB safeguards and honest REVIEW/UNSAFE labels),
  with projects (`project.3re`) and plain-language results.
- Segmentation: regions you mark and label (SAM 2.1-tiny on the CPU, colour regions for pixel
  art), brush/eraser, merge/split, undo/redo, saved in the project; included regions become
  the generation input. Not semantic part detection; no per-part 3D objects.
- Release self-test (`3re.exe --self-test --deep`) and end-to-end check (`--e2e-generate`).
- Report: `docs\reports\3re\README.md`.

1.2.0-alpha added **Low-Poly Studio** (`lowpoly_studio`): game-ready, rigged low-poly characters
(500–1500 triangles) from Front + optional Side (or Front only, with inferred depth), with a skinned glTF
export (project format 5); WebP, Aseprite and PSD drawings (layers as suggested parts). Reports:
`docs\reports\1.2.0\`.

## 1.2.4-alpha - 2026-10-14

- feat(gui): new projects are Low-Poly Studio; Visual Hull without Top never dead-ends
- docs(reports): 1.2 P3 final comparison with the 1.2.3-alpha package; handoff: P3 done

## 1.2.3-alpha - 2026-10-14

- docs(handoff): N-4b done; next 1.2.3-alpha; tools: 1.2.3 rows
- fix(lowpoly): a bridged part moves only its front back, keeping its Side silhouette (N-4b)

## 1.2.2-alpha - 2026-10-14

- docs(handoff): N-4 done; next 1.2.2-alpha; tools: 1.2.2 rows
- fix(lowpoly): a part bridged over another part's pixels stays behind it (N-4)
- tools(p3): 1.2.1-alpha rows for the comparison

## 1.2.1-alpha - 2026-10-14

- docs(handoff): P3 items 4, 5, N done; next 1.2.1-alpha
- fix(lowpoly): suggested Side masks keep every main colour of a part; one part = the whole Side (N-3)
- fix(lowpoly): branches hang where they are near their parent; enclosed parts get no extension (N-1); U-5 verified
- docs(reports): 1.2.0 P3 final comparison with the 1.2.0-alpha package — rows, per part, sheets, Godot 4.7, test logs
- wip(p3): LP-2 delete_part in Low-Poly projects, LP-4 glbcheck skinned node rule, U-5 multi-piece part shape (not yet built or tested)
- tools(p3): before/after tables from the measured P2 and P3 JSON
- docs(handoff): P3 delivery A in progress

## 1.2.0-alpha - 2026-10-14

- docs(changelog): fix the reports path
- chore(release): 1.2.0 changelog intro, ignore samples/, P3 rows for the comparison tools
- fix(lowpoly): part suggestion keeps drawn limbs apart and paint bands together (LP-5, LP-6)
- fix(lowpoly): front only, parts drawn on another part come forward (LP-3)
- fix(lowpoly): limb chains link only end to end (LP-1)
- docs(handoff): P3 handoff file
- docs(reports): 1.2.0 P2 report — comparison, causes, per-part results, glb_roundtrip status, formats, test logs
- test(input): grayscale Aseprite fixture (Aseprite export as truth); note the formats without an independent fixture
- feat(input): WebP, Aseprite and PSD sources; layers as suggested parts; open drawings as a Low-Poly project
- build(third_party): libwebp 1.6.0, decode only, GPG-verified release
- docs(reports): 1.2.0 P2 baseline — 1.1.0 binaries, P1 package rows, sheets, per-part results, Godot import
- test(p2): fixture F (input B, 3-view hooded reference), P2 defect reproductions, diagnosis tools
- docs(reports): 1.2.0 P1 integration build — test logs, package checksums, sample exports
- fix(gui): Project Settings switch the project type (Low-Poly Studio on/off)
- feat(lowpoly): Low-Poly Studio in Pixel3D.exe, docs and the package
- feat(lowpoly): Low-Poly Studio in the project (format 5), Session, MCP and CLI
- docs(lowpoly): v2 review sheets (automatic front-only depth, ellipsoid heads, stop thresholds)
- feat(lowpoly): allocator stop thresholds by part type + smoothed measured depth; suggest_parts (colour regions + shape split); normal guards
- feat(lowpoly): heads as regular ellipsoid lathes, crease-aware normals (40° along the profile), front-only depth review sheet
- feat(lowpoly): automatic front-only depth from part type (depth/width minimums, per-part override), accept_inferred_depth export decision
- docs(lowpoly): review contact sheets at 500/1000/1500; dev tool writes GLB + validation
- feat(lowpoly): skinned glTF export (tinygltf round trip + Khronos validator clean), low-poly gate and report; fix zero-width tip rings
- feat(lowpoly): UV island packing, per-texel bake (one view per texel, contour ink, inferred rules, baked cel), build orchestration, inverted hull
- feat(lowpoly): mesh assembly with UV seams and skin weights, style preview renderer, rig (test pose, LBS, rig checks)
- feat(lowpoly): layout (templates, chains, branches, skeleton, mirrored partners) and budget allocator
- feat(lowpoly): lofted closed tubes with exact silhouette extremes, caps, untangling
- feat(lowpoly): spines, cross-section profile, DT inflation with depth-cue hook, station reducer
- feat(lowpoly): spec, plan, fixture E (stylised characters), view alignment, validator fetch script

## 1.1.0-alpha - 2026-10-13

First alpha: everything finished since 1.0.0 in one app.

- **Real-world images** (input normalisation): opaque or white backgrounds, anti-aliasing, JPEG noise,
  AI "pixel style" art and oversize drawings are turned into clean pixel art before alignment; every
  change is listed. Clean pixel art is never changed. New source formats: JPEG, BMP, GIF (first frame), TGA.
- **Polished styles**: `crisp` (default, unchanged), `soft`, `lowpoly`, `clay`. Rounded volumes inside the
  drawings' hull (every drawn pixel stays covered), smooth shading, shading-bevel normal map, ambient
  occlusion, mirror symmetry for pixel-exact symmetric drawings, PBR material per colour family. The polished
  model is the one that is validated and exported.
  - Pixel3D.exe: Style in the project settings. CLI: `--style`. MCP: `style` on `new_project`,
    `new_single_view_project` and `update_settings`, and the new `set_style` tool (tuning and per-family
    materials). `render_view` / `render_turntable` take `style: beauty` for presentation renders.
- CLI: `--single-view [flat|thin|medium|thick|<depth>]` builds from one drawing.
- The version is shown in the window title, in Help ▸ About and by `--version`.
- Project files are format 4. Projects from 1.0.0 (formats 1 and 2) open unchanged and give the same model.
- Release workflow: `tools\release.ps1`, `docs\RELEASING.md`.

Known limits: see "Known issues" in `docs\integration-notes.md`.

## 1.0.0 - 2026-10-07

- Front/Side/Top pixel art to a validated, watertight true-solid model; editor (depth, push/pull, volume,
  parts, mirror), single-drawing mode, Aseprite reload.
- Export GLB, glTF, OBJ with presets for Godot, Unity and Roblox.
- Pixel3D.exe (desktop app), pixel3d-cli, Pixel3D-mcp (MCP server for Claude).
