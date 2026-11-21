# 3re

3re turns your own pixel-art drawings of an object — Front, Side and Top, optionally Back, Right and Bottom — into a validated, watertight, low-poly 3D model with the exact pixel colours as texture, and exports it as GLB, glTF or OBJ for game engines (presets for Godot, Unity and Roblox).

It never invents geometry or artwork silently: anything the drawings do not determine is reported, and the model is only called **PASS** when every check passes.

## Programs in this folder

| Program | Use |
|---|---|
| `Pixel3D.exe` | The desktop application. |
| `pixel3d-cli.exe` | Command line (batch conversion, scripts). See `README-CLI.txt`. |
| `Pixel3D-mcp.exe` | MCP server so AI agents can use 3re. See `README-MCP.md` and `skills/`. |
| `pixel3d-glbcheck.exe` | Independent check of an exported `.glb` file. |
| `LICENSES\`, `THIRD-PARTY-NOTICES.txt` | Licences of the included components. |

Nothing needs to be installed; keep the folder together.

## Project type

Every project has one of two types, always shown in the window title, on the start screen and in the
**Views** panel header (click it to switch; **Model ▸ Project Type** does the same):

| Type | Needs | Builds |
|---|---|---|
| **Low-Poly Studio** (default) | Front, or Front + Side (Top optional) | a rigged low-poly character (see *Low-Poly Studio* below) |
| **Visual Hull** | Front + Side + Top | the pixel-exact solid checked against every drawing (this quick start) |

New projects are Low-Poly Studio. Choose Visual Hull in **File ▸ New Project** (or switch) when you have
all three drawings. Switching keeps the drawings and is one undo step. A Visual Hull project without its
Top (or Side) never just fails: the Views panel, the hint and Reconstruct offer **Switch to Low-Poly Studio
(no Top needed)** or adding the missing drawing.

## Quick start (desktop application, Visual Hull)

1. Start `Pixel3D.exe` and switch the project type to **Visual Hull** (link under the title on the start screen). The start screen has three slots: drop your **Front**, **Side** and **Top** PNGs on them (or drop all three at once — file names containing `front`, `side`/`left`, `top`, `back`, `right`, `bottom` are sorted automatically; otherwise you are asked). From Aseprite, export each view as a PNG.
   - **Front**: seen from the front.
   - **Side**: seen from the **left** — the object's front points to the right of the image.
   - **Top**: seen from above — the object's front edge at the **bottom** of the image.
2. Press **Reconstruct** (F5). 3re builds the model, checks it against every drawing and opens the workspace.
3. The **status card** (right) says:
   - **PASS** — verified: silhouettes match every drawing and the 90°/180°/270° rotations, the model has real depth, is watertight, and the GLB reloads identically.
   - **NEEDS REVIEW** — the drawings do not determine something, for example *"Parts don't match between views"* (three legs drawn as 3 × 2 possible positions) or *"Hole or painted detail?"*. The undecided areas are amber in the model and on the drawings. Press **Review issues** (or click an amber area): the camera frames it and the right tool opens — choose which parts are real, or say how deep a hole goes. Decide only what you know, or add a Back, Right or Bottom view.
   - **FAIL** — the model needs fixes before export; the card lists what.
4. Look at it: **1–6** show each source camera with your drawing blended over the model, **0** returns to the 3D view; drag to orbit, right-drag to pan, scroll to zoom. The display buttons (top right of the view) toggle wireframe, lighting, grid, drawing overlay, issue highlights, estimated geometry (cyan) and turntable. Click a part to inspect it (**Faces** selects single faces).
5. **Validate** (F6) after a decision, then **Export** (Ctrl+E). A model that is not PASS is exported only after you confirm.
6. **File ▸ Save** keeps everything (drawings, decisions, checkpoints) in a `.p3d` project.

Undo/redo work for every change; **Edit ▸ Create Checkpoint** marks a state you can return to. Technical details (every check, metrics, internal ids) are in the collapsed *Advanced* sections, or everywhere with **View ▸ Show Advanced Details**; the operation log is in **View ▸ Panels ▸ Log** (Ctrl+L). **View ▸ Reset Layout** restores the default panels.

Every editing operation below is also available to AI agents through `Pixel3D-mcp.exe` (same engine commands; see `README-MCP.md`).

## Editing your model

The workspace is laid out as **Views** with the selected drawing (left), the **3D view** (centre), the **Inspector** (right) and **Checks & History** (bottom). The tools are in the tool bar; the options of the active tool float over the top-left of the 3D view.

| Tool | Key | What it does |
|---|---|---|
| Select | V | Click the model, or a pixel of the drawing on the left: the part — or a single face with **Faces**, or everything with **Object** — is selected in the model and outlined on the drawing. Ctrl+click adds parts (for Merge). |
| Depth | D | Paint on the drawing (left panel), or drag in the 3D view in Front/Side/Top (keys 1–6), to raise or lower the surface; size, depth and *selected part only* in the tool options. The model previews while you drag; releasing commits one undo step. Amber areas whose depth the drawings leave open are decided here too (click one). |
| Push/Pull | U | Drag a face of the model along its normal (up/down for a face turned to you). Whole pixels and *stay inside the drawings* are options. A selected face can also be pushed by a number in the Inspector. |
| Volume | B | Drag a rectangle on the drawing, then Add Volume, Erase Volume, Erase Through, Extrude, Push In, or assign it to a part. |
| Parts | P | The part list; with a part chosen in the tool options, paint which pixels belong to it. A part *defined* by its masks (Part ▸ New Part) is exactly what the masks allow — the clean way to say "these pixels are one leg". Only drawn pixels can be painted; the artwork never changes. Mask pixels the geometry does not cover are red. |
| Mirror | M | Mirror the selected part as a copy, or flip it (or the whole model) in place, across X, Y or Z. |

The **Part** menu and the Inspector's part page also rename, duplicate, transform, split (by plane or into pieces), merge, cut and delete parts.

Every edit is one step in **History** (double-click a step to return to it). The **live checks** beside it update after every edit: how many pixels of each drawing now differ, whether the model is closed and manifold, estimated and undecided volume, texture coverage, mask problems. An edit that makes the model stop matching a drawing is reported at once, and the model can then not be PASS until you undo it or update the drawing.

View options (View menu and the buttons over the 3D view): Orthographic (O), Textured (T), Wireframe (W), Pixel Preview (K), Normals (N), Grid (G), Snap to whole pixels (S), Issues and conflicts (C — undecided parts amber, and in views 1–6 the pixels that differ from the drawing: red only in the model, blue only drawn), Estimated geometry (E — parts and surfaces no drawing shows, cyan), and Camera ▸ Mouse Controls (3re, Blender or Maya style).

## Styles

**Model ▸ Project Settings ▸ Model style** (also in New Project):

| Style | Look |
|---|---|
| Crisp | The pixel-exact block model (default). This is the model you edit. |
| Soft | Rounded corners, smooth shading, soft shading bevel and ambient occlusion. |
| Low-poly | Chamfered corners, flat shading. |
| Clay | Like Soft, matte, with a wider shading bevel. |

Styles never change your silhouettes: corners are rounded only as far as every drawn pixel stays covered, so thin 1–2 px details stay sharp. The styled model is built when you validate (Reconstruct validates too); the 3D view then shows it, and it is the model that is checked and exported. GLB keeps its normal and occlusion maps and a material per colour family; glTF and OBJ get its shape and colours.

## Low-Poly Studio (rigged characters)

**Low-Poly Studio** (the default project type) builds a game-ready low-poly character instead of the
pixel-exact model: drop the **Front** drawing and, if you have it, the **Side** (no Top needed). Press
**Build character** on the start screen: 3re suggests typed parts (head, body, arms, legs, horns,
accessories) and shows the proposal before anything changes. On the Inspector's *Low-Poly Studio* page you
change any part's type, set the **triangle budget** (default 1000, a hard cap — nothing is decimated),
joint loops, look (cel + outline, cel, outline, flat) and body plan, then **Build**. The character has a
skeleton with the usual engine bone names; **Test pose** shows how its joints bend.

The status is honest: with Front only, every depth is estimated and the character stays NEEDS REVIEW
(you can accept the estimated depth for export — the report says so); a silhouette that the budget cannot
hold is NEEDS REVIEW too. **Export** writes a skinned GLB (and Godot shaders + Unity notes next to it);
a character that fails a check is never exported. Command line: `pixel3d-cli lowpoly front.png side.png
--out hero.glb`; agents: the `lowpoly_*` MCP tools.

## Only one drawing

**File ▸ New Single-Drawing Project** (or *Only one drawing?* on the start screen) turns one PNG into a solid: the silhouette, extruded to a depth you choose — Flat (1 px), Thin (¼ of the smaller side), Medium (½), Thick (= the smaller side) or a number — with optional per-part depths from part masks, then the editing tools for corrections. Everything the drawing does not show is marked **estimated**; the model needs review until you confirm a depth (Inspector ▸ Single drawing · depth), and it never claims the accuracy of Front + Side + Top.

## Aseprite workflow

Export your views from Aseprite as PNG (`front.png`, `side.png`, `top.png`, optional `back.png`, `right.png`, `bottom.png`) and drop them anywhere on the window — names decide the view; for other names 3re asks once and remembers. Pixels and palette are used exactly as exported. When you re-export a drawing, 3re notices and offers **Reload** in the status bar (File ▸ Reload Drawings from Disk, Ctrl+Shift+R; or tick *Reload Changed Drawings Automatically*): your edits, part masks and depth decisions are kept.

## Autosave and recovery

While you work, 3re autosaves to `%LOCALAPPDATA%\Pixel3D\Pixel3D\autosave`. If it ever closes unexpectedly, the next start offers to recover the work. A normal exit removes the autosave.

## Export

**File ▸ Export Model…** (Ctrl+E) asks where to write the model and for which engine:

| For | Format | Units | Facing |
|---|---|---|---|
| Generic glTF | GLB | metres | front faces +Z (glTF convention) |
| Godot 4 | GLB | metres | +Z (Godot's model front) |
| Unity | GLB | metres | +Z (glTF importers keep Unity's forward) |
| Roblox | OBJ | studs (1 stud = 0.28 m) | −Z (a part's Front surface) |

Every setting can be changed: format (**GLB** one file, **glTF** `.gltf` + `.bin` + `.png`, **OBJ** `.obj` + `.mtl` + `.png`), units (metres, centimetres, studs, or pixels = one unit per drawing pixel) and facing. The dialog shows the resulting size. **Unlit** (glTF only) shows the exact pixel colours without shading.

The size always comes from the project: by default 32 drawing pixels = 1 m (**Model ▸ Project Settings**). Origin: bottom centre of the model; +Y up. glTF and GLB sample the texture NEAREST, so pixels stay crisp. **OBJ cannot store that**: in your engine set the texture's filter to *Point* / *Nearest* (Roblox: import the `.obj` with its `.png` through the 3D Importer; Blender: Image Texture ▸ *Closest*).

Every exported file is read back by an independent reader and re-rendered before 3re reports it as written. A model that is not PASS is exported only after you confirm.

## If 3re closes unexpectedly

A crash report (`crash-<time>.txt`) is written to `%LOCALAPPDATA%\Pixel3D\crashes`; the next start mentions it and offers your autosaved work.

## Licences

3re includes third-party components; `THIRD-PARTY-NOTICES.txt` lists them and `LICENSES\` holds their licence texts. Qt is linked dynamically (the `Qt6*.dll` files) under the LGPL-3.0 and can be replaced.
