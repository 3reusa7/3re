# Re-downloads the libwebp release vendored in third_party/libwebp and checks it the way it
# was checked when it was vendored: pinned SHA-256 of the archive, then the GPG signature by
# the WebP release signing key (fingerprint pinned here). Needs gpg (Git for Windows has it).
# Downloads go to .toolchain/dl/libwebp (never into the repository). Exit 0 = verified.
$ErrorActionPreference = 'Stop'
$Version = '1.6.0'
$Sha256 = 'e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564'
$Fingerprint = '6B0E6B70976DE303EDF2F601F9C3D6BDB8232B5D'
$Base = 'https://storage.googleapis.com/downloads.webmproject.org/releases/webp'
$Dir = Join-Path $PSScriptRoot '..\.toolchain\dl\libwebp'
New-Item -ItemType Directory -Force $Dir | Out-Null
$Tar = Join-Path $Dir "libwebp-$Version.tar.gz"
$Asc = "$Tar.asc"
$Key = Join-Path $Dir 'webp_release_key.asc'
Invoke-WebRequest -Uri "$Base/libwebp-$Version.tar.gz" -OutFile $Tar -UseBasicParsing
Invoke-WebRequest -Uri "$Base/libwebp-$Version.tar.gz.asc" -OutFile $Asc -UseBasicParsing
$Actual = (Get-FileHash -Algorithm SHA256 $Tar).Hash.ToLower()
if ($Actual -ne $Sha256) { throw "libwebp archive checksum mismatch: expected $Sha256, got $Actual" }
Invoke-WebRequest -Uri "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x$Fingerprint" -OutFile $Key -UseBasicParsing
$Gpg = (Get-Command gpg -ErrorAction SilentlyContinue).Source
if (-not $Gpg) { $Gpg = Join-Path $env:ProgramFiles 'Git\usr\bin\gpg.exe' }
if (-not (Test-Path $Gpg)) { throw 'gpg not found (install Git for Windows or GnuPG)' }
# Git's gpg is an MSYS program: give it /c/... paths.
function Msys([string]$p) {
  $full = (Resolve-Path $p).Path -replace '\\', '/'
  if ($full -match '^([A-Za-z]):(.*)$') { return '/' + $Matches[1].ToLower() + $Matches[2] }
  return $full
}
# gpg reports progress on stderr: not an error for PowerShell.
$ErrorActionPreference = 'Continue'
$Home2 = Join-Path $Dir 'gnupg'
New-Item -ItemType Directory -Force $Home2 | Out-Null
& $Gpg --homedir (Msys $Home2) --batch --import (Msys $Key) 2>&1 | Out-Null
$out = & $Gpg --homedir (Msys $Home2) --batch --status-fd 1 --verify (Msys $Asc) (Msys $Tar) 2>&1 | Out-String
if ($out -notmatch "VALIDSIG $Fingerprint") { throw "libwebp signature is not valid for key $Fingerprint`n$out" }
Write-Host "libwebp $Version verified: SHA-256 $Sha256, signed by $Fingerprint"
