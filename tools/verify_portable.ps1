<#
.SYNOPSIS
  Proves dist\3re is portable: a real copy (no hard links) outside the repository,
  started with only Windows on PATH, passes `3re.exe --self-test --deep` and renders
  its Home page (--screenshot). The copy is removed afterwards unless -Keep.
#>
param(
  [string]$Release = '',
  [string]$Target = (Join-Path $env:TEMP ("3re-portable-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
  [string]$ScreenshotOut = '',
  [switch]$Keep
)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Release) {
  $common = (& git -C $Repo rev-parse --path-format=absolute --git-common-dir).Trim()
  $Release = Join-Path (Split-Path $common -Parent) 'dist\3re'
}
if (-not (Test-Path (Join-Path $Release '3re.exe'))) { Write-Host "No release at $Release" -ForegroundColor Red; exit 1 }
Write-Host "Copying $Release -> $Target"
& robocopy $Release $Target /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Write-Host "copy failed ($LASTEXITCODE)" -ForegroundColor Red; exit 1 }
$saved = $env:PATH
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$json = Join-Path $Target 'portable-self-test.json'
$p = Start-Process -FilePath (Join-Path $Target '3re.exe') -ArgumentList @('--self-test', '--deep', '--json', "`"$json`"") -Wait -PassThru -NoNewWindow
$shot = if ($ScreenshotOut) { $ScreenshotOut } else { Join-Path $Target 'portable-home.png' }
$s = Start-Process -FilePath (Join-Path $Target '3re.exe') -ArgumentList @('--no-recovery', '--screenshot', "`"$shot`"", '--page', 'home') -Wait -PassThru -NoNewWindow
$env:PATH = $saved
$self = Get-Content -Raw $json | ConvertFrom-Json
foreach ($i in $self.items) { Write-Host ("  {0} {1} - {2}" -f $(if ($i.ok) { 'PASS' } elseif ($i.fatal) { 'FAIL' } else { 'WARN' }), $i.name, $i.detail) }
$ok = ($p.ExitCode -eq 0) -and $self.passed -and ($s.ExitCode -eq 0) -and (Test-Path $shot)
Write-Host ("Self-test exit {0}, screenshot exit {1} ({2})" -f $p.ExitCode, $s.ExitCode, $shot)
if (-not $Keep) {
  if ($ScreenshotOut -eq '' -and (Test-Path $shot)) { Copy-Item $shot (Join-Path $env:TEMP 'portable-home.png') -Force }
  Remove-Item -Recurse -Force $Target
}
if ($ok) { Write-Host "PORTABLE: PASS" -ForegroundColor Green; exit 0 }
Write-Host "PORTABLE: FAIL" -ForegroundColor Red; exit 1
