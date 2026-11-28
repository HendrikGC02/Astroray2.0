# SPDX-License-Identifier: Apache-2.0
# Fast test of tools/measure_viewport.ps1 without a Blender start, on canned probe results (-Summarise) and a fake CLI:
# the history lines have the fixed keys, b14.rate is the median over runs of each run's median rate (never the 5th
# percentile or the mean), b14.rate_p5 and b14.floor_longest_s are the median p5 and the longest run below the floor, and
# the machine id is the one the CLI's bench line gives (default path, no -Machine); a run with errors, without a median
# rate or in a region other than 1920x1080 makes the runner print FAIL, exit 1 and write no line for the scene; an
# unknown scene or -Runs 0 is refused; the same id helper is in measure_ttff.ps1 and both scripts take their lines'
# machine from it (never COMPUTERNAME), and a CLI with no bench line makes the helper throw.
# Usage: pwsh -NoProfile -File tests/tools/fast_viewport_runner.ps1   (exit 0 = pass)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$script = Join-Path $root 'tools/measure_viewport.ps1'
. (Join-Path $root 'tools/lib/lock.ps1')   # the id helper holds the machine lock
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) {
  if (-not $ok) { Write-Host "FAIL fast_viewport_runner: $what"; @($out) | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }; $script:bad++ }
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_viewport_runner_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
# The id helper takes the lock: a private per-PID name and directory keep this fixture off the machine's real lock
# (the gate runs it under that lock, so a set ASTRORAY_LOCK_PID is cleared too).
$env:ASTRORAY_LOCK_PID = $null
$env:ASTRORAY_LOCK_NAME = "Localstroray2-test-$PID-viewport"
$env:ASTRORAY_LOCK_DIR = Join-Path $tmp 'lock'
try {
  $good = Join-Path $tmp 'good.cmd'; $none = Join-Path $tmp 'none.cmd'
  Set-Content $good '@echo off
echo {"machine": "fake-id-1c1t", "pass_ms": 1.0}'
  Set-Content $none '@echo off
echo no line'
  function Canned([string]$dir, [string]$name, [int]$k, [double]$med, [double]$p5, [double]$mean, [double]$floor,
                  [string[]]$errors = @(), [int[]]$region = @(1920, 1080), [bool]$noMedian = $false) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $stats = @{ rate_p5 = $p5; rate_mean = $mean; longest_below_floor_s = $floor; series = @(@(0.5, $med)) }
    if (-not $noMedian) { $stats.rate_median = $med }
    @{ scene = $name; region = $region; full_1080p = ($region[0] -eq 1920 -and $region[1] -eq 1080); errors = $errors
       stats = $stats; busy_before = ''; busy_after = '' } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $dir "${name}_r$k.json")
  }
  function Run([string]$dir, [string]$cli, [string[]]$more = @()) {
    $h = Join-Path $dir 'h.jsonl'
    Remove-Item $h -ErrorAction SilentlyContinue
    $o = @(& pwsh -NoProfile -File $script -Scene b1m -Summarise -OutDir $dir -HistoryOut $h -Cli $cli -Recording (Join-Path $dir 'x.arrec') `
        -Sha deadbeef -Date 2026-01-02T03:04:05Z @more 2>&1 | ForEach-Object { "$_" })
    return @{ out = $o; code = $LASTEXITCODE; hist = $h }
  }

  # Three good runs: the lines and the machine id from the CLI (no -Machine anywhere).
  $d1 = Join-Path $tmp 'ok'
  Canned $d1 'b1m' 1 30.0 20.0 29.0 0.2
  Canned $d1 'b1m' 2 33.0 22.0 31.0 0.0
  Canned $d1 'b1m' 3 31.0 21.0 30.0 0.1
  $r = Run $d1 $good
  Check "the summary run exited $($r.code)" ($r.code -eq 0) $r.out
  $L = @()
  if (Test-Path $r.hist) { $L = @(Get-Content $r.hist | ForEach-Object { $_ | ConvertFrom-Json }) }
  Check "3 history lines expected, got $($L.Count)" ($L.Count -eq 3) $r.out
  if ($L.Count -eq 3) {
    $keys = 'sha,machine,driver,cuda,scene,metric,value,unit,n,spread,date'
    Check 'keys differ from 0009 d22' ((($L[0].PSObject.Properties.Name) -join ',') -eq $keys) $r.out
    $m = $L | Where-Object { $_.metric -eq 'b14.rate' }
    Check "b14.rate is the median of the medians (31.0), got $($m.value)" ($m.value -eq 31.0) $r.out
    Check "b14.rate spread 3.0 and n 3, got $($m.spread) / $($m.n)" ($m.spread -eq 3.0 -and $m.n -eq 3) $r.out
    Check 'b14.rate unit /s, scene b1m' ($m.unit -eq '/s' -and $m.scene -eq 'b1m') $r.out
    $p = $L | Where-Object { $_.metric -eq 'b14.rate_p5' }
    Check "b14.rate_p5 is 21.0, got $($p.value)" ($p.value -eq 21.0) $r.out
    $f = $L | Where-Object { $_.metric -eq 'b14.floor_longest_s' }
    Check "b14.floor_longest_s is 0.2, got $($f.value)" ($f.value -eq 0.2) $r.out
    Check "machine id is the CLI's (fake-id-1c1t), got $($m.machine)" ($m.machine -eq 'fake-id-1c1t' -and $m.sha -eq 'deadbeef') $r.out
    Check 'date is the one given' ((Get-Content $r.hist -Raw) -match '"date":"2026-01-02T03:04:05Z"') $r.out
  }
  Check 'rate_b1m.json (read by report.py) must exist and carry the scene' ((Test-Path (Join-Path $d1 'rate_b1m.json')) -and ((Get-Content (Join-Path $d1 'rate_b1m.json') -Raw | ConvertFrom-Json).scene -eq 'b1m')) $r.out

  # A bad run in the set: FAIL, exit 1, no line for the scene.
  $cases = @(
    @{ name = 'errors'; text = 'errors'; make = { param($d) Canned $d 'b1m' 4 5.0 1.0 4.0 9.0 @('boom') } },
    @{ name = 'region'; text = '2560x1440'; make = { param($d) Canned $d 'b1m' 4 90.0 80.0 85.0 0.0 @() @(2560, 1440) } },
    @{ name = 'no median'; text = 'no median rate'; make = { param($d) Canned $d 'b1m' 4 0.0 1.0 4.0 0.0 @() @(1920, 1080) $true } })
  foreach ($c in $cases) {
    $d = Join-Path $tmp ('bad ' + $c.name)
    Canned $d 'b1m' 1 30.0 20.0 29.0 0.2
    Canned $d 'b1m' 2 33.0 22.0 31.0 0.0
    Canned $d 'b1m' 3 31.0 21.0 30.0 0.1
    & $c.make $d
    $x = Run $d $good
    Check "a run with $($c.name) must exit nonzero" ($x.code -ne 0) $x.out
    Check "a run with $($c.name) must print FAIL measure_viewport" (($x.out -join ' ') -match 'FAIL measure_viewport') $x.out
    Check "a run with $($c.name) must name the cause ($($c.text))" (($x.out -join ' ') -match [regex]::Escape($c.text)) $x.out
    Check "a run with $($c.name) must leave no b14 line" (-not (Test-Path $x.hist) -or -not (Get-Content $x.hist -Raw)) $x.out
  }

  # Refusals.
  $o2 = @(& pwsh -NoProfile -File $script -Scene nosuch -Summarise -OutDir $tmp 2>&1 | ForEach-Object { "$_" })
  Check 'an unknown scene must exit nonzero and name it' ($LASTEXITCODE -ne 0 -and ($o2 -join ' ') -match 'nosuch') $o2
  $o3 = @(& pwsh -NoProfile -File $script -Scene b1m -Runs 0 -Summarise -OutDir $tmp 2>&1 | ForEach-Object { "$_" })
  Check '-Runs 0 must exit nonzero' ($LASTEXITCODE -ne 0) $o3
  $x = Run $d1 $none
  Check 'a CLI with no bench line must make the runner exit nonzero' ($x.code -ne 0) $x.out

  # The machine id helper: in both scripts, read from the CLI line, and both scripts' lines take their machine from it.
  foreach ($f in 'measure_ttff', 'measure_viewport') {
    $text = Get-Content (Join-Path $root "tools/$f.ps1") -Raw
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root "tools/$f.ps1"), [ref]$null, [ref]$null)
    $fn = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-BenchMachineId' }, $true)
    Check "$f has no Get-BenchMachineId" ($null -ne $fn)
    if ($fn) {
      . ([scriptblock]::Create($fn.Extent.Text))
      Check "$f machine id read from the CLI line" ((Get-BenchMachineId $good 'x.arrec') -eq 'fake-id-1c1t')
      $threw = $false; try { $null = Get-BenchMachineId $none 'x.arrec' } catch { $threw = $true }
      Check "$f must throw when the CLI prints no bench line" $threw
      Check "$f must hold the machine lock around the bench run" ($fn.Extent.Text -match 'Enter-ArMachineLock' -and $fn.Extent.Text -match 'Exit-ArMachineLock')
    }
    Check "$f must not stamp COMPUTERNAME" ($text -notmatch 'COMPUTERNAME')
    Check "$f history lines must take machine from `$machine" ($text -match 'machine = \$machine;')
    Check "$f must set `$machine from Get-BenchMachineId" ($text -match '\$machine = Get-BenchMachineId')
  }
  Check 'measure_ttff has an s3_hdri bench id' ((Get-Content (Join-Path $root 'tools/measure_ttff.ps1') -Raw) -match "s3_hdri = 's3_hdri'")
} finally {
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
if ($bad) { exit 1 }
Write-Host 'PASS fast_viewport_runner'
