Pixel3D — pixel art (Front/Side/Top) to a validated TRUE SOLID 3D model
=======================================================================
(pixel3d-cli --version prints the release, e.g. 1.1.0-alpha)

pixel3d-cli.exe needs nothing else installed: double-click is not useful for a
command-line tool, so run it from a terminal (cmd or PowerShell).

Reconstruct
-----------
  pixel3d-cli reconstruct --front front.png --side side.png --top top.png ^
              --out chair.glb --report report.json --renders renders --turntable ^
              --project chair.p3d

  Optional: --back, --right, --bottom   (more views = fewer ambiguities)
            --mode voxel                (intentional blocky output)
            --profile strict|hand       (silhouette tolerance; hand is default)
            --pixels-per-meter 32       (export scale; saved in the project)

Real-world images
-----------------
  Views may be PNG, JPEG, BMP, GIF (first frame) or TGA. Images with an opaque
  or white background, anti-aliasing, JPEG noise, AI "pixel style" art or a
  size beyond the limits are normalised first; every change is printed as a
  note (input.background_removed, input.downscaled, ...). Clean pixel art is
  never changed.
            --normalize on|off          (default on; off = use the images as given)
            --target-size 128           (longest side after normalisation, 16-512)
            --bg-tolerance 24           (background colour tolerance, 0-255)
            --axis-tolerance 0.15       (views may disagree this much, 0-1)
  Refused with a clear error: views of different objects or a perspective shot
  (input.views_inconsistent), several figures in one image
  (input.turnaround_sheet), an image that is only background (input.no_subject).

Model style
-----------
            --style crisp|soft|lowpoly|clay
  crisp (default) is the pixel-exact block model. soft, lowpoly and clay round
  the volumes inside your drawings (every drawn pixel stays covered), add
  smooth shading, a shading-bevel normal map and ambient occlusion (soft,
  clay). The styled model is the one that is validated and exported; the
  summary lists what the style did ("style soft: ..."). GLB carries its maps
  and a PBR material per colour family; .gltf and .obj carry its geometry and
  colours only (a warning says so).

One drawing
-----------
  pixel3d-cli reconstruct --front hero.png --single-view thick --out hero.glb

Low-Poly Studio (rigged low-poly character from Front and optional Side/Top):

  pixel3d-cli lowpoly front.png side.png --out hero.glb [--budget 1000] [--style cel_outline]
            [--joint-loops 2] [--template auto] [--baked-cel] [--engine-files] [--renders DIR]
            [--report report.json] [--project hero.p3d] [--parts typed_parts.p3d]
            [--accept-status NEEDS_REVIEW] [--accept-inferred-depth] [--depth N]

  Typed parts are suggested from the drawing (or taken from --parts). The GLB is written only when the
  character is PASS, or NEEDS_REVIEW and accepted; a FAIL is never written. Exit code as above.
            --single-view flat|thin|medium|thick|<depth in pixels>
  Exactly one view. Its depth is your choice, never measured: everything the
  drawing does not show is reported as inferred.

Export formats and engine presets
---------------------------------
  The format comes from --out: .glb (one file), .gltf (+ .bin + .png) or
  .obj (+ .mtl + .png). Presets bundle the units and the facing:
            --preset generic|godot|unity   metres, the front faces +Z
            --preset roblox                studs (1 stud = 0.28 m), front -Z
  Overrides: --units metres|centimetres|studs|pixels, --front +z|-z,
             --unlit (glTF: exact pixel colours, no shading).
  Every file is reloaded by an independent reader and re-rendered before it
  is reported as written.

View conventions
----------------
  Front: seen from the front.   Side: seen from the LEFT, so the object's front
  points to the right of the image.   Top: seen from above, the object's front
  edge at the BOTTOM of the image.   Mirrored views are usually detected and
  fixed automatically. Art exported at 2x/4x/... is detected and normalised.

Result
------
  Exit code 0 = PASS, 2 = NEEDS_REVIEW, 1 = FAIL or error. A requested output
  (report, render or project) that cannot be written is an error (exit 1) and
  every file that failed is listed; nothing is skipped silently.
  PASS          the model is watertight, every view and its 90/180/270 degree
                rotations re-render to the drawn silhouettes, it has real depth
                (no cardboard), and the GLB reloads identically.
  NEEDS_REVIEW  the drawings do not determine something; report.json lists it:
                ambiguous_correspondence  several part combinations fit (e.g.
                                          three legs drawn as 3 x 2) - add a
                                          Bottom view or decide the parts;
                possible_recess           an enclosed darker area may be a hole
                                          or cavity of unknown depth.
                The GLB still contains the maximal (all-candidates) solution.
  INFO items (inferred_grid, inferred_surface, ...) are assumptions the program
  made and tells you about; they do not block PASS.

Outputs
-------
  chair.glb             one mesh, pixel-exact texture, NEAREST sampling, opaque
                        (.gltf/.obj: the same mesh in companion files; OBJ
                        cannot store NEAREST: set point filtering on import)
  report.json           every metric and check (schema pixel3d.report/1)
  renders/views         re-renders of each view at 1x and 8x
  renders/rotations     yaw_090 / yaw_180 / yaw_270
  renders/turntable     24 frames + contact_sheet.png
  chair.p3d             project (sources, decisions, cache); re-check with
                        pixel3d-cli validate --project chair.p3d

Examples are in the examples folder.
