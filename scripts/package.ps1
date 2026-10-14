#Requires -Version 5.1
<#
  package.ps1 - build the UI, build the release binaries, then compile the Inno Setup installer.

  Adaptation recorded for controller ruling M5-4 (see
  .superpowers/sdd/2026-10-08-m5-hardening/task-4-report.md):

    * NPM_TOKEN is optional.  The UI's private scoped dependency @bmsolutionsvn/design lives on
      npm.pkg.github.com and needs a registry token.  The brief's unconditional
      `$env:NODE_AUTH_TOKEN = (gh auth token)` hard-fails on a machine without `gh`.  Here a
      token is used only when one is available (an inherited NODE_AUTH_TOKEN, else `gh auth token`
      when gh is installed): that path runs a clean `npm ci`.  When no token is available and
      ui/node_modules already exists, the existing install is reused instead of failing.  A
      properly credentialed machine still takes the `npm ci` path.
#>
param([string]$Version = '0.1.0')
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')

# ---- UI ----
Push-Location (Join-Path $repo 'ui')
try {
  $env:PATH = "C:\Program Files\nodejs;$env:PATH"

  # `npm` may resolve to npm.ps1, which is blocked under a Restricted execution policy.  Prefer
  # npm.cmd so the script works whether or not the caller passed -ExecutionPolicy Bypass.
  $npm = 'npm'
  if (Get-Command npm.cmd -ErrorAction SilentlyContinue) { $npm = 'npm.cmd' }

  # M5-4: resolve a token if we can; do not fail when we cannot.
  $token = $env:NODE_AUTH_TOKEN
  if (-not $token -and (Get-Command gh -ErrorAction SilentlyContinue)) {
    try { $token = (gh auth token 2>$null) } catch { $token = $null }
  }

  if ($token) {
    $env:NODE_AUTH_TOKEN = $token
    & $npm ci
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
  } elseif (Test-Path (Join-Path (Get-Location) 'node_modules')) {
    Write-Warning 'No NODE_AUTH_TOKEN/gh token available: skipping "npm ci" and reusing the existing ui/node_modules.'
  } else {
    throw 'A registry token is required for a clean npm ci (set NODE_AUTH_TOKEN or install gh and authenticate).'
  }

  & $npm run build
  if ($LASTEXITCODE) { exit $LASTEXITCODE }
} finally {
  Pop-Location
}

# ---- release binaries ----
& (Join-Path $PSScriptRoot 'build.ps1') -Preset release
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# ---- stage the freshly built UI ----
# The UI is built by npm, not CMake.  CMakeLists.txt stages ui/dist next to BreadCamera.exe only
# as a POST_BUILD step on the shell target, so an incremental run where the shell is already up to
# date leaves the previous (possibly stale, possibly sync-conflict-tainted) copy in place.  Stage
# the new build explicitly here and drop the old directory, mirroring cmake/CopyUi.cmake.
$uiSrc = Join-Path $repo 'ui\dist'
$uiDst = Join-Path $repo 'build\release\ui'
if (-not (Test-Path (Join-Path $uiSrc 'index.html'))) {
  throw "The UI build produced no dist/index.html ($uiSrc); run the UI build first."
}
if (Test-Path $uiDst) { Remove-Item $uiDst -Recurse -Force }
Copy-Item -Path $uiSrc -Destination $uiDst -Recurse -Force

# ---- Visual C++ runtime ----
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$redist = Get-ChildItem "$vs\VC\Redist\MSVC\*\vc_redist.x64.exe" | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $redist) { throw 'vc_redist.x64.exe not found in the Visual Studio install' }

# ---- Inno Setup compiler ----
# ISCC installs per machine (Program Files) or per user (LocalAppData\Programs), depending on how
# it was installed; winget's per-user install is not on PATH in a fresh session.  Check all of them.
function Find-Iscc {
  foreach ($p in @(
      "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
      "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
      "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")) {
    if ($p -and (Test-Path $p)) { return $p }
  }
  $cmd = Get-Command ISCC.exe -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  return $null
}
$iscc = Find-Iscc
if (-not $iscc) {
  winget install --id JRSoftware.InnoSetup -e --silent --accept-package-agreements --accept-source-agreements
  $iscc = Find-Iscc
}
if (-not $iscc) {
  throw "ISCC.exe (Inno Setup 6) not found; install it (winget install --id JRSoftware.InnoSetup -e) and re-run."
}

& $iscc "/DAppVersion=$Version" "/DVcRedist=$($redist.FullName)" (Join-Path $repo 'installer\BreadCamera.iss')
exit $LASTEXITCODE