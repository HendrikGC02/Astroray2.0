# SPDX-License-Identifier: Apache-2.0
# Fast test of tools/measure_viewport.ps1 without a Blender start: on canned probe results (-Summarise) the history lines
# have the fixed keys, b14.rate is the median over runs of each run's median rate (never the 5th percentile or the
# mean), b14.rate_p5 and b14.floor_longest_s are the median p5 and the longest run below the floor, a run with errors
# is left out, and an unknown scene or -Runs 0 is refused with a nonzero exit.
# Usage: pwsh -NoProfile -File tests/tools/fast_viewport_runner.ps1   (exit 0 = pass)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$script = Join-Path $root 'tools/measure_viewport.ps1'
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) {
  if (-not $ok) { Write-Host "FAIL fast_viewport_runner: $what"; @($out) | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }; $script:bad++ }
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_viewport_runner_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
  function Canned([string]$name, [int]$k, [double]$med, [double]$p5, [double]$mean, [double]$floor, [string[]]$errors = @()) {
    @{ scene = $name; region = @(1920, 1080); full_1080p = $true; errors = $errors
       stats = @{ rate_median = $med; rate_p5 = $p5; rate_mean = $mean; longest_below_floor_s = $floor; series = @(@(0.5, $med)) }
       busy_before = ''; busy_after = '' } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $tmp "${name}_r$k.json")
  }
  Canned 'b1m' 1 30.0 20.0 29.0 0.2
  Canned 'b1m' 2 33.0 22.0 31.0 0.0
  Canned 'b1m' 3 31.0 21.0 30.0 0.1
  Canned 'b1m' 4 5.0 1.0 4.0 9.0 @('boom')
  $hist = Join-Path $tmp 'h.jsonl'
  $out = @(& pwsh -NoProfile -File $script -Scene b1m -Summarise -OutDir $tmp -HistoryOut $hist -Sha deadbeef -Date 2026-01-02T03:04:05Z 2>&1 | ForEach-Object { "$_" })
  Check "the summary run exited $LASTEXITCODE" ($LASTEXITCODE -eq 0) $out
  $L = @()
  if (Test-Path $hist) { $L = @(Get-Content $hist | ForEach-Object { $_ | ConvertFrom-Json }) }
  Check "3 history lines expected, got $($L.Count)" ($L.Count -eq 3) $out
  if ($L.Count -eq 3) {
    $keys = 'sha,machine,driver,cuda,scene,metric,value,unit,n,spread,date'
    Check 'keys differ from 0009 d22' ((($L[0].PSObject.Properties.Name) -join ',') -eq $keys) $out
    $r = $L | Where-Object { $_.metric -eq 'b14.rate' }
    Check "b14.rate is the median of the medians (31.0), got $($r.value)" ($r.value -eq 31.0) $out
    Check "b14.rate spread 3.0 and n 3, got $($r.spread) / $($r.n)" ($r.spread -eq 3.0 -and $r.n -eq 3) $out
    Check 'b14.rate unit /s, scene b1m' ($r.unit -eq '/s' -and $r.scene -eq 'b1m') $out
    $p = $L | Where-Object { $_.metric -eq 'b14.rate_p5' }
    Check "b14.rate_p5 is 21.0, got $($p.value)" ($p.value -eq 21.0) $out
    $f = $L | Where-Object { $_.metric -eq 'b14.floor_longest_s' }
    Check "b14.floor_longest_s is 0.2 (the errored run is out), got $($f.value)" ($f.value -eq 0.2) $out
    Check 'machine id is the neutral one' ($r.machine -eq 'rtx5070ti-8c16t' -and $r.sha -eq 'deadbeef') $out
    Check 'date is the one given' ((Get-Content $hist -Raw) -match '"date":"2026-01-02T03:04:05Z"') $out
  }
  Check 'rate_b1m.json (read by report.py) must exist and carry the scene' ((Test-Path (Join-Path $tmp 'rate_b1m.json')) -and ((Get-Content (Join-Path $tmp 'rate_b1m.json') -Raw | ConvertFrom-Json).scene -eq 'b1m')) $out
  $o2 = @(& pwsh -NoProfile -File $script -Scene nosuch -Summarise -OutDir $tmp 2>&1 | ForEach-Object { "$_" })
  Check 'an unknown scene must exit nonzero and name it' ($LASTEXITCODE -ne 0 -and ($o2 -join ' ') -match 'nosuch') $o2
  $o3 = @(& pwsh -NoProfile -File $script -Scene b1m -Runs 0 -Summarise -OutDir $tmp 2>&1 | ForEach-Object { "$_" })
  Check '-Runs 0 must exit nonzero' ($LASTEXITCODE -ne 0) $o3
} finally {
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($bad) { exit 1 }
Write-Host 'PASS fast_viewport_runner'
