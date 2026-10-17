# Downloads pinned third-party binaries into third_party/ (git-ignored). Safe to re-run.
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..\third_party'
New-Item -ItemType Directory -Force $root | Out-Null

function Get-NuGet($name, $id, $version, $sha256, [hashtable]$copy) {
  $dest = Join-Path $root $name
  if (Test-Path (Join-Path $dest '.ok')) { Write-Host "$name already present"; return }
  $zip = Join-Path $env:TEMP "$id.$version.zip"
  Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/$($id.ToLower())/$version/$($id.ToLower()).$version.nupkg" -OutFile $zip
  $actual = (Get-FileHash $zip -Algorithm SHA256).Hash
  if ($sha256 -and $actual -ne $sha256) { throw "${name}: SHA-256 mismatch ($actual)" }
  if (-not $sha256) { Write-Warning "${name}: no pinned hash yet; actual SHA-256 is $actual - pin it in this script" }
  $tmp = Join-Path $env:TEMP "$name-x"
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  Expand-Archive $zip $tmp
  foreach ($from in $copy.Keys) {
    $to = Join-Path $dest $copy[$from]
    New-Item -ItemType Directory -Force $to | Out-Null
    Copy-Item -Recurse -Force (Join-Path $tmp $from) $to
  }
  New-Item (Join-Path $dest '.ok') -ItemType File | Out-Null
}

Get-NuGet 'onnxruntime' 'Microsoft.ML.OnnxRuntime.DirectML' '1.24.4' '57E9F11B73437BEF7A309496135D4C1F96B1A8E9DDBA60013FA27BFC1D788681' @{
  'build\native\include\*' = 'include'
  'runtimes\win-x64\native\onnxruntime.lib' = 'lib'
  'runtimes\win-x64\native\onnxruntime.dll' = 'bin'
}
Get-NuGet 'directml' 'Microsoft.AI.DirectML' '1.15.4' '4E7CB7DDCE8CF837A7A75DC029209B520CA0101470FCDF275C1F49736A3615B9' @{ 'bin\x64-win\DirectML.dll' = 'bin' }

$model = Join-Path $root 'models\modnet.onnx'
if (-not (Test-Path $model)) {
  New-Item -ItemType Directory -Force (Split-Path $model) | Out-Null
  Invoke-WebRequest -Uri 'https://huggingface.co/Xenova/modnet/resolve/fa2fa546052fba4c08921230a26cc69a333fca12/onnx/model.onnx' -OutFile $model
}
$h = (Get-FileHash $model -Algorithm SHA256).Hash.ToLower()
if ($h -ne '07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9') { Remove-Item $model; throw "modnet.onnx: SHA-256 mismatch ($h)" }
