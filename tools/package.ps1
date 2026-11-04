<#
.SYNOPSIS
  The one build-and-package command of 3re: builds, tests, stages, self-tests and
  atomically replaces the ONE active release dist\3re\ (docs/RELEASING.md).

.DESCRIPTION
  1. Verifies the pinned stores (tools\stores.ps1).
  2. Configures and builds Release, runs the whole test suite (unless -SkipTests).
  3. Stages a complete release in <dist>\.staging-3re\:
       3re.exe (launcher), app\ (3re-studio + Qt + helpers + CLI + MCP + ONNX Runtime
       + runtime\ = TRELLIS.2 CUDA runtime), models\ (TRELLIS Q4 + SAM 2.1-tiny,
       hard links to the verified stores), app\3re.config.json (relative paths only),
       docs, licences, examples, RELEASE.json.
  4. Self-tests the staged folder as a user gets it: `3re.exe --self-test --deep`
     with only Windows on PATH, plus a scan for foreign paths in every text file.
  5. Swaps atomically: the current dist\3re moves to the rollback archive
     (<archive>\3re-rollback\<version>-<time>\), the staged folder becomes dist\3re.
     Any failure before the swap leaves the current dist\3re untouched; a failure
     during the swap restores it.
  Nothing is pushed, committed or deleted outside the staging folder.

.PARAMETER DistRoot     Folder holding 3re\ (default: dist\ of the main checkout).
.PARAMETER ArchiveRoot  Rollback archive (default: retile-archive\ next to the main checkout).
.PARAMETER SkipTests    Skip ctest (only for re-packaging an already tested build).
.PARAMETER FailAfterStage  Test hook: stop with an error after the self-test, before the swap.
#>
param(
  [string]$DistRoot = '',
  [string]$ArchiveRoot = '',
  [switch]$SkipTests,
  [switch]$FailAfterStage
)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $Repo
$Stage = $null
function Fail([string]$why) {
  Write-Host ""; Write-Host "PACKAGE STOPPED: $why" -ForegroundColor Red
  # The staging folder is this run's own scratch; dist\ keeps only the active release.
  if ($Stage -and (Test-Path $Stage)) { Remove-Item -Recurse -Force $Stage -ErrorAction SilentlyContinue }
  exit 1
}
function Step([string]$what) { Write-Host ""; Write-Host "== $what" -ForegroundColor Cyan }
function Native([string]$exe, [string[]]$a) {
  $saved = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  & $exe @a | Out-Host
  $code = $LASTEXITCODE; $ErrorActionPreference = $saved
  return $code
}

$common = (& git -C $Repo rev-parse --path-format=absolute --git-common-dir).Trim()
if (-not $DistRoot) {
  # The main checkout's dist\ (also when run from a worktree).
  $DistRoot = Join-Path (Split-Path $common -Parent) 'dist'
}
# retile-archive\ beside the main checkout's folder.
if (-not $ArchiveRoot) { $ArchiveRoot = Join-Path (Split-Path (Split-Path $common -Parent) -Parent) 'retile-archive' }
New-Item -ItemType Directory -Force $DistRoot | Out-Null
$DistRoot = (Resolve-Path $DistRoot).Path
$Version = (Get-Content (Join-Path $Repo 'VERSION') -TotalCount 1).Trim()
$Commit = (& git -C $Repo rev-parse --short HEAD).Trim()
$Dirty = [bool](& git -C $Repo status --porcelain --untracked-files=no)
$Active = Join-Path $DistRoot '3re'
$Stage = Join-Path $DistRoot '.staging-3re'
$TC = (Get-Item (Join-Path $Repo '.toolchain')).FullName
if ((Get-Item $TC).Target) { $TC = [string](Get-Item $TC).Target }
$Tools = Join-Path $Repo '.toolchain\Qt\Tools'
$QtBin = Join-Path $Repo '.toolchain\Qt\6.8.3\mingw_64\bin'
$Build = Join-Path $Repo 'build\release'

Step "3re $Version ($Commit$(if ($Dirty) { ', uncommitted changes' })) -> $Active"
Step 'Verifying pinned stores'
if ((Native 'powershell' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Repo 'tools\stores.ps1'))) -ne 0) { Fail 'stores are not verified (tools\stores.ps1)' }

Step 'Building (Release)'
$env:PATH = "$Tools\mingw1310_64\bin;$Tools\CMake_64\bin;$Tools\Ninja;$env:PATH"
if ((Native 'cmake' @('--preset', 'release')) -ne 0) { Fail 'configure failed' }
if ((Native 'cmake' @('--build', '--preset', 'release')) -ne 0) { Fail 'build failed' }
if (-not $SkipTests) {
  Step 'Testing (whole suite)'
  # trellis-upgrade needs six real images: saved TRELLIS inputs of this machine unless given.
  if (-not $env:PIXEL3D_TRELLIS_IMAGE_CASES) {
    $cases = @(Get-ChildItem (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Pixel3D\Generated\Trellis-*\input.png'), (Join-Path ([Environment]::GetFolderPath('MyDocuments')) '3re\Projects\*\source\*') -File -ErrorAction SilentlyContinue |
      Sort-Object LastWriteTime -Descending | Select-Object -First 6 -ExpandProperty FullName)
    if ($cases.Count -lt 6) { Fail 'trellis-upgrade needs six saved images: set PIXEL3D_TRELLIS_IMAGE_CASES (file;file;...)' }
    $env:PIXEL3D_TRELLIS_IMAGE_CASES = $cases -join ';'
  }
  if ((Native 'ctest' @('--preset', 'release', '-j', '4', '--timeout', '1500')) -ne 0) { Fail 'tests failed' }
}

Step "Staging $Stage"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
$App = Join-Path $Stage 'app'
New-Item -ItemType Directory -Force $App, (Join-Path $App 'runtime'), (Join-Path $Stage 'models\trellis\q4'), (Join-Path $Stage 'models\sam2.1-tiny') | Out-Null
function Link([string]$src, [string]$dst) {
  if (-not (Test-Path -LiteralPath $src)) { Fail "missing $src" }
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
  $linked = $false
  if ((Split-Path -Qualifier $src) -eq (Split-Path -Qualifier $dst)) {
    try { New-Item -ItemType HardLink -Path $dst -Target $src -ErrorAction Stop | Out-Null; $linked = $true } catch { }
  }
  if (-not $linked) { Copy-Item -LiteralPath $src -Destination $dst }
}
Copy-Item (Join-Path $Build 'apps\3re.exe') $Stage
foreach ($f in @('apps\3re-studio.exe', 'apps\3re-cli.exe', 'apps\3re-mcp.exe', 'apps\pixel3d-asset-inspect.exe',
                 'apps\pixel3d-geometry-audit.exe', 'apps\pixel3d-cuda-health.exe', 'apps\pixel3d-auto-texture.exe', 'apps\pixel3d-retopo.exe',
                 'tools\pixel3d-glbcheck.exe', 'apps\3re-agent.exe', 'apps\pixel3d-decimate-guarded.exe',
                 'apps\pixel3d-mesh-fidelity.exe', 'apps\pixel3d-decimate-gltf.exe', 'apps\3re-chat.exe', 'apps\pixel3d-regional-fix.exe',
                 'apps\3re-update.exe')) {
  Copy-Item (Join-Path $Build $f) $App
}
# Updates and first-run downloads (docs/UPDATES.md): the model packs, their pinned sources and SHA-256.
Copy-Item (Join-Path $Repo 'apps\update\models.json') (Join-Path $App 'models.json')
# Game-Ready Asset Agent: the Khronos glTF validator (Apache-2.0) next to it, licence + notices below.
$Validator = Join-Path $TC 'gltf_validator\2.0.0-dev.3.10'
Copy-Item (Join-Path $Validator 'gltf_validator.exe') $App
if ((Native (Join-Path $QtBin 'windeployqt.exe') @('--release', '--no-translations', '--no-system-d3d-compiler', '--no-opengl-sw',
      '--no-quick-import', '--skip-plugin-types', 'networkinformation,generic', '--dir', $App, (Join-Path $App '3re-studio.exe'))) -ne 0) { Fail 'windeployqt failed' }
Get-ChildItem (Join-Path $TC 'trellis\runtime') -File | Where-Object { $_.Name -ne 'trellis-cli.exe' -and $_.Name -ne 'pixel3d-trellis-post-safe.exe' } |
  ForEach-Object { Link $_.FullName (Join-Path $App "runtime\$($_.Name)") }
# GeometrySafe cleanup: the path-free rebuild of the verified e281d0a4 (same outputs on 6 saved runs,
# .toolchain\trellis-post\safe-415020aa\BUILD.txt).
$PostSafe = Join-Path $TC 'trellis-post\safe-415020aa\pixel3d-trellis-post-safe.exe'
if (-not (Test-Path $PostSafe) -or (Get-FileHash -Algorithm SHA256 $PostSafe).Hash.ToLowerInvariant() -ne '415020aaf90c6930d3f5e10916813a68381f2e043afd8be82c2cae97b51eb307') {
  Fail "GeometrySafe cleanup store missing or changed: $PostSafe"
}
Link $PostSafe (Join-Path $App 'runtime\pixel3d-trellis-post-safe.exe')
# Engine: the texture-candidates fork of the pinned engine (agent milestone 2, B2; tools\trellis\engine-patch\,
# docs\agent\TEXTURE-CANDIDATES.md): the same outputs (verified byte-identical on 8 runs) plus the texture session
# and texture-only replays. Shipped as the generation engine and as trellis-tex-replay.exe for the agent.
# tex-candidates-4e19aa96 is the path-free rebuild of 61fd983f (same outputs; its BUILD.txt).
$Fork = Join-Path $TC 'trellis-fork\tex-candidates-4e19aa96\trellis-cli.exe'
if (-not (Test-Path $Fork) -or (Get-FileHash -Algorithm SHA256 $Fork).Hash.ToLowerInvariant() -ne '4e19aa96208ee56cebdc647aa34c605708c369d5eae553ca71dff3fc1a00e3ba') {
  Fail "TRELLIS texture-candidates engine store missing or changed: $Fork"
}
Link $Fork (Join-Path $App 'runtime\trellis-cli.exe')
Copy-Item $Fork (Join-Path $App 'runtime\trellis-tex-replay.exe')
Copy-Item (Join-Path $TC 'onnxruntime\1.22.0\lib\onnxruntime.dll') $App
# The Visual C++ runtime ONNX Runtime needs, app-local (no installer).
Get-ChildItem (Join-Path $TC 'msvc-runtime\14.51.36247') -File | ForEach-Object { Copy-Item $_.FullName $App }
$manifest = Join-Path $TC 'models\trellis\models-manifest.json'
Copy-Item $manifest (Join-Path $Stage 'models\trellis\models-manifest.json')
foreach ($e in (Get-Content -Raw $manifest | ConvertFrom-Json).files) {
  $rel = $e.path -replace '/', '\'
  Link (Join-Path $TC "models\trellis\$rel") (Join-Path $Stage "models\trellis\$rel")
}
foreach ($n in @('sam2.1_hiera_tiny.encoder.onnx', 'sam2.1_hiera_tiny.decoder.onnx', 'config.yaml')) {
  Link (Join-Path $TC "models\sam2.1_hiera_tiny_20260221\extracted\$n") (Join-Path $Stage "models\sam2.1-tiny\$n")
}
$post = Join-Path $App 'runtime\pixel3d-trellis-post-safe.exe'
$engine = Join-Path $App 'runtime\trellis-cli.exe'
$config = [ordered]@{
  format  = '3re-config'
  version = 1
  trellis = [ordered]@{
    runtime_dir      = 'runtime'
    models_dir       = '../models/trellis/q4'
    post_executable  = 'runtime/pixel3d-trellis-post-safe.exe'
    weights_manifest = '../models/trellis/models-manifest.json'
    engine_sha256    = (Get-FileHash -Algorithm SHA256 $engine).Hash.ToLowerInvariant()
    texture_replay_sha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $App 'runtime\trellis-tex-replay.exe')).Hash.ToLowerInvariant()
    post_sha256      = (Get-FileHash -Algorithm SHA256 $post).Hash.ToLowerInvariant()
    scope            = 'GeometrySafe: Gate A memory admission; Dev2 inference; intersection-safe cleanup (post-safe); native 512 + TRELLIS texture (hybrid); Hybrid/Auto Texture with hidden-side gate; Generated PBR session experimental'
  }
  sam     = [ordered]@{ dir = '../models/sam2.1-tiny'; onnxruntime = 'onnxruntime.dll' }
}
[IO.File]::WriteAllText((Join-Path $App '3re.config.json'), ($config | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))

# Smart Retopology engine: QuadWild-BiMDF (GPL-3.0) as separate programs in app\retopo, its licence in
# LICENSES\ and its corresponding source (upstream, pinned dependencies, 3re build fixes and patches,
# build notes) in Retopology-Sources\ (tools\smartquad\QUADWILD.md).
$QuadWild = Join-Path $TC 'quadwild-bimdf\3re-v5'  # path-free rebuild of 3re-v4 (sources\BUILD.txt)
foreach ($f in @('runtime\quadwild.exe', 'runtime\quad_from_patches.exe', 'runtime\config\main_config\flow.txt', 'LICENSE-QuadWild-GPL-3.0.txt',
                 'sources\source.zip', 'sources\v4-patches.zip', 'sources\BUILD.txt')) {
  if (-not (Test-Path (Join-Path $QuadWild $f))) { Fail "QuadWild engine store incomplete: $f" }
}
Copy-Item -Recurse (Join-Path $QuadWild 'runtime') (Join-Path $App 'retopo')
Copy-Item -Recurse (Join-Path $QuadWild 'sources') (Join-Path $Stage 'Retopology-Sources')

# Documentation, licences, examples.
Copy-Item (Join-Path $Repo 'docs\README-3re.txt') (Join-Path $Stage 'README.txt')
Copy-Item (Join-Path $Repo 'CHANGELOG.md'), (Join-Path $Repo 'THIRD-PARTY-NOTICES.txt') $Stage
New-Item -ItemType Directory -Force (Join-Path $Stage 'LICENSES\Qt') | Out-Null
Copy-Item (Join-Path $Repo 'LICENSES\*.txt') (Join-Path $Stage 'LICENSES')
Copy-Item (Join-Path $Repo '.toolchain\Qt\6.8.3\mingw_64\sbom\qtbase-6.8.3.spdx'), (Join-Path $Repo '.toolchain\Qt\6.8.3\mingw_64\sbom\qtsvg-6.8.3.spdx') (Join-Path $Stage 'LICENSES\Qt')
Copy-Item (Join-Path $TC 'onnxruntime\1.22.0\ThirdPartyNotices.txt') (Join-Path $Stage 'LICENSES\onnxruntime-ThirdPartyNotices.txt')
Copy-Item (Join-Path $Validator 'LICENSE') (Join-Path $Stage 'LICENSES\gltf-validator-Apache-2.0.txt')
Copy-Item (Join-Path $Validator 'NOTICES') (Join-Path $Stage 'LICENSES\gltf-validator-NOTICES.txt')
Copy-Item (Join-Path $QuadWild 'LICENSE-QuadWild-GPL-3.0.txt') (Join-Path $Stage 'LICENSES\QuadWild-GPL-3.0.txt')
foreach ($ex in @('A_synthetic_chair', 'C_asym_planter', 'D_character')) {
  New-Item -ItemType Directory -Force (Join-Path $Stage "examples\pixel-art\$ex") | Out-Null
  Copy-Item (Join-Path $Repo "tests\fixtures\$ex\*.png") (Join-Path $Stage "examples\pixel-art\$ex")
}
foreach ($ex in @('cloaked', 'humanoid')) {
  New-Item -ItemType Directory -Force (Join-Path $Stage "examples\low-poly\$ex") | Out-Null
  Copy-Item (Join-Path $Repo "tests\fixtures\E_lowpoly_character\$ex\front.png"), (Join-Path $Repo "tests\fixtures\E_lowpoly_character\$ex\side.png") (Join-Path $Stage "examples\low-poly\$ex")
}
if (Test-Path (Join-Path $Repo 'tests\fixtures\S_segmentation')) {
  New-Item -ItemType Directory -Force (Join-Path $Stage 'examples\image-to-3d') | Out-Null
  Copy-Item (Join-Path $Repo 'tests\fixtures\S_segmentation\*.png') (Join-Path $Stage 'examples\image-to-3d')
}

Step 'Self-test of the staged folder (only Windows on PATH)'
$selfJson = Join-Path $Stage 'self-test.json'
$savedPath = $env:PATH
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$p = Start-Process -FilePath (Join-Path $Stage '3re.exe') -ArgumentList @('--self-test', '--deep', '--json', "`"$selfJson`"") -Wait -PassThru -NoNewWindow
$env:PATH = $savedPath
if (-not (Test-Path $selfJson)) { Fail 'self-test produced no result' }
$self = Get-Content -Raw $selfJson | ConvertFrom-Json
foreach ($i in $self.items) { Write-Host ("  {0} {1} - {2}" -f $(if ($i.ok) { 'PASS' } elseif ($i.fatal) { 'FAIL' } else { 'WARN' }), $i.name, $i.detail) }
if ($p.ExitCode -ne 0 -or -not $self.passed) { Fail 'the staged release failed its self-test (dist\3re unchanged)' }
$foreign = Get-ChildItem $Stage -Recurse -File -Include *.json, *.txt, *.conf, *.ini, *.md |
  Select-String -SimpleMatch -Pattern 'Documents\Codex', '.claude\worktrees', 'retile\.toolchain', 'Documents\\Codex', '.claude\\worktrees', 'retile\\.toolchain' -List
if ($foreign) { Fail ("foreign paths in the release: " + (($foreign | ForEach-Object { $_.Path }) -join ', ')) }
Remove-Item $selfJson

# RELEASE.json: what this release is, file by file (models: the manifest pins them).
$files = Get-ChildItem $Stage -Recurse -File | Where-Object { $_.FullName -notlike (Join-Path $Stage 'models\*') } | ForEach-Object {
  [ordered]@{ path = $_.FullName.Substring($Stage.Length + 1) -replace '\\', '/'; size = $_.Length; sha256 = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash.ToLowerInvariant() }
}
$release = [ordered]@{
  product = '3re'; version = $Version; commit = $Commit; uncommitted_changes = $Dirty
  packaged = (Get-Date).ToString('s'); self_test = 'PASS'; tests = $(if ($SkipTests) { 'skipped' } else { 'PASS' })
  models = [ordered]@{ trellis = 'models/trellis/models-manifest.json'; sam = 'models/sam2.1-tiny (SHA-256 pinned in tools/stores.ps1)' }
  files = @($files)
}
[IO.File]::WriteAllText((Join-Path $Stage 'RELEASE.json'), ($release | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))
if ($FailAfterStage) { Fail 'stopped after staging (-FailAfterStage); dist\3re unchanged' }

Step 'Swapping the active release'
$rollback = $null
if (Test-Path $Active) {
  $prev = 'unknown'
  if (Test-Path (Join-Path $Active 'RELEASE.json')) { $prev = (Get-Content -Raw (Join-Path $Active 'RELEASE.json') | ConvertFrom-Json).version }
  $rollbackRoot = Join-Path $ArchiveRoot '3re-rollback'
  New-Item -ItemType Directory -Force $rollbackRoot | Out-Null
  $rollback = Join-Path $rollbackRoot ("{0}-{1}" -f $prev, (Get-Date -Format 'yyyyMMdd-HHmmss'))
  try { Move-Item -LiteralPath $Active -Destination $rollback }
  catch { Fail "could not move the current dist\3re aside (is 3re running? close it first): $($_.Exception.Message)" }
}
try { Move-Item -LiteralPath $Stage -Destination $Active }
catch {
  if ($rollback) { Move-Item -LiteralPath $rollback -Destination $Active }
  Fail "could not activate the new release; the previous one was restored: $($_.Exception.Message)"
}
Write-Host ""
Write-Host "3re $Version is the active release: $Active\3re.exe" -ForegroundColor Green
if ($rollback) { Write-Host "Previous release kept for rollback: $rollback" }
exit 0
