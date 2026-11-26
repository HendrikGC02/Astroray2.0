# SPDX-License-Identifier: Apache-2.0
# tools/lib/load.ps1 (0009 d13): a fake timed step beside an injected foreign process. Red with no load; red when
# the single re-run under load also fails; green with RERUN printed when the re-run passes; no re-run when the
# first result passes; load that appears and ends inside the window is still seen; a descendant of this process is
# not load; the default name list holds the compilers, Blender and the GPU users. The injected process is a copy of
# ping.exe under a private name (Names narrows the sampling to it, so a real compile on the machine cannot turn the
# no-load case into a load case), started through WMI so that its parent is not this process. Run by
# tests/tools/run_fast.ps1 (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/lib/load.ps1')
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
  $own = Start-Process -FilePath $exe -ArgumentList '-n 4', '127.0.0.1' -PassThru -WindowStyle Hidden
  Start-Sleep -Milliseconds 200
  Check 'own descendant is not load' (@(Get-ArForeignLoad $names).Count -eq 0)

  $fpid = Start-Foreign 12
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
} finally {
  foreach ($i in $injected) { Stop-Process -Id $i -Force -ErrorAction SilentlyContinue }
  if ($own) { Stop-Process -Id $own.Id -Force -ErrorAction SilentlyContinue; $null = $own.WaitForExit(5000) }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { Write-Host "FAIL fast_timing_rerun: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_timing_rerun'
