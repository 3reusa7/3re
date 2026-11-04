# Good first issues

Small, self-contained tasks taken from real open items. Each one says where to look and when it is done.
On GitHub they carry the `good first issue` label; comment on one before you start so two people do not
do the same work. Generated from `.github/good-first-issues.json` by `tools/github/setup_repo.py docs`.

## 1. Settings cards: two-line wrapped labels get one line's height

Labels: `good first issue`, `ui`

In Settings, a word-wrapped `QLabel` whose text needs two lines sometimes gets the height of one line inside a `Card` (seen with the update privacy note; worked around in `apps/gui/studio/settings_page.cpp` with explicit line breaks and a minimum height). Find the layout cause (height-for-width through `Card` and the scroll area) and fix it once for every card.

Done when: a long wrapped label in any Settings card shows all its lines at 1280x800 and 1920x1080, with a Qt Test in `tests/studio/`.

## 2. Fix -Wmisleading-indentation warnings in tests/gui/trellis_workflow_smoke.cpp

Labels: `good first issue`, `tests`

The release build prints four `-Wmisleading-indentation` warnings for `tests/gui/trellis_workflow_smoke.cpp` (lines 52, 56, 81, 86). Reformat those `if` statements so the intent is clear.

Done when: the build has no warnings for that file and behaviour is unchanged.

## 3. Viewport toolbar crowds at 1280 px wide

Labels: `good first issue`, `ui`

In Pixel Art Builder the viewport toolbar (view buttons and display toggles) crowds or clips at a 1280 px wide window. Make it wrap or overflow into a menu.

Done when: `3re-studio --screenshot out.png --page image --image-mode pixel --size 1280x800 <front.png>` shows every control usable, with a screenshot before/after in the PR.

## 4. A public evaluation set made only of CC0 or self-made pictures

Labels: `good first issue`, `evaluation`

The maintainer's regression pictures cannot be published (several are third-party). Collect 8-12 pictures (characters, props, a vehicle, a plant) that are CC0 or made by the contributor, with a `SOURCES.md` listing author, licence and link for each, in a new `eval/public/` folder.

Done when: every picture has a recorded CC0/own source and `tools/ci/licence_scan.py` passes.

## 5. Arabic translation of the interface (Qt Linguist)

Labels: `good first issue`, `translation`

The interface is English only (`QLocale` is forced to English in `apps/gui/main.cpp`). Add Qt's translation pipeline (`qt_add_translations`, a `3re_ar.ts`) and a language choice in Settings, starting with the Home page and Settings.

Done when: choosing Arabic shows the translated pages right-to-left and English stays the default.

## 6. Hardware table from hardware reports

Labels: `good first issue`, `documentation`

Create `docs/hardware.md`: one row per hardware report issue (GPU, VRAM, driver, RAM, generated yes/no, minutes, result) and link it from the README's Hardware section.

Done when: the table exists with the maintainer's RTX 3050 4 GB row and a short how-to for adding rows.

## 7. Self-test --deep: check the downloaded GPU libraries against app/models.json

Labels: `good first issue`, `tests`, `updates`

`3re.exe --self-test --deep` re-hashes the weights (models-manifest.json) but only checks that the GPU engine libraries in `app/runtime` exist. Read `app/models.json` in `apps/gui/self_test.cpp` and compare the size and SHA-256 of every downloaded file (engine libraries, SAM) in deep mode.

Done when: a damaged downloaded DLL fails `--self-test --deep`, with a test in `tests/studio/test_self_test.cpp`.

## 8. Update dialog: show the time left while downloading

Labels: `good first issue`, `updates`

`apps/gui/studio/update_ui.cpp` shows bytes done / total. Add the download speed and an estimate of the time left (smoothed), for updates and model downloads.

Done when: both dialogs show speed and time left, tested with the local server in `tests/update/test_update_flow.cpp`.

## 9. Settings > AI models: open the models folder

Labels: `good first issue`, `ui`

Add an "Open folder" button next to "Download missing models…" that opens `<3re>\models` in Explorer (create it if missing).

Done when: the button works in a release folder and is disabled in development builds.

## 10. Model download dialog: per-pack progress list

Labels: `good first issue`, `ui`, `updates`

The model dialog shows one progress bar for everything. Show each pack (engine libraries, TRELLIS.2 weights, SAM) with its own state: waiting, downloading, checking, done, failed.

Done when: the states are visible and covered by a test with the local server.

## 11. Test the Windows command-line quoting of the updater launch

Labels: `good first issue`, `tests`, `updates`

`apps/update/update_service.cpp` quotes arguments for `CreateProcessW` (backslashes before quotes, spaces, trailing backslashes). Move the function to the Qt-free core and add doctest cases that round-trip through `CommandLineToArgvW`.

Done when: paths with spaces, quotes and trailing backslashes round-trip in `tests/update/test_update_core.cpp`.

## 12. 3re-cli: --check-update prints the available update

Labels: `good first issue`, `updates`

For scripts: `3re-cli --check-update` (or a small `3re-update --check`) that prints the newest version on the chosen channel and whether its signature verified, without downloading.

Done when: it prints `up to date` or `X.Y.Z available (signature OK)` and is tested against the local test server.

## 13. Label tests that need a GPU, Godot or downloaded models

Labels: `good first issue`, `tests`, `build`

Add ctest LABELS (`gpu`, `models`, `godot`, `network`) in `tests/CMakeLists.txt` so CI and contributors can select tests with `ctest -L` / `-LE`, and document them in `docs/BUILDING.md`.

Done when: `ctest -LE gpu` runs everything a CPU-only runner can run.

## 14. Rename the remaining Pixel3D names in user-facing docs

Labels: `good first issue`, `documentation`

3re was called Pixel3D before 2.0. `docs/README.md` and some help texts still say Pixel3D. Update the user-facing text (not file formats or identifiers that existing projects depend on).

Done when: user-facing docs say 3re, and saved `.p3d` projects still open.

## 15. Release notes: extract a version's section from CHANGELOG.md

Labels: `good first issue`, `build`

`tools/release/release_notes.py` writes the release body from `CHANGELOG.md`. Add tests for it (missing version, last section, Arabic summary file present/absent).

Done when: a small test suite runs in CI's lint job.

## 16. Inventory of Windows-only code for the Linux port

Labels: `good first issue`, `linux`, `documentation`

Before the platform layer (Linux CUDA first), list every Windows-only API use: Job Objects, PSAPI/PDH memory and GPU telemetry, `CreateProcessW`, `_wfopen`, registry, paths, the launcher. Write `docs/PORTING.md` with file, function and the planned portable replacement.

Done when: the list covers `apps/`, `core/` and `tools/` (grep for `windows.h`, `#ifdef _WIN32`, `Q_OS_WIN`).
