<#
.SYNOPSIS
  Downloads the pinned third-party inputs the build and the tests need into .toolchain\ (CI and new checkouts).

.DESCRIPTION
  -OnnxRuntime   official onnxruntime-win-x64-1.22.0.zip (headers + onnxruntime.dll) -> .toolchain\onnxruntime\1.22.0
  -Sam           SAM 2.1 tiny ONNX (Hugging Face, fixed revision) -> .toolchain\models\sam2.1_hiera_tiny_20260221\extracted
  -GltfValidator Khronos glTF-Validator (tools\fetch_gltf_validator.ps1)
  Every download is checked against its SHA-256 before it is unpacked; present inputs are skipped.
#>
param([switch]$OnnxRuntime, [switch]$Sam, [switch]$GltfValidator)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$TC = Join-Path $Repo '.toolchain'
$Dl = Join-Path $TC 'dl'
New-Item -ItemType Directory -Force $Dl | Out-Null

function Get-Pinned([string]$url, [string]$file, [string]$sha256) {
  if (-not ((Test-Path $file) -and (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower() -eq $sha256)) {
    Write-Host "download $url"
    & curl.exe -sSfL --retry 5 -o $file $url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $url" }
  }
  $got = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
  if ($got -ne $sha256) { Remove-Item $file -Force; throw "SHA-256 mismatch for $url (got $got, expected $sha256)" }
}

if ($OnnxRuntime) {
  $dst = Join-Path $TC 'onnxruntime\1.22.0'
  if (-not (Test-Path (Join-Path $dst 'include\onnxruntime_c_api.h'))) {
    $zip = Join-Path $Dl 'onnxruntime-win-x64-1.22.0.zip'
    Get-Pinned 'https://github.com/microsoft/onnxruntime/releases/download/v1.22.0/onnxruntime-win-x64-1.22.0.zip' $zip '174c616efc0271194488642a72f1a514e01487da4dfe84c49296d66e40ebe0da'
    $tmp = Join-Path $Dl 'ort-x'
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive -Path $zip -DestinationPath $tmp
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    Move-Item (Join-Path $tmp 'onnxruntime-win-x64-1.22.0') $dst
    Remove-Item -Recurse -Force $tmp
  }
  $dll = Join-Path $dst 'lib\onnxruntime.dll'
  if ((Get-FileHash -Algorithm SHA256 $dll).Hash.ToLower() -ne '579b636403983254346a5c1d80bd28f1519cd1e284cd204f8d4ff41f8d711559') { throw "onnxruntime.dll does not match its pin" }
  Write-Host "ONNX Runtime 1.22.0 ready: $dst"
}

if ($Sam) {
  $dst = Join-Path $TC 'models\sam2.1_hiera_tiny_20260221\extracted'
  $pins = @{
    'sam2.1_hiera_tiny.encoder.onnx' = '667384d1e686de6828b841ac8a24db0fafa2b3452494225f82eeedac56141230'
    'sam2.1_hiera_tiny.decoder.onnx' = 'c40f5aa7d37b681cd500481a85d44839fd81c93dce1e86271a2c866470d22105'
    'config.yaml'                    = '530b9c514078ac21ed3b0628cdcf2d4aa941b5b83bc259c032558b2edc31be13'
  }
  if (-not (Test-Path (Join-Path $dst 'sam2.1_hiera_tiny.encoder.onnx'))) {
    $zip = Join-Path $Dl 'sam2.1_hiera_tiny_20260221.zip'
    Get-Pinned 'https://huggingface.co/vietanhdev/segment-anything-2.1-onnx-models/resolve/6a3ac868340a3196a349050a6efae22a5acc0330/sam2.1_hiera_tiny_20260221.zip' $zip 'c602cb3f6fd297312a415f885f0df3f0eef9fbb334b069c9d30e828f2ae7c69a'
    New-Item -ItemType Directory -Force $dst | Out-Null
    Expand-Archive -Path $zip -DestinationPath $dst -Force
  }
  foreach ($n in $pins.Keys) {
    if ((Get-FileHash -Algorithm SHA256 (Join-Path $dst $n)).Hash.ToLower() -ne $pins[$n]) { throw "$n does not match its pin" }
  }
  Write-Host "SAM 2.1 tiny ready: $dst"
}

if ($GltfValidator) {
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Repo 'tools\fetch_gltf_validator.ps1')
  if ($LASTEXITCODE -ne 0) { throw 'glTF-Validator fetch failed' }
}
