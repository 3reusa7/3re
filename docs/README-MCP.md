# Pixel3D MCP server

`Pixel3D-mcp.exe` lets any MCP-compatible AI agent build 3D models with Pixel3D. It is the same engine as the command line and the desktop app: the agent calls engine operations (import, align, reconstruct, validate, render, export), it never clicks the GUI. Everything runs locally; reconstruction needs no network and no AI vendor.

## Connect a client

The server speaks MCP over stdio (JSON-RPC 2.0, one message per line). Register it in your client's MCP configuration, for example:

```json
{
  "mcpServers": {
    "pixel3d": {
      "command": "C:/Tools/Pixel3D/Pixel3D-mcp.exe",
      "args": ["--root", "C:/Users/me/PixelArt"]
    }
  }
}
```

Options:

| Option | Effect |
|---|---|
| `--root DIR` | Only allow file paths inside `DIR` (repeatable). Without it, any path the client names is allowed. |
| `--log FILE` | Also append the server log to `FILE` (the log always goes to stderr, never to stdout). |
| `--quiet` | Log warnings and errors only. |
| `--version`, `--help` | Print and exit. |

Relative paths are resolved against the server's working directory.

## What the agent gets

- **Tools** (49). Reconstruction (27): `new_project`, `open_project`, `save_project`, `update_settings`, `import_views`, `auto_align`, `analyze_views`, `segment_parts`, `match_parts`, `reconstruct`, `get_conflicts`, `get_inferred_geometry`, `set_part_depth`, `edit_part`, `cleanup_mesh`, `project_textures`, `assign_texture`, `validate`, `render_view`, `render_turntable`, `undo`, `redo`, `checkpoint`, `restore_checkpoint`, `export_glb`, `export_model`, `inspect`.
  Editing (22): `paint_depth`, `push_pull`, `add_volume`, `erase_volume`, `extrude`, `cut`, `mirror`, `transform_part`, `duplicate_part`, `split_part`, `merge_parts`, `delete_part`, `create_part`, `rename_part`, `paint_part_mask`, `sync_part_masks`, `set_selection`, `get_selection`, `preview_edit`, `new_single_view_project`, `set_single_view_depth`, `reload_views`.
- **Resources**: `pixel3d://project`, `pixel3d://views`, `pixel3d://parts`, `pixel3d://mesh/stats`, `pixel3d://conflicts`, `pixel3d://diagnostics`, `pixel3d://validation`, `pixel3d://edits`, `pixel3d://live`, `pixel3d://selection`, and the view images `pixel3d://views/{role}/aligned.png` / `original.png`.
- **Prompt** `reconstruct_from_views`: the full workflow. The same text ships as an agent skill in `skills/pixel3d-reconstruct/SKILL.md`.

Every tool returns the engine's result structure: `success`, `errors`, `warnings`, `info`, `changed`, `changed_ids`, `revision`, `stats`, `metrics`, `status`, `validation`, `data`. Render tools also return PNG images inline, so the agent can look at the model.

## Input normalisation

Views may be PNG, JPEG, BMP, GIF (first frame), TGA, WebP (first frame), Aseprite (`.aseprite`/`.ase`: the first frame, flattened as Aseprite exports it) or PSD (the composite stored in the file). The layers of Aseprite and PSD drawings can become parts: `suggest_layer_parts` lists every visible raster layer as a suggested part named after the layer (hidden layers are listed, never created; text, smart-object, adjustment and fill layers are reported, never a failed import); `apply: true` with `accept` creates the layers the user chose. At `auto_align` the server normalises real-world images before aligning them: it removes an opaque or white background (flood fill from the border, `bg_tolerance`), snaps semi-transparent pixels, merges anti-aliasing and compression-noise colours, detects each view's pixel scale (exact grid or estimated), evens out small differences between views (up to `axis_tolerance`, default 15 %) and reduces oversize art so the longest side is `target_size` (default 128). Every pixel keeps a colour of the source (each block's dominant colour; no interpolation). Clean pixel art is never changed.

Every change is reported as an `input.*` message with its numbers (`input.background_removed`, `input.background_leak_possible`, `input.colours_merged`, `input.scale_per_view`, `input.view_rescaled`, `input.downscaled`, `input.fitted`, ...). Inputs that cannot be handled are refused with a specific error: `input.views_inconsistent` (shared axes disagree, e.g. a perspective shot given as Top), `input.turnaround_sheet` (several figures in one image), `input.no_subject`. `view.too_large` carries per-view facts and a `likely_cause`. `inspect` (`what: views`) shows what was done to each view under `normalization`.

Settings: `normalize`, `target_size`, `bg_tolerance`, `axis_tolerance` on `new_project`, `import_views` and `update_settings` (undoable; they are saved in the project). `normalize: false` uses every image exactly as given.

## Model styles

`style` on `new_project`, `new_single_view_project` and `update_settings`: `crisp` (default, the pixel-exact model), `soft`, `lowpoly`, `clay`. `set_style` changes the tuning (`radius`, `exponent`, `bevel`, `smooth_angle`, `shade_radius`, `ao`, `symmetry`, `reset`) and per-family PBR `materials` (`family`, `roughness`, `metallic`, `emissive`; metal and glow are never guessed). Rounding is clamped per corner so every drawn pixel stays covered; no tolerance is loosened.

The polished model is built at `validate` from the crisp model (which stays the one the editing tools change). It is what `validate` checks, what `export_model` writes (GLB: UV1, tangents, normal and occlusion maps, one material per family; `.gltf`/`.obj`: geometry and colours, warning `export.style_maps_dropped`) and what the renders show. Before `validate`, renders show the crisp working model and say so (`style.not_built`). The validation report has a `style` section; conflicts `style.applied`, `style.fallback`, `style.symmetry` (pixel-exact mirror drawings built as mirror images) and `style.restored_pixels` describe what was done. `render_view` / `render_turntable` `style: beauty` is a separate presentation render.

## Low-Poly Studio

`new_project` `mode: lowpoly_studio` starts a character project (Front + optional Side/Top). Tools:
`lowpoly_suggest_parts` (proposal; `apply: true` creates the parts), `suggest_layer_parts` (parts from the drawing's layers), `set_part_type`, `set_lowpoly_settings`,
`lowpoly_build` (budget is a hard cap; GLB built and reloaded in memory; low-poly gate), `lowpoly_render`
(yaws, test pose, contact sheet), `lowpoly_export` (PASS, or NEEDS_REVIEW the user accepted; FAIL never),
`inspect` `what: lowpoly`. The visual-hull tools answer `state.wrong_mode` in this mode.

## Honesty rules the server enforces

- `status` is `PASS`, `NEEDS_REVIEW`, `FAIL` or `NOT_VALIDATED` in every result; `validation` says why (failed critical checks, blocking conflicts).
- Any edit makes the document `NOT_VALIDATED` until `validate` runs again.
- `export_glb` validates first and refuses `NEEDS_REVIEW` or `FAIL` unless the agent passes `accept_status` explicitly; the result then carries a warning. The export scale is the project's `pixels_per_meter`.
- `export_model` writes GLB, glTF (`.gltf` + `.bin` + `.png`) or OBJ (`.obj` + `.mtl` + `.png`) for an engine preset (`generic`, `godot`, `unity`: metres, front +Z; `roblox`: OBJ in studs, front −Z); `units`, `front` and `unlit` override the preset. Same `accept_status` rule; every file is reloaded by an independent reader and re-rendered before it is reported as written, and companion files are written all together or not at all.
- Part decisions the drawings cannot honour are refused and reported; they keep the status at `NEEDS_REVIEW`.
- Every document change is a transaction (all-or-nothing) and can be undone. `new_project` and `open_project` start a fresh history.
- Editing tools call exactly the same engine commands as the desktop application. Every edit returns live checks (projection mismatch per view, watertight, non-manifold, texture) and an `edit.projection_changed` warning when it moves the silhouette away from a drawing; `validate` then blocks PASS with `edit.projection_deviation` until the edit is undone or the drawings are updated.
- Single-view projects (`new_single_view_project`) label all depth as inferred, stay `NEEDS_REVIEW` until a depth is chosen (`set_single_view_depth`: `flat`, `thin`, `medium`, `thick` or a number), and never claim multi-view accuracy.

## Workflow

inspect → align → analyze → reconstruct → inspect ambiguity → repair only when determinable → texture → validate → 360° inspect → clean up → revalidate → export. See `skills/pixel3d-reconstruct/SKILL.md`.
