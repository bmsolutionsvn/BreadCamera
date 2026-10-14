param([double]$Hours = 0.5, [switch]$Faults)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Preset release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$out = Join-Path $env:TEMP ("bcam-soak-" + (Get-Date -Format 'yyyyMMdd-HHmm') + '.csv')
$exe = Join-Path $PSScriptRoot '..\build\release\bc_soak.exe'
$argsList = @('--minutes', [int]($Hours * 60), '--out', $out)
if ($Faults) { $argsList += '--faults' }
& $exe @argsList
$code = $LASTEXITCODE
Write-Host "CSV: $out"
exit $code