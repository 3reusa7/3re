# Building 3re

3re builds on Windows 10/11 x64 with MinGW-w64 GCC 13.1, CMake 3.25+, Ninja and Qt 6.8.3 (mingw_64). All C/C++
dependencies are vendored in `third_party/` (versions and licences: `third_party/README.md`), so the build itself
works offline. Linux and macOS builds are planned (see the roadmap in the README).

## 1. Toolchain (no admin rights)

```powershell
powershell -ExecutionPolicy Bypass -File tools\bootstrap_toolchain.ps1
```

This downloads MinGW 13.1, CMake, Ninja and Qt 6.8.3 from Qt's official repository into `.toolchain\Qt\`, checking
each archive's SHA-256. (The CI uses `jurplel/install-qt-action` with the same versions, see
`.github/workflows/ci.yml`.)

Two more inputs the build expects under `.toolchain\` (both official releases, pinned by SHA-256):

| Input | Where | How |
|---|---|---|
| ONNX Runtime 1.22.0 (headers + `onnxruntime.dll`) | `.toolchain\onnxruntime\1.22.0\` | `tools\ci\fetch_inputs.ps1 -OnnxRuntime` |
| Khronos glTF-Validator 2.0.0-dev.3.10 | `.toolchain\gltf_validator\2.0.0-dev.3.10\` | `tools\fetch_gltf_validator.ps1` |

## 2. Configure, build, test

From Git Bash:

```bash
TC=$PWD/.toolchain/Qt/Tools; PATH=$TC/mingw1310_64/bin:$TC/CMake_64/bin:$TC/Ninja:$PATH
cmake --preset release
cmake --build --preset release -j 6
ctest --preset release -j 4 --timeout 1500
```

Or PowerShell with the same PATH. The `debug` preset traps undefined behaviour (UBSan trap mode): an
"Illegal instruction" in a debug run is UB; `gdb -batch -ex run -ex bt --args <exe>` shows where.

Tests that need things a CI runner does not have skip or are configured by environment:

| Test | Needs | Without it |
|---|---|---|
| `trellis-upgrade` | six pictures in `PIXEL3D_TRELLIS_IMAGE_CASES` (`a.png;b.png;…`) | fails: pass any six pictures, for example from `tests/fixtures` |
| `studio-tests` (SAM) | SAM 2.1 tiny ONNX in `.toolchain\models\sam2.1_hiera_tiny_20260221\extracted` | `tools\ci\fetch_inputs.ps1 -Sam` downloads the pinned files |
| `agent-core` (Godot import) | `Godot_v4.7-stable_win64_console.exe` on the Desktop | that check is skipped with a message |
| generation on a GPU | the weights, an NVIDIA GPU | not part of `ctest`; `3re.exe --e2e-generate PICTURE --e2e-out SUMMARY.json` runs one |

## 3. A runnable folder

`build/release/apps/3re-studio.exe` runs from the build folder when the Qt `bin` folder is on PATH. For a complete,
portable release folder use the packaging script (it also runs the self-test):

- maintainers, with the pinned engine and models: `tools\package.ps1` (writes `dist\3re`, see `docs/RELEASING.md`);
- the public release (no models, downloaded on first use): `tools\release\make_package.ps1`, as the release workflow
  does (`.github/workflows/release.yml`).

## 4. The GPU engine

The TRELLIS.2 engine is `trellis-cli.exe` from [trellis.cpp](https://github.com/pwilkin/trellis.cpp) at commit
`610c90e` with 3re's patches in `tools/trellis/engine-patch/` (4 GB VRAM mode, texture candidates). It is built
separately with CUDA 12 and shipped as a pinned binary; its GPU libraries (ggml, CUDA runtime) and the weights are
downloaded on first use (`apps/update/models.json`). Building the engine in CI is a planned task.

## 5. Before a pull request

- `ctest` passes (or you say which test you could not run and why);
- `python -I tools/ci/lint.py` and `python -I tools/ci/licence_scan.py` pass;
- commits are signed off (`git commit -s`, see CONTRIBUTING.md).
