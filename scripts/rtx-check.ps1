# Read-only probe for an NVIDIA machine. Installs nothing. Writes rtx-report.txt next to this script.
$out = Join-Path $PSScriptRoot 'rtx-report.txt'
$lines = New-Object System.Collections.Generic.List[string]
function Add($t) { $lines.Add($t); Write-Host $t }

Add "== BreadCamera RTX check $(Get-Date -Format s) =="
$os = Get-CimInstance Win32_OperatingSystem
Add "OS: $($os.Caption) build $($os.BuildNumber)"
Get-CimInstance Win32_VideoController | ForEach-Object { Add "Adapter: $($_.Name) driver $($_.DriverVersion)" }

$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if ($smi) {
  Add "-- nvidia-smi"
  (& nvidia-smi --query-gpu=name,driver_version,memory.total,compute_cap --format=csv,noheader) | ForEach-Object { Add $_ }
} else { Add "nvidia-smi: MISSING (install the NVIDIA driver)" }

foreach ($dll in 'nvcuda.dll', 'nvEncodeAPI64.dll', 'nvcuvid.dll') {
  Add ("{0,-20} {1}" -f $dll, $(if (Test-Path "$env:windir\System32\$dll") { 'present' } else { 'MISSING' }))
}
$ff = Get-Command ffmpeg -ErrorAction SilentlyContinue
if ($ff) { Add "ffmpeg h264_nvenc: $((& ffmpeg -hide_banner -encoders 2>$null | Select-String h264_nvenc) -ne $null)" } else { Add "ffmpeg: not installed (fine)" }

Add "-- tools (all optional for this check)"
foreach ($c in 'git', 'node', 'nvcc', 'cmake') {
  $x = Get-Command $c -ErrorAction SilentlyContinue
  Add ("{0,-8} {1}" -f $c, $(if ($x) { $x.Source } else { 'not installed' }))
}
Add ("Visual Studio: " + $(if (Test-Path "$env:ProgramFiles\Microsoft Visual Studio") { (Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Directory).Name -join ', ' } else { 'none' }))
Add ("OpenSSH server: " + $(if (Get-Service sshd -ErrorAction SilentlyContinue) { 'installed' } else { 'not installed' }))
Add ("Tailscale: " + $(if (Get-Command tailscale -ErrorAction SilentlyContinue) { 'installed' } else { 'not installed' }))

$lines | Set-Content -Encoding utf8 $out
Write-Host "`nReport saved to $out"
