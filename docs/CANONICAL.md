# 3re — where things live

There is ONE product (3re), ONE development line and ONE active release.

| What | Where |
|---|---|
| Development line | branch `main` in `<main checkout>` (the main folder) |
| Active release | `<main checkout>\dist\3re\` — start `3re.exe` |
| The one build-and-package command | `powershell -ExecutionPolicy Bypass -File tools\package.ps1` (see `docs/RELEASING.md`) |
| Rollback packages | `<checkout parent>\retile-archive\3re-rollback\<version>-<time>\` (written by every package run) |
| Archived pre-3re builds | `<checkout parent>\retile-archive\builds-2026-10-20\` (`INDEX.md` lists each) |
| Pinned binary/model inputs | `.toolchain\` of the main folder, verified by `tools\stores.ps1` |

## Pinned inputs (`tools\stores.ps1`)

| Input | Store | Pinned by |
|---|---|---|
| TRELLIS.2 CUDA runtime (`trellis-cli.exe`, ggml, cuBLAS, CUDA runtime, MSVC/MinGW runtime) and the verified GeometrySafe cleanup `pixel3d-trellis-post-safe.exe` | `.toolchain\trellis\runtime\` | SHA-256 per file |
| TRELLIS.2 Q4 weights (dinov3, ss_flow, ss_dec, shape_flow_512, shape_dec) | `.toolchain\models\trellis\q4\` + `models-manifest.json` | size + LFS SHA-256 (manifest revision `a57397bd`) |
| SAM 2.1 Hiera-Tiny ONNX (encoder, decoder) | `.toolchain\models\sam2.1_hiera_tiny_20260221\extracted\` | SHA-256 per file |
| ONNX Runtime 1.22.0 CPU (official Microsoft release, Authenticode-signed DLL) | `.toolchain\onnxruntime\1.22.0\` | zip + DLL SHA-256 |

The stores were imported once (2026-10-20) from the GeometrySafe build and the original
TRELLIS model folder as NTFS hard links: they no longer depend on those paths. A release
never refers to `.toolchain`, a worktree or the original model folder; its config holds
relative paths only.

Releases ship path-free rebuilds (2026-11-03, no user paths inside the binaries) that give the same outputs as the
verified builds: the engine fork `trellis-fork\tex-candidates-4e19aa96` (of `61fd983f`), the cleanup
`trellis-post\safe-415020aa` (of `e281d0a4`) and QuadWild `quadwild-bimdf\3re-v5` (of `3re-v4`); each store's
BUILD.txt says how it was built and verified. They are not rebuilt per release. After changing
`tools/trellis/post_geometry_safe.cpp`, re-verify on saved runs and real generations, then replace the store file and
its pin together.

## History (not development lines)

The worktrees under `.claude\worktrees\` and branches such as `feat/trellis-image3d`,
`claude/pixel3d-unsafe-geometry-9bef46`, `feat/polished-styles`, `claude/low-poly-*` hold
earlier milestones. Everything approved from them is in `main`; they are kept for history and
rollback only. New work branches from `main` and merges back into `main`; releases always
update `dist\3re`.
