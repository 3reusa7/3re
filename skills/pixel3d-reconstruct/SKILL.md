---
name: pixel3d-reconstruct
description: Turn orthographic pixel-art drawings (Front/Side/Top images, optionally Back/Right/Bottom; PNG, JPEG, BMP, GIF, TGA, WebP, Aseprite .aseprite/.ase, PSD) into a validated, watertight, textured GLB with the Pixel3D MCP server. Use when asked to build, check or export a 3D model from pixel-art views with Pixel3D.
---

# Pixel3D reconstruction workflow

You drive the Pixel3D MCP server (`Pixel3D-mcp.exe`). The engine does all modelling; your job is to run the steps in order, read every result, look at renders when numbers are not enough, resolve only what can be determined, and report honestly.

Every tool returns the same structure: `success`, `errors`, `warnings`, `info`, `changed`, `changed_ids`, `revision`, `stats`, `metrics`, `status`, `validation`, `data`. Read `errors` and `warnings` after every call. `status` is one of `PASS`, `NEEDS_REVIEW`, `FAIL`, `NOT_VALIDATED`; `validation` lists `failed_critical_checks` and `blocking_conflicts`.

## Status rules (never bend these)

- `PASS` only comes from `validate`. Any edit afterwards makes the document `NOT_VALIDATED` again: re-validate.
- `NEEDS_REVIEW` means the drawings do not determine something. It is **not** a pass. Never describe it as success, and never make edits whose only purpose is to reach PASS.
- `FAIL` means a critical check failed (not watertight, silhouettes do not match, cardboard-thin, GLB does not reload, inside-out). Investigate; do not export unless the user explicitly accepts it.
- `export_glb` refuses NEEDS_REVIEW and FAIL. Pass `accept_status` only when the user has explicitly accepted that status, and tell them what is undetermined.

## Steps

1. **Inspect.** `inspect` (`what: project`) to see the session. Check the user's files exist and which roles they are. Conventions: Front = seen from the front; Side = seen from the LEFT (the object's front points to the image right); Top = seen from above with the front edge at the image bottom; Back/Right/Bottom are optional and remove ambiguity. `new_project` (choose `profile: strict` for clean synthetic art, `hand` for hand-drawn art; set `pixels_per_meter` if the user gave a scale), then `import_views`.
2. **Align.** `auto_align`. Real-world images are normalised first (see *Input normalisation* below): read every `input.*` message and tell the user what was done to their images. Then read the alignment warnings:
   - `view_flipped` / `flip_undetermined`: a mirrored or symmetric view. If a render later shows the wrong handedness, re-import that view with `flip_x`.
   - `extent_padded` (≤ 1 px) is fine. `extent_mismatch` and large `unexplained_px` mean the drawings disagree: report it to the user, do not invent geometry.
3. **Analyze.** `analyze_views`: pixel scale, crop, material families, Y-bands. Unexpected scale or a single family for multi-material art is worth mentioning.
4. **Reconstruct.** `reconstruct` (`mode: true_solid`; `voxel` only if the user wants blocky output). It checkpoints the previous state automatically.
5. **Inspect ambiguity.** `get_conflicts` and `get_inferred_geometry`. Blocking kinds: `ambiguous_correspondence` (several part combinations fit, e.g. three legs drawn as 3 × 2), `evidence_conflict`, `possible_recess` (an enclosed darker area that may be a hole/cavity), `extent_mismatch`, `edit.uncovers_atom` (a removal you asked for cannot be honoured). Info kinds (`inferred_grid`, `inferred_fill`, `inferred_continuation`, `inferred_flush`, `inferred_surface`, `user_depth`) are assumptions to report, not errors. Use `render_view` with `compare: true` to see where drawn and rendered pixels differ.
6. **Repair only when determinable.** A repair needs a source of truth:
   - an extra drawing (import Bottom/Back/Right — the best fix for leg ambiguity),
   - an explicit statement from the user (e.g. "it has three legs: front-left, front-right, back-centre", "the top is an open box 11 px deep", "that dark square is painted, not a hole"),
   - or unambiguous evidence visible in the supplied drawings.
   Then:
   - parts: `edit_part` with `keep` / `remove` candidate ids (footprints are Top-view boxes `[x0, z0, x1, z1]`; z grows toward the front). Check `data.refused_decisions` and warnings; a refused decision keeps the conflict blocking.
   - recess/cavity: `set_part_depth` with `conflict_index` (or `view` + `rect`/`polygon` in aligned view pixels) and `depth_from` (material starts this deep, measured from that view's side of the bounding box); `flush: true` when the user says it is painted.
   - Use `checkpoint` before experiments and `undo` / `restore_checkpoint` when a change makes things worse.
   If nothing determines the answer, leave it: the result stays NEEDS_REVIEW and you report exactly what the user must decide.
7. **Texture.** `project_textures`. Faces no view shows are `inferred_surface` (opposite projection, edge extend or flat palette — never new art). A warning that more than 10 % is flat palette is worth reporting.
8. **Validate.** `validate`. Report `status`, `validation.failed_critical_checks`, `validation.blocking_conflicts`, and the key metrics (projection IoU per view, rotation IoU at 90/180/270, thickness ratio, triangle count).
9. **360° inspect.** `render_turntable` (look at the contact sheet) and `render_view` at a few orbit angles (`yaw` 45/135/225/315). Look for missing parts, wrong handedness, cardboard-thin areas, texture smears. Numbers can pass while something looks wrong — say so if it does.
10. **Clean up.** If the mesh reports fragments, internal or duplicate faces, `cleanup_mesh`.
11. **Revalidate.** `validate` again after any change.
12. **Export.** `export_glb` (scale comes from the project), or `export_model` with the user's engine preset (`godot`, `unity`, `roblox`; OBJ for Roblox: tell the user to set point filtering). Then `save_project` so the user can continue later. Report: file paths, status, what was inferred, what remains undetermined (if accepted), and what the user could supply to resolve it.

## Input normalisation

`auto_align` turns real-world images into native pixel art before aligning: an opaque or white background is removed (flood fill from the border), semi-transparent pixels are snapped, anti-aliasing and JPEG noise colours are merged, each view's pixel scale is detected (exact grid or estimated), and oversize art is reduced to `target_size` (default 128 px on the longest side). Pixels are never interpolated: every pixel keeps a colour of the source. **Clean pixel art is never changed** (no `input.*` messages appear for it).

Messages to read and report:

| Code | Meaning | What to do |
|---|---|---|
| `input.background_removed` | colour and share removed per view | Mention it. |
| `input.background_leak_possible` (warning) | more than 60 % removed or the object touches the edge | Look at `render_view`; if part of the object vanished, `update_settings` with a lower `bg_tolerance`. |
| `input.background_not_found` (warning) | no uniform border colour | The whole image is the object: ask for an image with transparency or a plain background. |
| `input.alpha_snapped`, `input.colours_merged` | clean-up counts | Mention briefly. |
| `input.scale_per_view` (warning) | views drawn at different scales; factor per view | Mention it. |
| `input.view_rescaled` (warning) | a view was scaled up to `axis_tolerance` to match Front | Mention it; the drawings were not quite consistent. |
| `input.downscaled`, `input.fitted` | sizes before and after | Report the final model size. |
| `input.views_inconsistent` (error) | shared axes disagree beyond `axis_tolerance` (default 15 %) | Usually one image is not a straight orthographic view of the same object (e.g. a 3/4 or perspective "top" shot). Ask for a corrected view; do not raise the tolerance to force it through. |
| `input.turnaround_sheet` (error) | several figures side by side in one image | Ask for one image per view. |
| `input.no_subject` (error) | nothing left after removing the background | Ask for a better image or a lower `bg_tolerance`. |
| `view.too_large` (error) | still too large; `where.views` has per-view facts and `likely_cause` (`opaque_background`, `not_clean_pixel_art`, `different_scales`, `drawing_too_large`) | Follow the cause; the text says when normalisation is off (turn `normalize` on). Clean native-size art that is too large is left alone: ask for smaller art. |

Settings (`new_project`, `import_views`, `update_settings`; undoable, changes re-align): `normalize` (true/false), `target_size` (16-512), `bg_tolerance` (0-255), `axis_tolerance` (0-1). For clean pixel art you can switch it off with `normalize: false`; it is not needed, because clean art is left alone anyway. Normalised views are an approximation of the user's art: say so, and never call the result accurate beyond what the status says.

## Model styles

Use a style only when the user asks for a look (soft, low-poly, clay); `crisp` is the default and the exact model. Set it with `style` on `new_project` / `new_single_view_project` / `update_settings`, tune with `set_style`. Then `validate`: the polished model is built there, and it is the model that is validated and exported.

- A style never fixes a status: it is built from the crisp model and keeps every drawn pixel. Report the status of the styled model as it is.
- Read the `style.*` conflicts and tell the user: `style.applied` (what was rounded), `style.fallback` (parts that kept their crisp shape and why), `style.symmetry` (both sides built as mirror images because the drawings are pixel-exact mirrors: an inferred assumption, say so), `style.restored_pixels` (should be 0).
- Metal, glow and roughness per colour family come only from the user (`set_style` `materials`); never guess them.
- Export to `.gltf` or `.obj` drops the maps (`export.style_maps_dropped`): suggest GLB when the user wants the full look.
- Renders before `validate` show the crisp working model (`style.not_built`).

## Low-Poly Studio (rigged low-poly characters)

A separate project mode for game characters: `new_project` with `mode: lowpoly_studio`, then `import_views` with Front and, when the user has it, Side (Top optional; Front + Side is the normal case and needs no Top). The model is a skinned low-poly character built part by part, not the visual hull: `reconstruct`, `validate` and `export_glb` answer `state.wrong_mode` in this mode.

1. **Parts.** `lowpoly_suggest_parts` returns typed parts (head, torso, limb, accessory, cloth) proposed from the drawing. It changes nothing: show the proposal to the user, then `lowpoly_suggest_parts` with `apply: true` (`replace: true` over existing parts). The user can correct any type with `set_part_type`, or paint parts with `create_part` + `paint_part_mask` and type them. Never apply a suggestion the user has not seen. When the drawings are layered files (.aseprite, .ase, .psd), `suggest_layer_parts` proposes one part per visible layer, named after it (Low-Poly Studio: with a type guessed from the name); show the list, then `suggest_layer_parts` with `apply: true` and `accept` = the layers the user kept.
2. **Build.** `lowpoly_build` (optional `tri_budget` 500-5000, default 1000, a hard cap; `joint_loops`; `style` flat|cel|outline|cel_outline; `template`; `baked_cel` for engines without shaders; `hull`). It builds the shells, the skeleton and weights, the texture from the drawings, writes the GLB in memory and reloads it independently, and runs the low-poly gate. Read `data.lowpoly`: `triangles`/`budget`, `parts` (triangles and depth source per part), `checks`, `conflicts`, `silhouette_iou`, `surface` (shares of seen / inferred texture), `glb.ok`.
3. **Look.** `lowpoly_render` (yaws, `pose: true` for the rig's test pose, `contact_sheet`). Look at 0/90/180/270 and the pose: numbers can pass while a part looks wrong; say so.
4. **Export.** `lowpoly_export` writes the GLB built at step 2 (`engine_files: true` adds Godot toon/outline shaders and Unity notes).

Honesty rules of this mode:

- The status is the low-poly gate (`validation.mode` = `lowpoly_studio`). PASS needs every critical check (budget, closed shells, skin weights, adjacency, rigid invariance, GLB round trip) and no blocking conflict.
- Front only: every depth is INFERRED (`lowpoly.inferred_depth`, blocking). The user may set a depth (`depth`) or explicitly accept the inferred depth for export (`accept_inferred_depth: true`); the status stays NEEDS_REVIEW either way and the report says it was accepted. Set it only when the user says so.
- `lowpoly.silhouette_deviation` (blocking) means the rest-pose silhouette misses the drawing (IoU < 0.90): report it, suggest a higher budget or part corrections; never raise thresholds.
- Hidden sides are inferred texture (`lowpoly.inferred_surface`), mirrored partners are an assumption (`lowpoly.mirrored`), template joints are inferred (`lowpoly.inferred_joint`): report them.
- A FAIL is never exported. NEEDS_REVIEW is exported only with `accept_status: NEEDS_REVIEW` after the user accepts it, or with the user's `accept_inferred_depth`.

## Editing (only what the user asks for)

The editing tools change the model directly; the drawings stay the source of truth. Use them only for changes the user asked for or that the drawings determine, never to make checks pass.

- **Select:** `set_selection` with `view` + `u`/`v` (a drawing pixel) or `origin` + `direction` (a 3D ray); `get_selection` returns the part, the face and its pixels in every view.
- **Depth brush:** `paint_depth` with a `stroke` on a view (`mode: raise|lower`, `radius`, `strength`, `hardness`). One call = one undo step. `preview_edit` shows any edit's result without committing it.
- **Faces and volumes:** `push_pull` (a face or a view pixel, `distance` in pixels, clamped to the drawings' box unless `respect_bounds: false`), `extrude`, `add_volume`, `erase_volume`, `cut`, `mirror`, `transform_part`, `duplicate_part`, `split_part`, `merge_parts`, `delete_part`.
- **Part masks:** `create_part` (`define: true` when the masks define the part, e.g. "these pixels are one leg"), `paint_part_mask` (pixels, rect or stroke on opaque drawing pixels only), `sync_part_masks` to fill the other views, `rename_part`. Masks with `define` resolve ambiguous correspondence the same way `edit_part` does.
- After every edit read `warnings`: `edit.projection_changed` means the model no longer matches a drawing. Validation then reports `edit.projection_deviation` (blocking). Undo unless the user wants that change and will update the drawing.
- **Single view:** `new_single_view_project` with one PNG. All depth is INFERRED; the status stays `NEEDS_REVIEW` until the user picks a depth (`set_single_view_depth` preset or number). Never describe a single-view model as accurate from all sides.
- `reload_views` re-reads the drawings after the user edits them in Aseprite; masks, constraints and edits are kept.

## Coordinates

Region coordinates are pixels of the **aligned** view image (`inspect` `what: views` gives sizes): Front/Back are W × H, Side/Right D × H, Top/Bottom W × D, with u to the right and v down. Depth is measured from that view's camera side of the bounding box (Top: from the top surface downward).
