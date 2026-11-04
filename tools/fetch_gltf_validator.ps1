# Fetches the Khronos glTF-Validator (Apache-2.0) used by the tests as an
# independent check of skinned glTF exports. Pinned version; the archive's
# SHA-256 is pinned here because upstream publishes no digest (recorded on the
# first download, 2026-10-13). Not redistributed: it lives in .toolchain/ only.
$ErrorActionPreference = 'Stop'
$Version = '2.0.0-dev.3.10'
$Sha256 = 'c5068f51205deedc28acc3529ee7e11ee60e853454f673093398eba80142202c'
$Root = Join-Path $PSScriptRoot '..\.toolchain\gltf_validator'
$Dir = Join-Path $Root $Version
$Zip = Join-Path $Dir "gltf_validator-$Version-win64.zip"
$Exe = Join-Path $Dir 'gltf_validator.exe'
if (Test-Path $Exe) { Write-Host "glTF-Validator $Version already present: $Exe"; exit 0 }
New-Item -ItemType Directory -Force $Dir | Out-Null
$Url = "https://github.com/KhronosGroup/glTF-Validator/releases/download/$Version/gltf_validator-$Version-win64.zip"
Invoke-WebRequest -Uri $Url -OutFile $Zip -UseBasicParsing
$Actual = (Get-FileHash -Algorithm SHA256 $Zip).Hash.ToLower()
if ($Actual -ne $Sha256) {
  Remove-Item $Zip -Force
  throw "glTF-Validator checksum mismatch: expected $Sha256, got $Actual"
}
Expand-Archive -Path $Zip -DestinationPath $Dir -Force
Write-Host "glTF-Validator $Version installed: $Exe"
