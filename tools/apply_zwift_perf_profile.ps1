# zvr_loader.exe now does this by itself ([setup] vr_graphics in zwift_vr.ini; "Restore Zwift graphics.cmd" undoes
# it). This script is kept for doing it by hand, e.g. without the loader. Same files, same backup names.
#
# Replace Zwift's graphics profiles (ultra/high/medium/basic) with the VR profile (config\zwift-vr-graphics.txt),
# or restore Zwift's originals. Run from an ADMIN PowerShell (Zwift is installed under Program Files). Close Zwift first.
#
#   powershell -ExecutionPolicy Bypass -File <repo>\tools\apply_zwift_perf_profile.ps1
#   powershell -ExecutionPolicy Bypass -File <repo>\tools\apply_zwift_perf_profile.ps1 -Restore
#
# Zwift picks one of its profiles from the GPU it detects, and which one differs per PC (an RTX 4090 gets ultra; a
# laptop GPU may not), so every profile gets the same VR file. The VR profile turns off what costs CPU/GPU or is
# uncomfortable in a headset: screen-space reflections, ambient occlusion, sun rays, bloom, radial speed blur and
# headlights; water uses simple reflections; less foliage and earlier low-detail models; 512 shadow maps (the mod
# already skips shadow drawing with [render] shadows = 0, since Zwift has no shadows-off switch).
# Whole-file copies only (no text editing); each original is backed up once as <name>.txt.zwiftvr-backup.
# A Zwift update may restore its own profiles - just run this again.
param([switch]$Restore)
$ErrorActionPreference = 'Stop'
$cfgDir = 'C:\Program Files (x86)\Zwift\data\configs'
$profiles = 'ultra.txt', 'high.txt', 'medium.txt', 'basic.txt'
$vr = Join-Path (Split-Path $PSScriptRoot -Parent) 'config\zwift-vr-graphics.txt'

if (Get-Process -Name ZwiftApp -ErrorAction SilentlyContinue) { throw 'Close Zwift first (it reads the profile at start).' }
if (-not $Restore -and -not (Test-Path $vr)) { throw "VR profile not found: $vr" }
foreach ($name in $profiles) {
  $target = Join-Path $cfgDir $name
  $backup = "$target.zwiftvr-backup"
  if ($Restore) {
    if (Test-Path $backup) { Copy-Item -LiteralPath $backup -Destination $target -Force; "Restored $name" }
    else { "No backup for $name - left as is." }
  } else {
    if (-not (Test-Path $target)) { "Zwift has no $name - skipped."; continue }
    if (-not (Test-Path $backup)) { Copy-Item -LiteralPath $target -Destination $backup }
    Copy-Item -LiteralPath $vr -Destination $target -Force
    "Installed the VR profile as $name (original kept as $name.zwiftvr-backup)"
  }
}
