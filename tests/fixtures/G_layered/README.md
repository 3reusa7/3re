# G_layered — layered and WebP source files (1.2.0 input formats)

Ground truth never comes from Pixel3D's own decoders.

| file | made by | ground truth |
|---|---|---|
| `rgb_layers.aseprite` | Aseprite 1.3.18 (headless): 10×12 RGB, layers `body`, `head`, `hat_hidden` (hidden), two frames (frame 2 paints the head green) | `rgb_layers_frame1.png` = Aseprite's PNG export of frame 1; `rgb_layers_frame1_body.png`, `rgb_layers_frame1_head.png` = Aseprite's export of that layer alone |
| `indexed.aseprite` | Aseprite 1.3.18: 8×8 indexed (4-colour palette, index 0 transparent) | `indexed_frame1.png` (Aseprite export) |
| `grayscale.aseprite` | Aseprite 1.3.18: 6×6 grayscale (value + alpha) | `grayscale_frame1.png` (Aseprite export) |
| `groups_opacity.aseprite` | Aseprite 1.3.18: `base`; group `grp` with `decal` at layer opacity 128; hidden group `hidden_grp` with `secret` | `groups_opacity_frame1.png` (Aseprite export) |
| `krita_layers.psd` | Krita 5 (`krita.com layers.ora --export`) from an OpenRaster file with the same three layers as `rgb_layers` (hat hidden) | `krita_layers_merged.png` = the composite stored in the PSD, read by Pillow; layer rectangles from the OpenRaster source |
| `krita_groups.psd` | Krita 5 from OpenRaster: `base`, group `grp` / `decal` at 50 %, hidden group `hidden_grp` / `secret` | `krita_groups_merged.png` (Pillow) |
| `lossless.webp`, `lossy.webp`, `animated.webp` | Pillow 12.1 (libwebp) from `rgb_layers_frame1.png`; the animation's frame 2 has a red pixel at (0, 0) | `rgb_layers_frame1.png` (lossless and the animation's first frame are exact) |

Not covered by an independent fixture (implemented to the specification, tested only through the
robustness cases): 16-bit PSD channels, indexed and grayscale PSD, ZIP-compressed PSD layer channels,
Aseprite tilemap cels (reported and skipped), old Aseprite palette chunks.
