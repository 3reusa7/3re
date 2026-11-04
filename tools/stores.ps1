<#
.SYNOPSIS
  The canonical, verified inputs of the 3re release (docs/CANONICAL.md).

.DESCRIPTION
  3re ships binaries and models that are not built from this repository:
    .toolchain\trellis\runtime\          TRELLIS.2 CUDA runtime (trellis-cli + DLLs + the verified post-safe cleanup)
    .toolchain\models\trellis\q4\        TRELLIS.2 Q4 weights (+ ..\models-manifest.json)
    .toolchain\models\sam2.1_hiera_tiny_20260221\extracted\   SAM 2.1-tiny encoder/decoder ONNX
    .toolchain\onnxruntime\1.22.0\       ONNX Runtime CPU (official Microsoft release, signed DLL)
  Every file is pinned by SHA-256 below (weights: by the manifest's LFS oids).

  Without -Init the script only verifies (exit 0 = all present and matching).
  -Init imports missing files ONCE from their original locations, as NTFS hard
  links when on the same volume (no extra disk space, independent of the source
  path afterwards) or as copies. Sources are never moved, changed or deleted.
  -Deep re-hashes the weights too (2.5 GB, about a minute).
#>
param([switch]$Init, [switch]$Deep)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# Worktrees use a junction to the main checkout's toolchain; resolve it so links land there.
$TC = (Get-Item (Join-Path $Repo '.toolchain')).FullName
if ((Get-Item $TC).Target) { $TC = [string](Get-Item $TC).Target }

# -Init only: where the original files were imported from once (2026-10-20), given by the maintainer.
$RuntimeSource = "$env:THREERE_STORE_RUNTIME_SOURCE"
$WeightsSource = "$env:THREERE_STORE_WEIGHTS_SOURCE"
# TRELLIS texture weights (hybrid texture, 2026-10-20): same Q4 release, verified against the published LFS digests.
$TextureWeightsSource = "$env:THREERE_STORE_TEXTURE_WEIGHTS_SOURCE"
# After tools\archive_old_builds.ps1 the GeometrySafe build lives in the archive beside the main checkout.
if ($Init -and -not ($RuntimeSource -and (Test-Path $RuntimeSource))) {
  $common = (& git -C $Repo rev-parse --path-format=absolute --git-common-dir).Trim()
  $archiveRoot = Join-Path (Split-Path (Split-Path $common -Parent) -Parent) 'retile-archive'
  $archived = Get-ChildItem (Join-Path $archiveRoot 'builds-*\Pixel3D-1.2.4-GeometrySafe\TrellisRuntime') -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($archived) { $RuntimeSource = $archived.FullName }
}
$ManifestSource = if ($RuntimeSource) { Join-Path $RuntimeSource '..\models-manifest.json' } else { '' }

# Verified GeometrySafe runtime (2026-10-20). post-safe = the cleanup verified on real generations.
$Runtime = [ordered]@{
  'trellis-cli.exe'               = '7a80638a99d7ca7d4e423591bcb954896985f51538a5ec21eed2d6a61a5c45ee'
  'pixel3d-trellis-post-safe.exe' = 'e281d0a4693db5204a6cb583636494afd577ada9bba84d3d7b9bbcc024a64c9d'
  'cublas64_12.dll'               = '90052a83efd1b57a8e3616a6590b335855f81b814a4f16eecb7b5bf6d1b1d4eb'
  'cublasLt64_12.dll'             = 'c3a05ea244c937314afec09f87b91f814c7e27977681f6c67eb51bb06ced3a4a'
  'cudart64_12.dll'               = '760c38928bbe5759f7b31ed6692599eb7ec83cedd5702e84c2b72028a89837e1'
  'ggml-base.dll'                 = 'a145091b0f2f8114f7134dd9b2e86d8d086402ecce869ec362109d95ebe3300c'
  'ggml-cpu.dll'                  = '2fc337f0d1ca094cc9581f8e40419f8c271e62630e2cbf9722811eec811514d2'
  'ggml-cuda.dll'                 = '909a8998f136eef2365bc93457c95a2fd7f3ff7b974c7d7f3d26e9b73c2f15e0'
  'ggml.dll'                      = '4d46a2155df193cdaadac028ead27345cbb34c4ea0a5652d1c3d592e8acd82da'
  'libgcc_s_seh-1.dll'            = '5e27589147caa6d7a30f4cc058a14c455dc62233901ffff9a1797ede425e9f86'
  'libstdc++-6.dll'               = '8013488c5528bad7966ca07f3ea2e7a9b743cacb258fe76b46a326f821cc83b0'
  'libwinpthread-1.dll'           = 'c7c7dced65fff71c7bb61f80c771c562f533d26d72722d9d6091129a3b03d5ce'
  'msvcp140.dll'                  = '7c26614e1d733892c2deac7e245ce115504b1d80592dd0a01b08e3e5a55f89ca'
  'vcruntime140.dll'              = 'd1f4225df2cd877dbf130d5668a021dce3f94118455ff5ec952061c30afc9ce7'
  'vcruntime140_1.dll'            = 'a7146c08f89fe5b04541ab507cdb59ff7b44534d4ba3c668a426c6450a03434e'
}
$Sam = [ordered]@{
  'sam2.1_hiera_tiny.encoder.onnx' = '667384d1e686de6828b841ac8a24db0fafa2b3452494225f82eeedac56141230'
  'sam2.1_hiera_tiny.decoder.onnx' = 'c40f5aa7d37b681cd500481a85d44839fd81c93dce1e86271a2c866470d22105'
  'config.yaml'                    = '530b9c514078ac21ed3b0628cdcf2d4aa941b5b83bc259c032558b2edc31be13'
}
# Official release onnxruntime-win-x64-1.22.0.zip = 174c616efc0271194488642a72f1a514e01487da4dfe84c49296d66e40ebe0da
$Ort = [ordered]@{ 'lib\onnxruntime.dll' = '579b636403983254346a5c1d80bd28f1519cd1e284cd204f8d4ff41f8d711559' }
# Microsoft Visual C++ runtime 14.51.36247.0 that onnxruntime.dll needs, shipped app-local (redistributable).
$Msvc = [ordered]@{
  'msvcp140.dll'       = '7c26614e1d733892c2deac7e245ce115504b1d80592dd0a01b08e3e5a55f89ca'
  'msvcp140_1.dll'     = '206c931bf90fdad8816de3b5e2ef80b2bcaa9406c89ecc05fe6fddffe251e982'
  'vcruntime140.dll'   = 'd1f4225df2cd877dbf130d5668a021dce3f94118455ff5ec952061c30afc9ce7'
  'vcruntime140_1.dll' = 'a7146c08f89fe5b04541ab507cdb59ff7b44534d4ba3c668a426c6450a03434e'
}

$Dirs = @{
  Runtime  = Join-Path $TC 'trellis\runtime'
  Weights  = Join-Path $TC 'models\trellis'
  Sam      = Join-Path $TC 'models\sam2.1_hiera_tiny_20260221\extracted'
  Ort      = Join-Path $TC 'onnxruntime\1.22.0'
  Msvc     = Join-Path $TC 'msvc-runtime\14.51.36247'
}
$failures = New-Object System.Collections.Generic.List[string]

function Hash([string]$f) { (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLowerInvariant() }
function Import-File([string]$src, [string]$dst) {
  New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
  # A hard link costs no disk space; system-owned files cannot be linked and are copied.
  $linked = $false
  if ((Split-Path -Qualifier $src) -eq (Split-Path -Qualifier $dst)) {
    try { New-Item -ItemType HardLink -Path $dst -Target $src -ErrorAction Stop | Out-Null; $linked = $true } catch { }
  }
  if (-not $linked) { Copy-Item -LiteralPath $src -Destination $dst }
}
function Check-Pinned([string]$dir, $pins, [string]$sourceDir) {
  foreach ($name in $pins.Keys) {
    $f = Join-Path $dir $name
    if (-not (Test-Path -LiteralPath $f)) {
      if ($Init -and $sourceDir -and (Test-Path -LiteralPath (Join-Path $sourceDir $name))) { Import-File (Join-Path $sourceDir $name) $f }
      else { $failures.Add("missing $f"); continue }
    }
    if ((Hash $f) -ne $pins[$name]) { $failures.Add("SHA-256 mismatch $f") }
  }
}

Check-Pinned $Dirs.Runtime $Runtime $RuntimeSource
Check-Pinned $Dirs.Sam $Sam $null
Check-Pinned $Dirs.Ort $Ort $null
Check-Pinned $Dirs.Msvc $Msvc (Join-Path $env:SystemRoot 'System32')
# Path-free rebuilds (2026-11-03) that releases ship instead of the older builds above; same outputs on saved runs
# (each store's BUILD.txt). The older files stay as they are.
Check-Pinned (Join-Path $TC 'trellis-fork\tex-candidates-4e19aa96') ([ordered]@{ 'trellis-cli.exe' = '4e19aa96208ee56cebdc647aa34c605708c369d5eae553ca71dff3fc1a00e3ba' }) $null
Check-Pinned (Join-Path $TC 'trellis-post\safe-415020aa') ([ordered]@{ 'pixel3d-trellis-post-safe.exe' = '415020aaf90c6930d3f5e10916813a68381f2e043afd8be82c2cae97b51eb307' }) $null
Check-Pinned (Join-Path $TC 'quadwild-bimdf\3re-v5\runtime') ([ordered]@{
  'quadwild.exe'          = '2a459b242e61e96f8ed3d4aefffb22c55c85ed03e45ee306b9d46fecad9dbd2e'
  'quad_from_patches.exe' = 'c841478653ebcc5d5098b5f172209fadd07a3c89584d1a73ddefd24992b2c66a' }) $null

# Weights: the manifest (from the verified release) pins every file by size and LFS oid.
$manifest = Join-Path $Dirs.Weights 'models-manifest.json'
if (-not (Test-Path $manifest) -and $Init -and $ManifestSource) { Import-File (Resolve-Path $ManifestSource).Path $manifest }
if (-not (Test-Path $manifest)) { $failures.Add("missing $manifest") }
else {
  foreach ($e in (Get-Content -Raw $manifest | ConvertFrom-Json).files) {
    $f = Join-Path $Dirs.Weights ($e.path -replace '/', '\')
    if (-not (Test-Path -LiteralPath $f)) {
      $src = ''
      foreach ($from in @($WeightsSource, $TextureWeightsSource)) {
        if ($from -and (Test-Path -LiteralPath (Join-Path $from ($e.path -replace '/', '\')))) { $src = Join-Path $from ($e.path -replace '/', '\'); break }
      }
      if ($Init -and $src) { Import-File $src $f } else { $failures.Add("missing $f"); continue }
    }
    if ((Get-Item -LiteralPath $f).Length -ne [int64]$e.size) { $failures.Add("size mismatch $f") }
    elseif (($Deep -or $Init) -and (Hash $f) -ne $e.lfs.oid) { $failures.Add("SHA-256 mismatch $f") }
  }
}

if ($failures.Count) {
  Write-Host "STORES NOT READY:" -ForegroundColor Red
  $failures | ForEach-Object { Write-Host "  $_" }
  exit 1
}
Write-Host "Stores verified: runtime ($($Runtime.Count) files), weights, SAM 2.1-tiny, ONNX Runtime 1.22.0 ($TC)"
exit 0
