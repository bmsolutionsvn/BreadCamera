param(
  [ValidateSet('release', 'debug')][string]$Preset = 'release',
  [string]$Filter = ''
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Preset $Preset -SkipUi
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$exe = Join-Path $PSScriptRoot "..\build\$Preset\bc_tests.exe"
if ($Filter) { & $exe "--test-case=$Filter" } else { & $exe }
exit $LASTEXITCODE
