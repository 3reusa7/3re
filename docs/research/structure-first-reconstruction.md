# Pixel3D: structure-first reconstruction (research brief)

Date: 2026-10-14. Purpose: the root-cause fix that takes Pixel3D's Low-Poly Studio from alpha to polished.
Constraints: free, fully local (RTX 3050 4 GB, 16 GB RAM, CPU fallback), commercially licensable worldwide (EU included), 500–1,500 triangles, rig-ready, original art style preserved. Inputs: Front + Side, or Front only.

## 1. Diagnosis: why the alpha keeps failing

Every visible failure so far is the same missing signal: **the engine has no model of the character's structure.** It sees silhouettes and colour regions, then guesses.

| Failure seen in testing | Missing structural signal |
|---|---|
| Side-by-side legs fused (LP-1); touching arms merged into the torso (LP-5) | No skeleton: nothing says "a shoulder is here, an elbow is there" |
| Knight: the sword and right arm became `leg_r_2…leg_r_4`; no right arm | No joints: a thin object starting at a wrist is a held item, not a leg |
| Shield became a capsule | No part-shape type: flat parts need a slab, not a loft |
| Face buried inside the hood; face coloured scarf-blue (LP-3) | No occlusion/depth ordering between overlapping parts |
| Back of the character shows the front's scarf and chest | No texture policy separating "front detail" from "base colour" |
| 37 manual operations to fix one character | Corrections are pixel brushing, not "click the part" |

Patching each symptom separately is why fixes keep producing new failures. The published systems that work on drawings (Monster Mash, RigMesh, CreatureShop, Animated Drawings, DrawingSpinUp) all build the same backbone: **skeleton → parts → depth order → per-part primitive → constrained inflation → texture policy**. They also all use a few cheap annotations rather than a fully automatic black box.

## 2. What the research says, and what we take from each

| Source | What it proves | What Pixel3D takes | Licence / feasibility |
|---|---|---|---|
| **Monster Mash** (Dvorožňák et al., SIGGRAPH Asia 2020) | Single-view parts become a good 3D character with per-part inflation plus depth-order constraints | Poisson inflation with a √ profile; in-front/behind inequality constraints; merging vs free boundaries | Apache-2.0 (src). Read it, re-implement |
| **Animated Drawings** (Smith et al., Meta 2023) | A keypoint model fine-tuned on drawings finds 17 joints reliably (~90 % usable after 2,500 samples); classical segmentation beats Mask R-CNN on drawings | Joint detection (shoulders, elbows, wrists, hips, knees, ankles) to drive part splitting | Code AND weights MIT. Repo archived Sep 2025 (read-only, still usable). Needs ONNX export. Untested on pixel art: benchmark it |
| **LazyBrush + body-part segmentation** (Sýkora 2009; Tsukuba 2018) | Graph-cut segmentation from a few seeds handles gaps, weak edges and touching parts; overlap at joints prevents cracks | Graph-cut segmentation seeded by the skeleton; small overlap at joints | Algorithm only, re-implement |
| **Sparse depth (in)equalities** (Sýkora et al. 2010) | Relative depth for cartoon regions from sparse in-front/behind relations, with a fast approximate solver | Depth ordering from enclosure and T-junction cues | Algorithm only |
| **RigMesh** (Borosán et al. 2012) | Part-based modelling where the rig is built and kept consistent per part during cut and merge | Skeleton per part type; rig stays valid while parts are edited | Algorithm only |
| **DrawingSpinUp** (SIGGRAPH Asia 2024) | Thin structures in drawings need special handling (skeleton-based thinning); contour lines confuse texturing (remove, then re-render) | Thin-part primitive; strip ink outlines before texturing, re-add them as the 3D outline | Uses LaMa, Mixamo (online) and a 2080 Ti or better; licence unclear. Ideas only |
| **Short-cut rule** (Luo et al. 2014) | Silhouette part cuts at concave points, matching human perception | Fallback splitting when there is no skeleton or layer | Algorithm only |
| **SAM 2.1** (Meta) | Click-to-segment works on arbitrary images | One-click part correction instead of brushing | Apache-2.0. Tiny ONNX is 111 MB and runs on CPU |
| **Kenney Shape** (commercial tool) | Artists accept and like per-pixel depth painting for pixel art | Keep and polish the depth brush as the manual fallback | UX reference only |

**Rejected, with reasons**
- **TripoSR:** flat depth on 2D art, measured on your machine.
- **Hunyuan3D:** licence excludes the EU, UK and South Korea.
- **StdGEN, PartCrafter, MV-Adapter:** dense meshes, not low-poly. MV-Adapter image-to-multiview needs about 14 GB of VRAM; the others have unclear licences or need high-end GPUs.
- **See-through:** non-commercial paper licence, and it needs an RTX 4090.

None of these fit a 4 GB, commercially sold, pixel-art pipeline.

## 3. The root solution: structure-first pipeline

### Stage A: skeleton (new)
1. If Aseprite/PSD layers exist, they define the parts (highest trust).
2. Otherwise, run the Animated Drawings keypoint model (ONNX, CPU or DML) on the Front view, upscaled with nearest-neighbour. On the Side view, run the same model plus cross-view row matching.
3. Fallback: medial-axis skeleton plus short-cut-rule cuts.
4. Output a joint graph with a confidence per joint. Low-confidence joints are shown to the user, never silently trusted.

### Stage B: parts (replaces colour-band splitting)
- Run graph-cut segmentation (LazyBrush-style). Seeds come from the bones: each bone seeds its part. Ink lines and strong colour edges are cheap cut places; flat colour interiors are expensive.
- Rules, each with a unit test:
  - A thin region that starts at a wrist and has no ground contact is a **held item**, never a leg.
  - Legs start below the hips and reach the lowest row.
  - An arm drawn over the torso keeps its own label wherever the Side view shows it in front.
  - Parallel colour bands with no concavity are texture, never parts.
- Add a small overlap at each joint (as in the Tsukuba paper) so posing never opens cracks.

### Stage C: part shape type (new classifier)
Use the thickness ratio between the two views plus the skeleton:

| Type | Rule | Primitive |
|---|---|---|
| Round (head, limb, barrel) | Similar width in Front and Side | Ellipsoid, lathe or loft along the bone |
| Flat (shield, cape, sign, ear) | Wide in one view, ≤ 2–3 px in the other | Slab; thickness comes from the thin view |
| Thin (sword, staff, antenna) | ≤ 2 px wide along a long axis | Swept thin box along its centre line |
| Volume (torso, cloak, hair mass) | Neither of the above | Poisson-inflated loft clamped by Side |
| Inconsistent | Shape/width contradicts between views | **UNRESOLVED**: shown to the user, never silently a capsule |

### Stage D: depth ordering (new)
- Build relations from:
  - **enclosure:** a region fully inside another, with a different colour family, sits in front (face in the hood opening, eyes, buckle);
  - **T-junctions:** the occluding contour continues, the occluded one stops;
  - **the Side view**, where present: it is ground truth.
- Solve the sparse depth (in)equalities (approximate solver). Feed them as constraints into inflation, as in Monster Mash ARAP-L, so enclosed parts sit on the parent's front surface and are never buried.

### Stage E: texture policy (fixes the back-side problems)
1. Strip ink outlines from the texture source (DrawingSpinUp's removal-then-restoration). The 3D outline shader redraws them.
2. Split each part's art into **base colour** (the dominant palette colour of the part) and **front details** (small enclosed features: eyes, emblems, buckles).
3. A face takes projected texels only if its normal is within about 60° of that view. Details are never projected to the back.
4. Back and hidden faces get the base colour plus a 1-step palette shade. A part is mirrored only when it is symmetric left/right. Everything here is labelled INFERRED.

### Stage F: side-view draft loop (new; removes the "Top/Side is hard to draw" problem)
1. From the inferred model, render a pixel-art **Side draft**: orthographic, nearest-neighbour, palette-quantised, with rows aligned to the Front.
2. Export it as a PNG or a new Aseprite layer next to the user's art.
3. The artist fixes the draft in minutes (it is already the right size and the right colours) and re-imports it.
4. The re-imported view upgrades the parts it covers from INFERRED to CONFIRMED.

This turns the hardest drawing task into an editing task, with no generative AI.

### Stage G: one-click correction (replaces brushing)
- SAM 2.1-tiny (Apache-2.0, 111 MB ONNX, CPU) for click-to-select a part; click again to add or remove.
- "Wrong type" stays a dropdown (it already exists). "Split here" is a single stroke across a limb.
- **Target:** the corrected hooded_character drops from 37 operations to ≤ 10, measured as before.

## 4. Order of work (each step measured with the existing P2 comparison and quality metrics)

| Release | Content | Expected effect |
|---|---|---|
| 1.3.0 | Stages A–D: skeleton, graph-cut parts, shape types, depth ordering | Fixes the knight (sword/shield), touching arms, the buried face |
| 1.4.0 | Stage E (texture policy) + Stage G (SAM click-fix) | Clean backs; far fewer manual operations |
| 1.5.0 | Stage F (side-draft loop) | Front-only becomes Front + confirmed Side with minimal drawing |

## 5. Risks and open questions

- **Animated Drawings on pixel art:** the keypoint model was trained on children's drawings, so tiny pixel-art characters (about 24×48 px) may be out of domain. Benchmark on samples\ first. If it is weak, the medial-axis and short-cut fallback carries the load, and joints come from a 2-click "mark shoulder/hip" UI.
- **SAM 2.1 on 8× upscaled pixel art:** untested. It may over-merge flat colour areas; benchmark before committing to it.
- **Depth solver:** the paper's QP is slow, so use its approximate solver. Our part count is small (under 30), so this is cheap either way.
- **Licences:** Animated Drawings is MIT (code and weights), SAM 2.1 and Monster Mash src are Apache-2.0. Re-implement the algorithms in our own C++. Keep NOTICE files for any shipped weights.

## Sources
- Monster Mash paper: https://dcgi.fel.cvut.cz/wp-content/wpallimport-dist/publications/pdf/publications-2020-dvoroznak-tog-mm-paper.pdf
- google/monster-mash (Apache-2.0): https://github.com/google/monster-mash
- Animated Drawings paper: https://arxiv.org/pdf/2303.12741
- facebookresearch/AnimatedDrawings (MIT code + weights): https://github.com/facebookresearch/AnimatedDrawings
- Character body-part segmentation (LazyBrush extension): https://kanamori.cs.tsukuba.ac.jp/publications/nicoint2018/character_segmentation.pdf
- LazyBrush: https://dcgi.fel.cvut.cz/~sykorad/lazybrush.html
- Adding Depth to Cartoons Using Sparse Depth (In)equalities: https://dcgi.fel.cvut.cz/~sykorad/depth.html
- RigMesh: https://cragl.cs.gmu.edu/rigmesh/
- DrawingSpinUp: https://lordliang.github.io/DrawingSpinUp/ and https://github.com/LordLiang/DrawingSpinUp
- Short-cut rule: https://arxiv.org/pdf/1409.2104
- SAM 2.1 ONNX (Apache-2.0): https://huggingface.co/vietanhdev/segment-anything-2.1-onnx-models
- CreatureShop (NVIDIA): https://research.nvidia.com/publication/2022-08_creatureshop-interactive-3d-character-modeling-and-texturing-single-color
- Sketch Animation STAR (2025): https://arxiv.org/html/2510.10218v1
- StdGEN: https://arxiv.org/html/2411.05738v2
- PartCrafter: https://arxiv.org/html/2506.05573v1
- MV-Adapter: https://github.com/huanngzh/MV-Adapter
- Kenney Shape docs: https://kenney.nl/knowledge-base/tools/kenney-shape-documentation
