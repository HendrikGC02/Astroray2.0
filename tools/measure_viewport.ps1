# SPDX-License-Identifier: Apache-2.0
# tools/measure_viewport.ps1 -Scene b1m,b5m,b_tx,b_foliage,b1m_m1b [-Runs 3] [-Orbit 10] [-Rest 9] [-Dry] [-Rebuild] [-Summarise]
# B-14 (0009 d13, ruling 30): the viewport's presented updates per second at 1080p during a scripted orbit, per scene.
# For every scene and run one fresh windowed Blender (Invoke-ArBlender -Gui, so the machine lock) runs
# tools/viewport_rate_probe.py (maximised Rendered view, orbit -Orbit s, rest -Rest s; the probe's docstring says what it
# reads). Scene ids are bench ids (bench/scenes.toml): b1m, b5m, b_tx (scenes/s3_textured), b_foliage, b1m_m1b; a scene's
# .blend is written once into out/viewport/blend/ (scenes/<dir>/build.py, scenes/lib/blend_writer.py).
# Output, in -OutDir (default out/viewport): <scene>_r<k>.json per run, rate_<scene>.json (the median run, which
# tools/report.py plots, plus the table of runs) and, under out/evidence/214, Blender's screenshot of run 1, which the probe takes as the orbit ends (before the rest).
# History lines go to out/bench/history-viewport-<yyyyMMdd>.jsonl (the lead commits them to bench/history/):
# b14.rate (median over runs of each run's median rate), b14.rate_p5 (median of the 5th percentiles), b14.floor_longest_s
# (the longest run of intervals above 1/24 s over all runs; B-14 allows 0.5). A run that sees other build or render
# processes records their names in its json (busy_before, busy_after) and the summary line says "busy".
# -Dry: a 2 s orbit and 1 s rest into out/tmp/214/dry, no history lines, to prove the flow. -Summarise: read the run
# files again and print the table and the lines (tests/tools/fast_viewport_runner.ps1 uses it on canned files with
# -HistoryOut, -Sha, -Date, and -Cli, -Recording for the machine id). A run with errors, without a median rate or whose
# region is not 1920x1080 is a FAIL: the scene gets no history line and the exit code is 1. The history machine id is
# read from the CLI's bench line before any capture (a minimal CUDA run under the machine lock), so a missing id fails at
# once; -Dry writes no lines and reads none.
param(
  [string[]]$Scene = @('b1m'),
  [int]$Runs = 3,
  [double]$Orbit = 10,
  [double]$Rest = 9,
  [switch]$Dry,
  [switch]$Rebuild,
  [switch]$Summarise,
  [string]$OutDir = '',
  [string]$HistoryOut = '',
  [string]$Cli = '',        # default out/bin/astroray_cli.exe
  [string]$Recording = '',  # the .arrec the machine-id bench run reads; default the first scene's in out/viewport/blend
  [string]$Sha = '',
  [string]$Date = '',
  [int]$TimeoutS = 600
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$Scene = @($Scene | ForEach-Object { $_ -split ',' })   # -File passes a comma list as one string
$dirs = @{ b1m = 'b1m'; b5m = 'b5m'; b_tx = 's3_textured'; b_foliage = 'b_foliage'; b1m_m1b = 'b1m_m1b' }
$bad = @($Scene | Where-Object { -not $dirs.ContainsKey($_) })
if ($bad) { Write-Host "FAIL measure_viewport: unknown scene $($bad -join ', '); known: $($dirs.Keys | Sort-Object | ForEach-Object { $_ })"; exit 2 }
if ($Runs -lt 1) { Write-Host 'FAIL measure_viewport: -Runs must be at least 1'; exit 2 }
if (-not $OutDir) { $OutDir = if ($Dry) { Join-Path $root 'out/tmp/214/dry' } else { Join-Path $root 'out/viewport' } }
New-Item -ItemType Directory -Force $OutDir | Out-Null
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/proc.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
function Get-BenchMachineId([string]$Cli, [string]$Rec) {
  # perf.ps1's neutral id, read from the CLI's own bench line of a minimal run (one source; cli/bench.cpp hardware_id).
  # The run uses the GPU, so it holds the machine lock like perf.ps1 does.
  $null = Enter-ArMachineLock -Job other -CeilingS 300
  try { $o = @(& $Cli bench $Rec --size 320x180 --passes 1 --warmup 0 --device cuda 2>&1 | ForEach-Object { "$_" }) }
  finally { Exit-ArMachineLock }
  $line = $o | Where-Object { $_ -like '{*' } | Select-Object -First 1
  $id = if ($line) { ($line | ConvertFrom-Json).machine } else { $null }
  if (-not $id) { throw "the CLI printed no bench line to read the machine id from ($Cli bench $Rec): $($o | Select-Object -Last 3)" }
  return $id
}

function Median([double[]]$v) {
  if ($v.Count -eq 0) { return $null }
  $s = $v | Sort-Object; $n = $s.Count
  if ($n % 2) { return [double]$s[($n - 1) / 2] } else { return ([double]$s[$n / 2 - 1] + [double]$s[$n / 2]) / 2 }
}

function Get-Busy {
  $names = 'cl', 'link', 'nvcc', 'ninja', 'cmake', 'ctest', 'astroray_cli', 'astroray_tests_cpu', 'astroray_tests_gpu', 'blender'
  return @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $names -contains $_.Name } | ForEach-Object { $_.Name }) -join ','
}

if (-not $Summarise) {
  $blendDir = Join-Path $root 'out/viewport/blend'
  New-Item -ItemType Directory -Force $blendDir | Out-Null
  $failed = 0
  # B-14 is at full 1080p and the probe maximises the window on its monitor: put it on a 1920 x 1080 display, or refuse.
  Add-Type -AssemblyName System.Windows.Forms
  $mon = [Windows.Forms.Screen]::AllScreens | Where-Object { $_.Bounds.Width -eq 1920 -and $_.Bounds.Height -eq 1080 } | Select-Object -First 1
  if (-not $mon) { Write-Host 'FAIL measure_viewport: no 1920x1080 display; B-14 is defined at 1080p'; exit 1 }
  $geom = @('--window-geometry', "$($mon.Bounds.X)", "$($mon.Bounds.Y)", '1920', '1080')
  foreach ($name in $Scene) {
    $blend = Join-Path $blendDir "$name.blend"
    if ($Rebuild -or -not (Test-Path $blend)) {
      $py = Get-ArPython
      & $py -I (Join-Path $root "scenes/$($dirs[$name])/build.py") --out (Join-Path $blendDir "$name.arrec") --blend $blend
      if ($LASTEXITCODE -ne 0) { throw "scene builder $name failed" }
      Remove-Item $blend -ErrorAction SilentlyContinue   # a failed writer must not leave the last .blend to be measured
      # b1m_m1b's GRATING material needs the add-on registered in the writer's Blender, so its Blender job enables the
      # extension first (tests/blender/smoke_a1.py, as the probe does); the others are written as measure_ttff.ps1 does.
      if ($name -eq 'b1m_m1b') {
        $ext = Join-Path $blendDir 'write_blend_ext.py'
        Set-Content -Path $ext -Value @'
import sys
root, desc, out = sys.argv[sys.argv.index("--") + 1:]
sys.path[:0] = [root + "/tests/blender", root + "/scenes/lib"]
import smoke_a1
import blend_writer
smoke_a1.install_and_enable()
blend_writer.build(desc, out)
'@
        $null = Invoke-ArBlender -Script $ext -TimeoutS 900 -Args @($root, "$blend.json", $blend)
      } else {
        $null = Invoke-ArBlender -Script (Join-Path $root 'scenes/lib/blend_writer.py') -TimeoutS 900 -Args @('--no-extension', "$blend.json", $blend)
      }
      if (-not (Test-Path $blend)) { throw "the .blend writer for $name failed (no $blend)" }
    }
  }
  # The machine id before any capture: a CLI that prints no bench line fails here, not after the runs.
  if (-not $Dry) { $Machine = Get-BenchMachineId $(if ($Cli) { $Cli } else { Join-Path $root 'out/bin/astroray_cli.exe' }) $(if ($Recording) { $Recording } else { Join-Path $blendDir "$($Scene[0]).arrec" }) }
  foreach ($name in $Scene) {
    $blend = Join-Path $blendDir "$name.blend"
    for ($k = 1; $k -le $Runs; $k++) {
      $res = Join-Path $OutDir "${name}_r$k.json"
      Remove-Item $res -ErrorAction SilentlyContinue
      $shot = if ($k -eq 1) { @('--shot', (Join-Path $root "out/evidence/214/$name.png")) } else { @() }
      $before = Get-Busy
      $ret = Invoke-ArBlender -Gui -Script (Join-Path $root 'tools/viewport_rate_probe.py') -TimeoutS $TimeoutS `
        -BlenderArgs $geom -Args (@('--scene', $name, '--blend', $blend, '--out', $res, '--orbit', "$Orbit", '--rest', "$Rest") + $shot) 6>$null
      $r = @($ret | Where-Object { $_ -is [hashtable] })[-1]   # the launcher also prints the log tail; the summary is one line per scene
      if (-not (Test-Path $res)) { Write-Host "FAIL measure_viewport: $name r${k} wrote no result (exit $($r.ExitCode)); log $($r.Log)"; $failed++; continue }
      $o = Get-Content $res -Raw | ConvertFrom-Json
      $o | Add-Member -NotePropertyName busy_before -NotePropertyValue $before -Force
      $o | Add-Member -NotePropertyName busy_after -NotePropertyValue (Get-Busy) -Force
      $o | ConvertTo-Json -Depth 12 | Set-Content $res
    }
  }
}

$lines = @()
$date = if ($Date) { $Date } else { (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') }
$sha = if ($Sha) { $Sha } else { (git rev-parse HEAD).Trim() }
if ($Summarise -and -not $Dry) { $Machine = Get-BenchMachineId $(if ($Cli) { $Cli } else { Join-Path $root 'out/bin/astroray_cli.exe' }) $(if ($Recording) { $Recording } else { Join-Path $root "out/viewport/blend/$($Scene[0]).arrec" }) }
$gpu = (& nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>$null) -split ','
$driver = if ($gpu.Count -ge 2) { $gpu[1].Trim() } else { 'unknown' }
function New-Line([string]$scene, [string]$metric, [double]$value, [string]$unit, [int]$n, [double]$spread) {
  return ([ordered]@{ sha = $sha; machine = $machine; driver = $driver; cuda = 'cuda'; scene = $scene; metric = $metric
      value = [math]::Round($value, 2); unit = $unit; n = $n; spread = [math]::Round($spread, 2); date = $date } | ConvertTo-Json -Compress)
}
$fails = 0
foreach ($name in $Scene) {
  $files = @(Get-ChildItem $OutDir -Filter "${name}_r*.json" -ErrorAction SilentlyContinue | Sort-Object Name | ForEach-Object { Get-Content $_.FullName -Raw | ConvertFrom-Json })
  # A run that errored, has no median rate or is not 1920x1080 never feeds b14.rate: the scene gets no history line.
  $ok = @($files | Where-Object { $_.stats -and $null -ne $_.stats.rate_median -and -not $_.errors -and $_.full_1080p -eq $true })
  if ($files.Count -eq 0 -or $ok.Count -ne $files.Count) {
    $why = @($files | Where-Object { $ok -notcontains $_ } | ForEach-Object { if ($_.errors) { 'errors' } elseif (-not $_.stats -or $null -eq $_.stats.rate_median) { 'no median rate' } else { "region $($_.region -join 'x')" } })
    Write-Host "FAIL measure_viewport: $name has $($files.Count - $ok.Count) of $($files.Count) bad runs in $OutDir ($($why -join '; ')); no history line"
    $fails++; continue
  }
  $med = @($ok | ForEach-Object { [double]$_.stats.rate_median })
  $p5 = @($ok | ForEach-Object { [double]$_.stats.rate_p5 })
  $floor = @($ok | ForEach-Object { [double]$_.stats.longest_below_floor_s })
  $mn = ($med | Measure-Object -Minimum).Minimum; $mx = ($med | Measure-Object -Maximum).Maximum
  $lines += New-Line $name 'b14.rate' (Median $med) '/s' $ok.Count ($mx - $mn)
  $lines += New-Line $name 'b14.rate_p5' (Median $p5) '/s' $ok.Count (($p5 | Measure-Object -Maximum).Maximum - ($p5 | Measure-Object -Minimum).Minimum)
  $lines += New-Line $name 'b14.floor_longest_s' ($floor | Measure-Object -Maximum).Maximum 's' $ok.Count 0
  $busy = @($ok | Where-Object { $_.busy_before -or $_.busy_after }).Count
  $region = ($ok | ForEach-Object { if ($_.full_1080p) { '1080p' } else { "$($_.region -join 'x')" } } | Select-Object -Unique) -join ','
  # tools/report.py plots the median run's series from rate_<scene>.json.
  $pick = $ok | Where-Object { [double]$_.stats.rate_median -eq (Median $med) } | Select-Object -First 1
  if (-not $pick) { $pick = $ok[0] }
  $pick | Add-Member -NotePropertyName scene -NotePropertyValue $name -Force
  $pick | Add-Member -NotePropertyName runs_rate_median -NotePropertyValue $med -Force
  $pick | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $OutDir "rate_$name.json")
  '{0,-10} rate median {1,5:N1}/s (runs {2}; p5 {3,5:N1}; mean {4,5:N1}) longest below 24/s {5:N2} s (floor 0.5) region {6} busy {7}/{8}' -f `
    $name, (Median $med), (($med | ForEach-Object { '{0:N1}' -f $_ }) -join ' '), (Median $p5), (Median @($ok | ForEach-Object { [double]$_.stats.rate_mean })), `
    (($floor | Measure-Object -Maximum).Maximum), $region, $busy, $ok.Count
}
if ($lines.Count -gt 0 -and -not $Dry) {
  $hp = if ($HistoryOut) { $HistoryOut } else { Join-Path $root ('out/bench/history-viewport-' + (Get-Date -Format 'yyyyMMdd') + '.jsonl') }
  New-Item -ItemType Directory -Force (Split-Path $hp) | Out-Null
  Set-Content -Path $hp -Value $lines
  "history lines: $hp ($($lines.Count))"
}
if ($fails -or ($failed -gt 0)) { exit 1 }
