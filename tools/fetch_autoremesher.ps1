# Optional offline Smart Quad backend. Download once; processing stays offline.
# Keep the complete official DLL/plugin bundle together. Never commit it to Git.
$ErrorActionPreference = 'Stop'
$Version = '1.2.0'
$Sha256 = 'f6184622cef84f0bcf032a0474df44e2076f4c10c159f1af0cb7230259af7476'
$DependencyDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.toolchain/autoremesher/$Version"))
$Exe = Join-Path $DependencyDir 'autoremesher.exe'
if (Test-Path -LiteralPath $Exe) { Write-Output "AutoRemesher already available: $Exe"; exit 0 }
if (([IO.DriveInfo]::new([IO.Path]::GetPathRoot($DependencyDir))).AvailableFreeSpace -lt 1GB) {
  throw 'At least 1 GiB free disk space is required for dependency setup.'
}
New-Item -ItemType Directory -Force $DependencyDir | Out-Null
$Archive = Join-Path $DependencyDir "autoremesher-$Version-win32-x86_64.zip"
if (-not (Test-Path -LiteralPath $Archive)) {
  Invoke-WebRequest -Uri "https://github.com/huxingyi/autoremesher/releases/download/$Version/autoremesher-$Version-win32-x86_64.zip" -OutFile $Archive
}
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Sha256) {
  throw 'AutoRemesher checksum mismatch; dependency was not installed.'
}
Expand-Archive -LiteralPath $Archive -DestinationPath $DependencyDir
if (-not (Test-Path -LiteralPath $Exe)) { throw 'Verified archive is missing the expected executable.' }
@{ version=$Version; archive_sha256=$Sha256; executable_sha256=(Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash.ToLowerInvariant() } |
  ConvertTo-Json | Set-Content (Join-Path $DependencyDir 'verified-package.json')
Write-Output "AutoRemesher $Version installed: $Exe"
