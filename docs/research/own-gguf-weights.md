# Our own TRELLIS.2 Q4 GGUF files

Written 2026-11-04, before the experiment. Time-box: 1 day.

## 1. Problem

3re's first-run download points at `huggingface.co/ilintar/trellis2-gguf` (revision a57397bd). That repository declares
"License: other" and its card only says "see the source model", linking a different repository
(`microsoft/TRELLIS-image-large`). A public release should not make a file set with no stated licence its default
download. The owner's ruling: build our own files from the Microsoft checkpoints and switch only after proving
equivalence.

## 2. Study

Facts (read 2026-11-04 from the Hugging Face API and the trellis.cpp source):

| File | Source (revision) | Licence |
|---|---|---|
| ss_flow, shape_flow_512, tex_flow_512, shape_dec, tex_dec | `microsoft/TRELLIS.2-4B` (af44b45f) | MIT |
| ss_dec | `microsoft/TRELLIS-image-large` (25e0d31f), `ss_dec_conv3d_16l8_fp16` | MIT |
| dinov3 | `timm/vit_large_patch16_dinov3.lvd1689m` (30c11095), ungated mirror | **DINOv3 License**, not MIT |

- The converter is trellis.cpp's own (MIT): `tools/convert.py` (safetensors to F16/F32 GGUF, plain bit conversion) and
  `tools/quantize_gguf.py` (2-D matmul weights with a block-aligned ne0 to Q4_0, everything else kept). Its README names
  exactly the sources above.
- The DINOv3 License allows redistribution when the agreement is passed on and "Built with DINOv3" is shown. So
  `dinov3.gguf` can be hosted by the project, but under that licence, with its text next to it, never as MIT.

Assumptions: the published q4 files were made by these same tools at about this version. Both steps are deterministic,
so the same inputs should give the same bytes (only the writer's metadata could differ). Not known until measured.

## 3. Feasibility

Download about 10.9 GB (needed files only, not the 1024 models). Disk 31 GB free. Conversion streams tensors on the CPU,
in numpy; no GPU and no training. RAM: one tensor at a time plus the writer; measured during the run. Our Hugging Face
link has been 0.3-1 MB/s, so 3-10 hours for the download, inside the time-box.

Changed on 2026-11-04, before any result: the local download ran at about 0.4 MB/s, so the conversion runs in GitHub
Actions instead (`.github/workflows/build-weights.yml`, same tools and sources, also saves uploading the files from
here). The criteria in section 5 are unchanged; the tensor comparison of Level 2 runs there too.

## 4. Licence

MIT for six files, DINOv3 License for one. Commercial use allowed by both; conditions as in section 2.

## 5. Hypothesis and acceptance criteria (fixed before running)

Hypothesis: running the same tools on the Microsoft checkpoints reproduces the pinned q4 files.

Baseline: the pinned q4 files (`models-manifest.json`, revision a57397bd).

- **Level 1, adopt at once:** every file has the same SHA-256 as its pin. Same bytes mean the same outputs; no further
  test is needed.
- **Level 2, a file differs:** compare every tensor (name, shape, type, bytes) and the metadata.
  - only metadata differs, all tensors byte-identical: adopt after one generation shows identical `raw.ply`;
  - any tensor differs: run the standard pictures (`tests/fixtures`: S_segmentation/subject, F_hooded_reference/front,
    D_character/front, plus a saved prepared input), seed 42, the app's arguments, old and new weights, and require for
    every picture: the same gate verdict; vertex and face counts within 0.5 %; symmetric Chamfer distance at most 0.1 %
    of the bounding-box diagonal; front silhouette IoU at most 0.005 lower. Any failure: do not switch.
- **Not reproducible in the time-box:** negative result written here, the release stays held, the owner decides.

## 6. Risks and time cost

- Slow download (mitigation: resume, verified per file against LFS digests).
- The tools changed after the published files were made (then Level 2 decides).
- Hosting: all seven files are under 1 GB, so GitHub Release assets (2 GB limit per file) are enough.
- If `dinov3.gguf` differs from the pin, the shipped engine's image conditioning changes: Level 2 covers it.

## 7. Result

**Level 1 met on 2026-11-04: all seven files have the same SHA-256 as their pins. Adopted.**

| Run | Q4_0 rounding | Result |
|---|---|---|
| [41872351553](https://github.com/3reusa7/3re/actions/runs/41872351553) | gguf-py 0.17.1 as it is (emulates fused multiply-add) | `ss_dec` identical (no Q4 tensors); in the other six, metadata, tensor names, types and scales identical, but 0.02-0.29 % of Q4 blocks differ by one step |
| [41874690706](https://github.com/3reusa7/3re/actions/runs/41874690706) | the same with numpy 1.26.4 | the same counts as with numpy 2.5.4 |
| [41876115817](https://github.com/3reusa7/3re/actions/runs/41876115817) | plain float32 | **7 of 7 byte-identical** |

So the published files were made with plain float32 rounding, and that is the only setting changed from the tools as
they are (`q4_float32` in `tools/weights/build_q4.py`). Same bytes mean the same outputs, so as section 5 says, the
standard pictures were not run. The report of each run is its `weights-report` artifact.
