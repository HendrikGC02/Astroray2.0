# SPDX-License-Identifier: Apache-2.0
# tools/sync_timing.ps1 [-Runs 3] [-History] [-Profile] [-Rebuild]
# B-31's four cells on S1 (b1m) and S2 (b5m), cold and warm (tools/sync_timing.py): writes each scene's .blend once
# into out/sync_timing/, then per run starts one fresh Blender per scene under the machine lock, and prints the
# table (median over runs, <= 20 lines; full log out/check/sync_timing.log). -History also writes one history line
# per cell into out/bench/history-<yyyyMMdd>.jsonl (the lead commits them to bench/history/). Needs a quiet machine.
param([int]$Runs = 3, [switch]$History, [switch]$Profile, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
$py = Get-ArPython
$dir = Join-Path $root 'out/sync_timing'
New-Item -ItemType Directory -Force $dir | Out-Null
foreach ($s in 'b1m', 'b5m') {
  $blend = Join-Path $dir "$s.blend"
  if ($Rebuild -or -not (Test-Path $blend)) {
    & $py -I (Join-Path $root "scenes/$s/build.py") --out (Join-Path $dir "$s.arrec") --blend $blend | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "scenes/$s/build.py failed" }
    $r = Invoke-ArBlender -Script (Join-Path $root 'scenes/lib/blend_writer.py') -Args @("--no-extension", "$blend.json", $blend) -TimeoutS 600
    if ($r.ExitCode -ne 0) { throw "blend_writer for $s exited $($r.ExitCode)" }
  }
}
Get-ChildItem $dir -Filter '*_r*.json' | Remove-Item
$extra = if ($Profile) { @('--profile') } else { @() }
for ($k = 1; $k -le $Runs; $k++) {
  foreach ($s in 'b1m', 'b5m') {
    $r = Invoke-ArBlender -Script (Join-Path $root 'tools/sync_timing.py') -TimeoutS 600 `
      -Args (@('measure', (Join-Path $dir "$s.blend"), $s, (Join-Path $dir "${s}_r$k.json")) + $extra)
    if ($r.ExitCode -ne 0) { throw "sync_timing measure $s run $k exited $($r.ExitCode)" }
  }
}
$rep = @('report', $dir) + $(if ($History) { '--history' }) + $(if ($Profile) { '--profile' })
& $py -I (Join-Path $root 'tools/sync_timing.py') @rep
exit $LASTEXITCODE
