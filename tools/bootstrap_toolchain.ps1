<#
.SYNOPSIS
  Fetches the portable development toolchain into .toolchain\ (no admin rights needed).

.DESCRIPTION
  Downloads from Qt's official online repository with resume support and SHA-256
  verification, then extracts with Windows' built-in tar.exe (bsdtar reads 7z):
    - MinGW-w64 GCC 13.1 (Qt's mingw1310_64 build)
    - CMake 3.30 and Ninja 1.12
    - Qt 6.8.3 win64_mingw (skipped with -SkipQt; only needed from milestone M3)
  Re-running is safe: components that are already present are skipped.
  End users never need this script; it is for building Pixel3D.
#>
param(
  [switch]$SkipQt,
  [string]$Repo = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tc = Join-Path $root ".toolchain"
$dl = Join-Path $tc "dl"
$out = Join-Path $tc "Qt"
New-Item -ItemType Directory -Force $dl, $out | Out-Null

function Get-PackageInfo([string]$repoDir, [string]$packageName) {
  $xml = [xml](& curl.exe -sS -L --retry 5 "$Repo/$repoDir/Updates.xml")
  $pkg = $xml.Updates.PackageUpdate | Where-Object { $_.Name -eq $packageName } | Select-Object -First 1
  if (-not $pkg) { throw "package $packageName not found in $repoDir" }
  $archives = @("$($pkg.DownloadableArchives)".Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ })
  return @{ Version = $pkg.Version; Archives = $archives }
}

function Get-VerifiedArchive([string]$url, [string]$file) {
  $expected = ((& curl.exe -sS -L --retry 5 "$url.sha256") -split "\s+")[0].ToLower()
  if ($expected.Length -ne 64) { throw "no sha256 for $url" }
  for ($i = 1; $i -le 60; $i++) {
    if (Test-Path $file) {
      $actual = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
      if ($actual -eq $expected) { return }
    }
    Write-Host "  downloading $(Split-Path -Leaf $file) (attempt $i)"
    & curl.exe -sS -L -C - --connect-timeout 20 --speed-limit 20000 --speed-time 30 -o $file $url
    if (Test-Path $file) {
      $actual = (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower()
      if ($actual -eq $expected) { return }
      $size = (Get-Item $file).Length
      # A complete-but-wrong file cannot be resumed: start over.
      if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 33) { if ($size -gt 0) { Remove-Item $file } }
    }
  }
  throw "failed to download $url"
}

function Install-Package([string]$repoDir, [string]$packageName, [string]$probe, [string[]]$only = @()) {
  if (Test-Path (Join-Path $out $probe)) { Write-Host "ok   $packageName (present)"; return }
  Write-Host "get  $packageName"
  $info = Get-PackageInfo $repoDir $packageName
  foreach ($a in $info.Archives) {
    if ($only.Count -gt 0 -and -not ($only | Where-Object { $a.StartsWith($_) })) { continue }
    $name = "$($info.Version)$a"
    $file = Join-Path $dl $name
    Get-VerifiedArchive "$Repo/$repoDir/$packageName/$name" $file
    & "$env:SystemRoot\System32\tar.exe" -xf $file -C $out
    if ($LASTEXITCODE -ne 0) { throw "extract failed: $file" }
  }
  if (-not (Test-Path (Join-Path $out $probe))) { throw "$packageName installed but $probe is missing" }
}

Install-Package "tools_mingw1310" "qt.tools.win64_mingw1310" "Tools\mingw1310_64\bin\g++.exe"
Install-Package "tools_cmake" "qt.tools.cmake" "Tools\CMake_64\bin\cmake.exe"
Install-Package "tools_ninja" "qt.tools.ninja" "Tools\Ninja\ninja.exe"
if (-not $SkipQt) {
  Install-Package "qt6_683/qt6_683" "qt.qt6.683.win64_mingw" "6.8.3\mingw_64\lib\cmake\Qt6Widgets\Qt6WidgetsConfig.cmake" `
    @("qtbase", "qttools", "qttranslations", "MinGW-w64", "d3dcompiler", "opengl32sw")
  $qtconf = Join-Path $out "6.8.3\mingw_64\bin\qt.conf"
  if (-not (Test-Path $qtconf)) { Set-Content -Encoding ascii $qtconf "[Paths]`r`nPrefix=..`r`n" }
}
Write-Host "toolchain ready in $out"
