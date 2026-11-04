# Releasing 3re

There is one active release: `dist\3re\` of the main checkout. Every approved change on
`main` updates it with one command:

```
powershell -ExecutionPolicy Bypass -File tools\package.ps1
```

What it does (details in the script header):

1. `tools\stores.ps1` verifies the pinned inputs (TRELLIS runtime + weights, SAM, ONNX Runtime).
2. Release build + the whole test suite (`trellis-upgrade` gets six saved images from
   `Documents\Pixel3D\Generated` / `Documents\3re\Projects`, or `PIXEL3D_TRELLIS_IMAGE_CASES`).
3. Stages `dist\.staging-3re\` (launcher, `app\`, `models\` as hard links, relative config).
4. `3re.exe --self-test --deep` on the staged folder with only Windows on PATH, and a scan
   for foreign paths (Codex, worktrees, `.toolchain`).
5. Atomic swap: the current `dist\3re` moves to
   `<checkout parent>\retile-archive\3re-rollback\<version>-<time>\`, the staged folder
   becomes `dist\3re`. A failure before the swap leaves `dist\3re` untouched.

Then check portability on a real copy: `powershell -File tools\verify_portable.ps1`.

Version: `VERSION` (e.g. `2.0.0-alpha`); bump it in the commit that changes behaviour and add
a `CHANGELOG.md` section. Nothing is pushed by these scripts.

Rollback: close 3re, move `dist\3re` aside, move the wanted folder from `3re-rollback\` back to
`dist\3re` (each holds its own `RELEASE.json`).

## Public releases on GitHub

The public release is built by CI, not by `tools\package.ps1`, and does not contain the AI models (they are
downloaded on first use, `apps/update/models.json`):

1. Bump `VERSION`, add the `CHANGELOG.md` section and an Arabic summary `docs/release-notes/<version>.ar.md`.
2. Push the tag `v<version>`. `.github/workflows/release.yml` builds, runs every test, packages with
   `tools\release\make_package.ps1` (self-test included), builds the per-user installer (`tools\release\3re.iss`),
   writes `update-manifest.json`, signs it with the Actions secret `THREERE_UPDATE_SIGNING_KEY`, and publishes the
   release with `SHA256SUMS.txt`. A tag containing `dryrun` makes a draft instead.
3. The binaries the workflow cannot build yet (the trellis.cpp engine fork, the verified cleanup, QuadWild and the
   runtimes) come from the release `build-inputs-<N>`, pinned file by file in `tools/release/build-inputs.json`
   (`tools/release/pack_build_inputs.py` makes a new one).

Installed copies see the new release through Settings > Updates (docs/UPDATES.md).
