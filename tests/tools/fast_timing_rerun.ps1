# SPDX-License-Identifier: Apache-2.0
# tools/lib/load.ps1 (0009 d13): a fake timed step beside an injected foreign process. Red with no load; red when
# the single re-run under load also fails; green with RERUN printed when the re-run passes; no re-run when the
# first result passes; load that appears and ends inside the window is still seen; a descendant of this process is
# not load; the default name list holds the compilers, Blender and the GPU users. The injected process is a copy of
# ping.exe under a private name (Names narrows the sampling to it, so a real compile on the machine cannot turn the
# no-load case into a load case), started through WMI so that its parent is not this process and killed in the
# finally (it outlives every case, so a slow runner cannot end the load early). The wiring is covered too: the B-16
# failure test on canned integrator output, the fast tier's second result and the first attempt's exclusion from the
# tier clock, and perf's re-run (Assert-QuietGpu first, lines replaced, deciding load) on a canned bench. Run by
# tests/tools/run_fast.ps1 (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/lib/load.ps1')
. (Join-Path $root 'tools/lib/summary.ps1')
. (Join-Path $root 'tools/lib/testrun.ps1')
# Canned B-08 and B-12 overruns are judged for the reference machine, whatever machine this runs on (machine.ps1).
$script:ArMachineIdForTest = 'rtx5070ti-8c16t'
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) {
  if (-not $ok) { Write-Host "FAIL fast_timing_rerun: $what"; @($out) | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }; $script:bad++ }
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_timing_rerun_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$exe = Join-Path $tmp 'fxload.exe'
Copy-Item (Join-Path $env:SystemRoot 'System32/PING.EXE') $exe
$names = @('fxload')
$injected = @()
function Start-Foreign([int]$Count) {
  $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "`"$exe`" -n $Count 127.0.0.1" }
  if ($r.ReturnValue -ne 0) { throw "WMI could not start the injected process ($($r.ReturnValue))" }
  $script:injected += [int]$r.ProcessId
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Get-Process -Id $r.ProcessId -ErrorAction SilentlyContinue) -and $sw.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 20 }
  return [int]$r.ProcessId
}
# One fake timed step: the first attempt reads `slow`, the second `$secondSlow`; Over means slow.
function Invoke-Fake([string[]]$LoadNames, [bool]$SecondSlow, [bool]$FirstSlow = $true, [scriptblock]$Inside = {}) {
  $script:runs = 0
  $all = @(& {
      Invoke-ArTimedCheck -Check 'fake' -Names $LoadNames -IntervalMs 100 -Run {
        param($n)
        $script:runs++
        & $Inside
        @{ Slow = $(if ($n -eq 1) { $FirstSlow } else { $SecondSlow }) }
      } -Over { param($x) @{ Over = $x.Slow; Text = $(if ($x.Slow) { 'slow' } else { 'fast' }) } }
    } 6>&1)
  [pscustomobject]@{
    Res  = @($all | Where-Object { $_ -isnot [Management.Automation.InformationRecord] })[-1]
    Text = @($all | Where-Object { $_ -is [Management.Automation.InformationRecord] } | ForEach-Object { "$_" })
    Runs = $script:runs
  }
}
try {
  foreach ($n in 'cl', 'link', 'nvcc', 'ninja', 'blender', 'astroray_cli', 'astroray_tests_gpu', 'nsys') {
    Check "default load names include $n" ($script:ArLoadNames -contains $n)
  }

  # A process that appears and ends inside the window, with the sampler between the start and the end.
  $x = Invoke-Fake $names $false -Inside { $null = Start-Foreign 2; Start-Sleep -Milliseconds 1700 }
  Check 'load that ended before the window did is seen' ($x.Res.Rerun -and $x.Runs -eq 2 -and -not $x.Res.Over) $x.Text

  # A descendant of this process is the run's own work.
  $own = Start-Process -FilePath $exe -ArgumentList '-n 120', '127.0.0.1' -PassThru -WindowStyle Hidden
  Start-Sleep -Milliseconds 200
  Check 'own descendant is not load' (@(Get-ArForeignLoad $names).Count -eq 0)

  $fpid = Start-Foreign 120
  Check 'foreign process is load, the descendant is not' ((@(Get-ArForeignLoad $names) -join ',') -eq "fxload $fpid")

  $x = Invoke-Fake @('fxnone') $false
  Check 'no load: the first result decides (red, one run, no RERUN)' ($x.Res.Over -and -not $x.Res.Rerun -and $x.Runs -eq 1 -and -not ($x.Text -match 'RERUN')) $x.Text

  $x = Invoke-Fake $names $true
  Check 'load, re-run also slow: red after two runs' ($x.Res.Over -and $x.Res.Rerun -and $x.Runs -eq 2) $x.Text
  Check 'RERUN line names the check and the process, both results printed' (($x.Text -join "`n") -match "RERUN fake: load fxload $fpid" -and ($x.Text -join "`n") -match 'first:\s+slow' -and ($x.Text -join "`n") -match 'second:\s+slow') $x.Text

  $x = Invoke-Fake $names $false
  Check 'load, re-run passes: green with RERUN printed' (-not $x.Res.Over -and $x.Res.Rerun -and $x.Runs -eq 2 -and ($x.Text -join "`n") -match "RERUN fake: load fxload $fpid" -and ($x.Text -join "`n") -match 'second:\s+fast') $x.Text

  $x = Invoke-Fake $names $true -FirstSlow $false
  Check 'load, first passes: one run, no RERUN' (-not $x.Res.Over -and -not $x.Res.Rerun -and $x.Runs -eq 1 -and -not ($x.Text -match 'RERUN')) $x.Text

  # B-16: only the tripwire's timing line is a timing failure.
  $line = 'FAIL integrator tripwire cuda: 1 spp 3.2 ms per sample (baseline 2, limit x1.25)'
  $sum = 'FAIL integrator cuda: 9 checks in 5.0 s'
  Check 'tripwire alone over its limit is a timing failure' (Test-ArOnlyTiming @($line, $sum) 1 'tripwire')
  Check 'tripwire and another failing check is not' (-not (Test-ArOnlyTiming @($line, 'FAIL integrator counters cuda: +3 %', $sum) 1 'tripwire'))
  Check 'a bare summary line is not' (-not (Test-ArOnlyTiming @($sum) 1 'tripwire'))
  Check 'no baseline is not' (-not (Test-ArOnlyTiming @('FAIL integrator tripwire cuda: ms_per_sample_1spp 3: no baseline (run --bless)', $sum) 1 'tripwire'))
  Check 'a caught error is not' (-not (Test-ArOnlyTiming @('FAIL integrator tripwire cuda: CUDA error 700', $sum) 1 'tripwire'))
  Check 'exit 0 is not' (-not (Test-ArOnlyTiming @($line) 0 'tripwire'))

  # The fast tier's tests step: a fake Step that runs the body and records it, the way check.ps1's does.
  function Set-FakeClock { $script:ArClock = [Diagnostics.Stopwatch]::StartNew(); $script:ArExcluded = 0.0; $script:ArSteps = @() }
  # The first attempt "takes" 600 s: the fake Step puts them on the tier clock (a negative exclusion) and reports
  # them as the step's seconds, so no real time passes and a slow runner's overhead (seconds) stays far below
  # the 100 s budget while the first attempt alone is far above it.
  $fakeStep = {
    param($name, $body)
    $t = [Diagnostics.Stopwatch]::StartNew(); $o = @(& $body)
    $secs = $t.Elapsed.TotalSeconds
    if ($name -eq 'tests') { $secs = 600.0; $script:ArExcluded -= 600.0 }
    Add-ArStep $name $secs $true
    [pscustomobject]@{ Code = 0; Out = $o }
  }
  $script:attempt = 0
  $tests = { $script:attempt++; if ($script:attempt -eq 1) { '  0.50 s: slow case' } else { '  0.10 s: slow case' } }
  Set-FakeClock
  $x = Invoke-ArFastTests -Step $fakeStep -Tests $tests -PerTest 0.41 -Budget 100 -Names $names -IntervalMs 100 -Log (Join-Path $tmp "tier.log")
  Check 'fast tests: load and a slow first attempt run the step again' ($x.Rerun -and $script:attempt -eq 2 -and $x.Result.Out[0] -match '0.10 s') @($script:ArSteps | ForEach-Object { $_.name })
  Check 'fast tests: the second result decides (B-12 passes)' (-not $x.Over)
  $tl = if (Test-Path (Join-Path $tmp 'tier.log')) { @(Get-Content (Join-Path $tmp 'tier.log')) } else { @() }
  Check 'the RERUN line and both results are in the tier log' (($tl -join "`n") -match "RERUN B-08/B-12 fast tests: load fxload $fpid" -and ($tl -join "`n") -match 'second:') $tl
  Check 'fast tests: the first attempt is out of the tier clock (B-08 100 s passes after a 600 s first attempt)' ((Get-ArSeconds) -lt 100 -and @($script:ArSteps).Count -eq 2) @("tier $(Get-ArSeconds) s")
  $script:attempt = 0
  Set-FakeClock
  $x = Invoke-ArFastTests -Step $fakeStep -Tests $tests -PerTest 0.41 -Budget 100 -Names @('fxnone') -IntervalMs 100
  Check 'fast tests: no load, the slow first result decides' ($x.Over -and -not $x.Rerun -and $script:attempt -eq 1)

  # Perf's re-run on a canned bench.
  $script:benchCalls = 0; $script:quietCalls = 0
  $bench = { $script:benchCalls++; if ($script:benchCalls -eq 1) { 'slow' } else { 'fast' } }
  $compare = { param($ln) @{ Code = $(if (@($ln) -contains 'slow') { 1 } else { 0 }); Out = @("pass_ms $ln") } }
  $quiet = { $script:quietCalls++; $script:benchCallsAtQuiet = $script:benchCalls }
  $x = Invoke-ArPerfCheck -Bench $bench -Compare $compare -Quiet $quiet -Names $names -IntervalMs 100
  Check 'perf: load and B-38 over run the bench again and the second lines replace the first' ($x.Rerun -and $script:benchCalls -eq 2 -and @($x.Result) -contains 'fast' -and -not $x.Over) @($x.Result)
  Check 'perf: Assert-QuietGpu runs once, before the re-run' ($script:quietCalls -eq 1 -and $script:benchCallsAtQuiet -eq 1)
  Check 'perf: the deciding load is reported (it refuses -Record)' (@($x.DecidingLoad) -contains "fxload $fpid")
  $script:benchCalls = 5; $script:quietCalls = 0
  $x = Invoke-ArPerfCheck -Bench $bench -Compare $compare -Quiet $quiet -Names $names -IntervalMs 100
  Check 'perf: a passing first run under load is not repeated but still reports the load' (-not $x.Rerun -and $script:quietCalls -eq 0 -and @($x.DecidingLoad).Count -eq 1)
  $script:benchCalls = 0
  $x = Invoke-ArPerfCheck -Bench $bench -Compare $compare -Quiet $quiet -Names @('fxnone') -IntervalMs 100
  Check 'perf: no load, the first result decides and nothing is reported' ($x.Over -and -not $x.Rerun -and $script:benchCalls -eq 1 -and @($x.DecidingLoad).Count -eq 0)
} finally {
  foreach ($i in $injected) { Stop-Process -Id $i -Force -ErrorAction SilentlyContinue }
  if ($own) { Stop-Process -Id $own.Id -Force -ErrorAction SilentlyContinue; $null = $own.WaitForExit(5000) }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { Write-Host "FAIL fast_timing_rerun: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_timing_rerun'
