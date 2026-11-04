<#
.SYNOPSIS
  Stages the public 3re release — no AI models (downloaded on first use, app\models.json) — self-tests it and
  writes the portable zip. Used by .github/workflows/release.yml; runs locally too. Never touches dist\3re
  (that is tools\package.ps1, the maintainer's release with bundled models).

.PARAMETER Build     CMake build folder (default build\release).
.PARAMETER Inputs    The unpacked build inputs zip (tools\release\build-inputs.json lists and pins every file).
.PARAMETER QtBin     Qt's bin folder (windeployqt.exe).
.PARAMETER Out       Output folder: Out\3re\ (the staged release) and Out\3re-<version>-windows-x64.zip.
#>
param(
  [string]$Build = 'build\release',
  [Parameter(Mandatory = $true)][string]$Inputs,
  [Parameter(Mandatory = $true)][string]$QtBin,
  [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
function Fail([string]$why) { Write-Host "PACKAGE STOPPED: $why" -ForegroundColor Red; exit 1 }
function Hash([string]$f) { (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLowerInvariant() }
$Build = (Resolve-Path (Join-Path $Repo $Build)).Path
$Inputs = (Resolve-Path $Inputs).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$Version = (Get-Content (Join-Path $Repo 'VERSION') -TotalCount 1).Trim()
$TC = Join-Path $Repo '.toolchain'

Write-Host "== Checking the build inputs"
$pins = Get-Content -Raw (Join-Path $Repo 'tools\release\build-inputs.json') | ConvertFrom-Json
foreach ($f in $pins.files) {
  $p = Join-Path $Inputs ($f.path -replace '/', '\')
  if (-not (Test-Path -LiteralPath $p) -or (Hash $p) -ne $f.sha256) { Fail "build input missing or changed: $($f.path)" }
}

$Stage = Join-Path $Out '3re'
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
$App = Join-Path $Stage 'app'
New-Item -ItemType Directory -Force $App, (Join-Path $App 'runtime'), (Join-Path $Stage 'models\trellis') | Out-Null

Write-Host "== Staging $Stage"
Copy-Item (Join-Path $Build 'apps\3re.exe') $Stage
foreach ($f in @('apps\3re-studio.exe', 'apps\3re-cli.exe', 'apps\3re-mcp.exe', 'apps\pixel3d-asset-inspect.exe',
                 'apps\pixel3d-geometry-audit.exe', 'apps\pixel3d-cuda-health.exe', 'apps\pixel3d-auto-texture.exe', 'apps\pixel3d-retopo.exe',
                 'tools\pixel3d-glbcheck.exe', 'apps\3re-agent.exe', 'apps\pixel3d-decimate-guarded.exe',
                 'apps\pixel3d-mesh-fidelity.exe', 'apps\pixel3d-decimate-gltf.exe', 'apps\3re-chat.exe', 'apps\pixel3d-regional-fix.exe',
                 'apps\3re-update.exe')) {
  Copy-Item (Join-Path $Build $f) $App
}
Copy-Item (Join-Path $Repo 'apps\update\models.json') (Join-Path $App 'models.json')
$Validator = Join-Path $TC 'gltf_validator\2.0.0-dev.3.10'
Copy-Item (Join-Path $Validator 'gltf_validator.exe') $App
& (Join-Path $QtBin 'windeployqt.exe') --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-quick-import `
  --skip-plugin-types 'networkinformation,generic' --dir $App (Join-Path $App '3re-studio.exe') | Out-Host
if ($LASTEXITCODE -ne 0) { Fail 'windeployqt failed' }
Copy-Item (Join-Path $TC 'onnxruntime\1.22.0\lib\onnxruntime.dll') $App
Get-ChildItem (Join-Path $Inputs 'msvc') -File | ForEach-Object { Copy-Item $_.FullName $App }

# The engine: 3re's trellis.cpp fork and the verified cleanup; its GPU libraries are downloaded on first use.
$Runtime = Join-Path $App 'runtime'
Copy-Item (Join-Path $Inputs 'engine\trellis-cli.exe') $Runtime
Copy-Item (Join-Path $Inputs 'engine\trellis-cli.exe') (Join-Path $Runtime 'trellis-tex-replay.exe')
Copy-Item (Join-Path $Inputs 'engine\pixel3d-trellis-post-safe.exe') $Runtime
foreach ($n in @('libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) { Copy-Item (Join-Path $Inputs "engine\$n") $Runtime }
foreach ($n in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) { Copy-Item (Join-Path $Inputs "msvc\$n") $Runtime }

# The weights manifest the engine checks, written from the pinned catalog (same format as the store's).
$catalog = Get-Content -Raw (Join-Path $Repo 'apps\update\models.json') | ConvertFrom-Json
$trellis = $catalog.packs | Where-Object { $_.id -eq 'trellis-q4' }
$weights = [ordered]@{
  files    = @($trellis.files | ForEach-Object { [ordered]@{ path = ($_.path -replace '^models/trellis/', ''); size = $_.size; lfs = [ordered]@{ oid = $_.sha256; size = $_.size } } })
  revision = 'a57397bd3d351599d9729fc144b3f87c3f87d65b'
}
[IO.File]::WriteAllText((Join-Path $Stage 'models\trellis\models-manifest.json'), ($weights | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))

$engine = Join-Path $Runtime 'trellis-cli.exe'
$post = Join-Path $Runtime 'pixel3d-trellis-post-safe.exe'
$config = [ordered]@{
  format  = '3re-config'
  version = 1
  trellis = [ordered]@{
    runtime_dir           = 'runtime'
    models_dir            = '../models/trellis/q4'
    post_executable       = 'runtime/pixel3d-trellis-post-safe.exe'
    weights_manifest      = '../models/trellis/models-manifest.json'
    engine_sha256         = (Hash $engine)
    texture_replay_sha256 = (Hash (Join-Path $Runtime 'trellis-tex-replay.exe'))
    post_sha256           = (Hash $post)
    scope                 = 'GeometrySafe: Gate A memory admission; Dev2 inference; intersection-safe cleanup (post-safe); native 512 + TRELLIS texture (hybrid); Hybrid/Auto Texture with hidden-side gate; Generated PBR session experimental'
  }
  sam     = [ordered]@{ dir = '../models/sam2.1-tiny'; onnxruntime = 'onnxruntime.dll' }
}
[IO.File]::WriteAllText((Join-Path $App '3re.config.json'), ($config | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))

# Smart Retopology: QuadWild-BiMDF (GPL-3.0) as separate programs, with its corresponding source.
Copy-Item -Recurse (Join-Path $Inputs 'quadwild\runtime') (Join-Path $App 'retopo')
Copy-Item -Recurse (Join-Path $Inputs 'quadwild\sources') (Join-Path $Stage 'Retopology-Sources')

# Documentation, licences, examples.
Copy-Item (Join-Path $Repo 'docs\README-3re.txt') (Join-Path $Stage 'README.txt')
foreach ($n in @('CHANGELOG.md', 'THIRD-PARTY-NOTICES.txt', 'LICENSE', 'NOTICE')) { Copy-Item (Join-Path $Repo $n) $Stage }
New-Item -ItemType Directory -Force (Join-Path $Stage 'LICENSES\Qt') | Out-Null
Copy-Item (Join-Path $Repo 'LICENSES\*.txt') (Join-Path $Stage 'LICENSES')
foreach ($n in @('qtbase-6.8.3.spdx', 'qtsvg-6.8.3.spdx')) {
  $sbom = Join-Path (Split-Path $QtBin -Parent) "sbom\$n"
  if (Test-Path $sbom) { Copy-Item $sbom (Join-Path $Stage 'LICENSES\Qt') } else { Write-Host "note: Qt SBOM $n not in this Qt installation" }
}
Copy-Item (Join-Path $TC 'onnxruntime\1.22.0\ThirdPartyNotices.txt') (Join-Path $Stage 'LICENSES\onnxruntime-ThirdPartyNotices.txt')
Copy-Item (Join-Path $Validator 'LICENSE') (Join-Path $Stage 'LICENSES\gltf-validator-Apache-2.0.txt')
Copy-Item (Join-Path $Validator 'NOTICES') (Join-Path $Stage 'LICENSES\gltf-validator-NOTICES.txt')
Copy-Item (Join-Path $Inputs 'quadwild\LICENSE-QuadWild-GPL-3.0.txt') (Join-Path $Stage 'LICENSES\QuadWild-GPL-3.0.txt')
foreach ($ex in @('A_synthetic_chair', 'C_asym_planter', 'D_character')) {
  New-Item -ItemType Directory -Force (Join-Path $Stage "examples\pixel-art\$ex") | Out-Null
  Copy-Item (Join-Path $Repo "tests\fixtures\$ex\*.png") (Join-Path $Stage "examples\pixel-art\$ex")
}
foreach ($ex in @('cloaked', 'humanoid')) {
  New-Item -ItemType Directory -Force (Join-Path $Stage "examples\low-poly\$ex") | Out-Null
  Copy-Item (Join-Path $Repo "tests\fixtures\E_lowpoly_character\$ex\front.png"), (Join-Path $Repo "tests\fixtures\E_lowpoly_character\$ex\side.png") (Join-Path $Stage "examples\low-poly\$ex")
}
New-Item -ItemType Directory -Force (Join-Path $Stage 'examples\image-to-3d') | Out-Null
Copy-Item (Join-Path $Repo 'tests\fixtures\S_segmentation\*.png') (Join-Path $Stage 'examples\image-to-3d')

Write-Host "== Self-test of the staged folder (only Windows on PATH; models not downloaded yet = warnings)"
$selfJson = Join-Path $Out 'self-test.json'
$savedPath = $env:PATH
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$p = Start-Process -FilePath (Join-Path $Stage '3re.exe') -ArgumentList @('--self-test', '--json', "`"$selfJson`"") -Wait -PassThru -NoNewWindow
$env:PATH = $savedPath
if (-not (Test-Path $selfJson)) { Fail 'self-test produced no result' }
$self = Get-Content -Raw $selfJson | ConvertFrom-Json
foreach ($i in $self.items) { Write-Host ("  {0} {1} - {2}" -f $(if ($i.ok) { 'PASS' } elseif ($i.fatal) { 'FAIL' } else { 'WARN' }), $i.name, $i.detail) }
if ($p.ExitCode -ne 0 -or -not $self.passed) { Fail 'the staged release failed its self-test' }

Write-Host "== RELEASE.json"
$commit = (& git -C $Repo rev-parse --short HEAD).Trim()
$files = Get-ChildItem $Stage -Recurse -File | Sort-Object FullName | ForEach-Object {
  [ordered]@{ path = $_.FullName.Substring($Stage.Length + 1) -replace '\\', '/'; size = $_.Length; sha256 = (Hash $_.FullName) }
}
$release = [ordered]@{
  product = '3re'; version = $Version; commit = $commit; packaged = (Get-Date).ToUniversalTime().ToString('s') + 'Z'; self_test = 'PASS'
  models = 'downloaded on first use: app/models.json'
  files = @($files)
}
[IO.File]::WriteAllText((Join-Path $Stage 'RELEASE.json'), ($release | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding $false))

Write-Host "== Zip"
$zip = Join-Path $Out "3re-$Version-windows-x64.zip"
& python -I (Join-Path $Repo 'tools\release\zip_tree.py') $Stage $zip
if ($LASTEXITCODE -ne 0) { Fail 'zip failed' }
Write-Host "3re $Version packaged: $zip"
exit 0
