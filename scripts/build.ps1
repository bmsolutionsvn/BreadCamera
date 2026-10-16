param(
  [ValidateSet('release', 'debug')][string]$Preset = 'release',
  [switch]$SkipUi
)
$ErrorActionPreference = 'Stop'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
Set-Location $repo

# ---- UI ----
# BreadCamera.exe serves the React app from a "ui" folder beside itself, and CMake copies ui/dist
# there only when the shell target relinks.  So an incremental run where only the UI changed would
# leave a stale copy and the change would never reach the app; build the UI here and stage it
# explicitly (mirrors scripts/package.ps1).  -SkipUi is for fast C++-only iterations and for the
# callers (test, soak, package) that do not need or already manage the UI.
$uiDist = Join-Path $repo 'ui\dist'
$uiDst = Join-Path $repo "build\$Preset\ui"
if (-not $SkipUi) {
  Push-Location (Join-Path $repo 'ui')
  try {
    $env:PATH = "C:\Program Files\nodejs;$env:PATH"
    # `npm` may resolve to npm.ps1, blocked under a Restricted execution policy; prefer npm.cmd.
    $npm = 'npm'
    if (Get-Command npm.cmd -ErrorAction SilentlyContinue) { $npm = 'npm.cmd' }
    if (-not (Test-Path 'node_modules')) {
      # A clean install needs a registry token for the private @bmsolutionsvn/design package.
      $token = $env:NODE_AUTH_TOKEN
      if (-not $token -and (Get-Command gh -ErrorAction SilentlyContinue)) {
        try { $token = (gh auth token 2>$null) } catch { $token = $null }
      }
      if (-not $token) {
        throw 'ui/node_modules is missing and no registry token is available; run "npm ci" in ui/ first (set NODE_AUTH_TOKEN or authenticate gh).'
      }
      $env:NODE_AUTH_TOKEN = $token
      & $npm ci
      if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    & $npm run build
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
  } finally {
    Pop-Location
  }
}

# ---- C++ ----
cmake --preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# ---- stage the fresh UI next to the exe ----
# Explicit, so it runs even when the shell target is already up to date (the CMake POST_BUILD copy
# only fires on a relink).  Drop the old directory first, like package.ps1 and CopyUi.cmake.
if (-not $SkipUi) {
  if (-not (Test-Path (Join-Path $uiDist 'index.html'))) {
    throw "The UI build produced no dist/index.html ($uiDist)."
  }
  if (Test-Path $uiDst) { Remove-Item $uiDst -Recurse -Force }
  Copy-Item -Path $uiDist -Destination $uiDst -Recurse -Force
  Write-Host "UI staged to $uiDst"
}
exit 0
