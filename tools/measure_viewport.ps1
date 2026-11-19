# SPDX-License-Identifier: Apache-2.0
# tools/measure_viewport.ps1 [-Scene b1m,b5m,s3_textured,a1_furnace] [-Orbit 10] [-Rest 9] [-Rebuild] [-Summarise]
# One-off measurement of task 096 (deleted when it lands): the viewport update rate on the M1a bench scenes
# (0009 B-14), the present cost (B-13) and the batching schedule (B-34, 0007 V23) through
# tools/viewport_rate_probe.py, one fresh windowed Blender per scene, each under the machine lock
# (Invoke-ArBlender -Gui). a1_furnace is the B-13 run: its pass is far shorter than a display frame, so the
# producer is the display and view_draw's time is the present's.
# Output: out/viewport/rate_<scene>.json per run, the tables below, history lines in
# out/bench/history-viewport-<yyyyMMdd>.jsonl (the lead commits them to bench/history/), the orbit screenshots
# out/evidence/096/orbit_<scene>.png (Blender's own screenshot operator) and the rate plot
# out/evidence/096/viewport_rate_b1m.png (tools/report.py --viewport-png).
param(
  [string[]]$Scene = @('b1m', 'b5m', 's3_textured', 'a1_furnace'),
  [double]$Orbit = 10,
  [double]$Rest = 9,
  [switch]$Rebuild,
  [switch]$Summarise
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/proc.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
$out = Join-Path $root 'out/viewport'
$ev = Join-Path $root 'out/evidence/096'
New-Item -ItemType Directory -Force $out, $ev | Out-Null
$bench = @{ b1m = 'b1m'; b5m = 'b5m'; s3_textured = 'b_tx'; a1_furnace = 'a1'; a1_paced = 'a1_paced60' }   # run key -> bench id
# One run per scene; the B-13 run (a1_paced) is a1_furnace again with the newest frame offered at 60 Hz.
$Keys = @($Scene) + $(if ($Scene -contains 'a1_furnace') { @('a1_paced') } else { @() })

function Build-Blend([string]$name) {
  $blend = Join-Path $out "$name.blend"
  if ((Test-Path $blend) -and -not $Rebuild) { return $blend }
  $py = Get-ArPython
  $extra = if ($name -eq 'a1_furnace') { @('--sampler', 'sobol') } else { @() }   # a .blend cannot carry another sampler
  & $py -I (Join-Path $root "scenes/$name/build.py") --out (Join-Path $out "$name.arrec") --blend $blend @extra
  if ($LASTEXITCODE -ne 0) { throw "scene builder $name failed" }
  $null = Invoke-ArBlender -Script (Join-Path $root 'scenes/lib/blend_writer.py') -TimeoutS 900 -Args @('--no-extension', "$blend.json", $blend)
  if ($LASTEXITCODE -ne 0) { throw "blend_writer for $name failed" }
  return $blend
}

if (-not $Summarise) {
  foreach ($name in $Keys) {
    $paced = $name -eq 'a1_paced'
    $blend = Build-Blend $(if ($paced) { 'a1_furnace' } else { $name })
    $more = if ($paced) { @('--pace', '60', '--frames', '360', '--skip', '60', '--rest', '1') } else { @('--rest', "$Rest") }   # B-13: 60 warm-up and 300 paced frames
    $r = Invoke-ArBlender -Gui -Script (Join-Path $root 'tools/viewport_rate_probe.py') -TimeoutS 400 -Args (@(
        '--scene', $name, '--blend', $blend, '--out', (Join-Path $out "rate_$name.json"), '--orbit', "$Orbit",
        '--shot', (Join-Path $ev "orbit_$name.png")) + $more) 6>$null
    '{0}: blender exit {1}' -f $name, $r.ExitCode
  }
}

$date = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
$sha = (git rev-parse --short HEAD).Trim()
$gpu = (& nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>$null) -split ','
$driver = if ($gpu.Count -ge 2) { $gpu[1].Trim() } else { 'unknown' }
$hist = @()
function Add-Hist($scene, $metric, $value, $unit) {
  $script:hist += ([ordered]@{ sha = $sha; machine = $env:COMPUTERNAME; driver = $driver; cuda = 'cuda'; scene = $bench[$scene]
    metric = $metric; value = [math]::Round([double]$value, 3); unit = $unit; n = 1; spread = 0; date = $date } | ConvertTo-Json -Compress)
}
function Read-Run([string]$name) {
  $f = Join-Path $out "rate_$name.json"
  if (Test-Path $f) { return (Get-Content $f -Raw | ConvertFrom-Json) }
  "$name : no result"
  return $null
}

'--- B-14: presented updates per second during a continuous orbit, full 1080p region ---'
foreach ($name in $Keys) {
  $o = Read-Run $name
  if ($null -eq $o) { continue }
  $s = $o.stats
  if ($o.errors.Count) { "  ERRORS ${name}: $($o.errors -join ' | ')" }
  if ($null -eq $s -or $null -eq $s.rate_mean) { continue }
  '{0,-12} region {1}x{2}, {3} presented in {4:N1} s: mean {5:N1}/s median {6:N1}/s p5 {7:N1}/s min {8:N1}/s; longest run below 24/s {9:N3} s (breach {10}); fewest frames in 0.5 s {11}; view_draws {12:N0}/s, set_views {13:N0}/s; engine pass (last_pass_ms) median {14:N1} ms' -f `
    $name, $o.region[0], $o.region[1], $s.presented, $s.window_s, $s.rate_mean, $s.rate_median, $s.rate_p5, $s.rate_min, $s.longest_below_floor_s, $s.floor_breach, $s.min_frames_in_half_second, $s.draws_per_s, $s.set_views_per_s, $s.pass_ms_median
  Add-Hist $name 'viewport.rate_mean' $s.rate_mean '1/s'
  Add-Hist $name 'viewport.rate_median' $s.rate_median '1/s'
  Add-Hist $name 'viewport.rate_p5' $s.rate_p5 '1/s'
  Add-Hist $name 'viewport.longest_below_floor' $s.longest_below_floor_s 's'
  if ($s.present) {
    Add-Hist $name 'viewport.view_draw_median' $s.present.view_draw_ms_median 'ms'
    Add-Hist $name 'viewport.view_draw_p99' $s.present.view_draw_ms_p99 'ms'
  }
}
'--- B-13: view_draw per presented frame (ms), CPU wall time inside view_draw ---'
foreach ($name in $Keys) {
  $o = Read-Run $name
  $p = if ($o -and $o.stats) { $o.stats.present } else { $null }
  if ($p) { '{0,-12} n {1}, view_draw median {2:N2} p99 {3:N2} max {4:N2}; upload median {5:N2}; draw median {6:N2}' -f $name, $p.n, $p.view_draw_ms_median, $p.view_draw_ms_p99, $p.view_draw_ms_max, $p.upload_ms_median, $p.draw_ms_median }
}
'--- V23: passes per update during the orbit, and the schedule after it (B-34) ---'
foreach ($name in $Keys) {
  $o = Read-Run $name
  $s = if ($o) { $o.stats } else { $null }
  if (-not $s -or -not $s.rest) { continue }
  '{0,-12} orbit: one pass {1}, two {2}, more {3} (max {4}); after: {5} frames, rate after 0.5 s {6:N1}/s, most in one second {7}' -f `
    $name, $s.orbit_passes_per_update.one, $s.orbit_passes_per_update.two, $s.orbit_passes_per_update.more, $s.orbit_passes_per_update.max, $s.rest.frames, $s.rest.rate_after_half_second, $s.rest.max_frames_in_a_second
  foreach ($b in $s.rest.buckets) {
    '    {0}-{1} s after the last view change: schedule {2:N2} s, observed interval median {3:N3} s, passes per update median {4}, n {5}, samples at the end {6}' -f $b.from_s, $b.to_s, $b.schedule_interval_s, $b.interval_s_median, $b.passes_median, $b.n, $b.spp_end
  }
}
if ($hist.Count -gt 0) {
  $hp = Join-Path $root ('out/bench/history-viewport-' + (Get-Date -Format 'yyyyMMdd') + '.jsonl')
  New-Item -ItemType Directory -Force (Split-Path $hp) | Out-Null
  Set-Content -Path $hp -Value $hist
  "history lines: $hp ($($hist.Count))"
}
& (Get-ArPython) -I (Join-Path $root 'tools/report.py') --viewport-png
