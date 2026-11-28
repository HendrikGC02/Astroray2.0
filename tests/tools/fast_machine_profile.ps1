# SPDX-License-Identifier: Apache-2.0
# Owner decision 47 (c): a timing row over its reference limit is judged by the machine it ran on. A fake machine id
# gives `off-reference` lines and no failure; the reference id still fails; an id with a profile is judged against
# the limit derived from it (reference limit x median / reference median, never below the reference limit); an id
# with a profile that lacks the row, or no profile, prints; a missing machines file or CLI fails loudly; a failed
# test run fails on both machines. Also the Python twins (timing_ok for the timing CHECKs of tests, the B-05 and B-16
# verdicts, `record` for `check.ps1 profile`) and the standing a tier hands to those tests. No CLI, no GPU: the
# fixture hands the id in. Run by tests/tools/run_fast.ps1.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/lib/python.ps1')
. (Join-Path $root 'tools/lib/machine.ps1')
. (Join-Path $root 'tools/lib/load.ps1')
. (Join-Path $root 'tools/lib/summary.ps1')
. (Join-Path $root 'tools/lib/testrun.ps1')
$py = Get-ArPython
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) {
  if (-not $ok) { Write-Host "FAIL fast_machine_profile: $what"; @($out) | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }; $script:bad++ }
}
$ref = (& $py -I (Join-Path $root 'tools/machine_id.py') reference)
Check 'bench/machines.toml names the reference machine' ($ref -eq 'rtx5070ti-8c16t') $ref
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_machine_profile_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$saveEnv = $env:ASTRORAY_OFF_REFERENCE
try {
  $toml = Join-Path $tmp 'machines.toml'
  Set-Content -Path $toml -Value @'
reference = "refbox-8c16t"

[reference_median]
"B-08" = 5.0

[machine."profiled-4c8t"]
"B-08" = 10.0
'@
  function Judge([string]$Id, [string]$Row, [double]$Measured, [double]$Limit = 10, [string]$File = $toml) {
    $o = @(& $py -I (Join-Path $root 'tools/machine_id.py') judge --row $Row --measured $Measured --limit $Limit --unit s --id $Id --machines $File 2>&1 | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0) { return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $o } }
    $j = $o[-1] | ConvertFrom-Json
    [pscustomobject]@{ Code = 0; Verdict = $j.verdict; Limit = $j.limit; Line = $j.line; Out = $o }
  }
  # The reference id fails on an overrun, silently; within the limit passes.
  $x = Judge 'refbox-8c16t' 'B-08' 12
  Check 'the reference id fails a row over its limit' ($x.Verdict -eq 'fail' -and $x.Line -eq '') $x.Out
  $x = Judge 'refbox-8c16t' 'B-08' 9.9
  Check 'a row within its limit passes' ($x.Verdict -eq 'pass') $x.Out
  # An id the file does not know: the overrun is printed, never failed, never silent.
  $x = Judge 'laptop-6c12t' 'B-08' 12
  Check 'a machine without a profile prints off-reference with its id and the row' ($x.Verdict -eq 'off' -and $x.Line -eq 'off-reference laptop-6c12t B-08: 12 s against 10 s') $x.Out
  # A profile derives the limit: 10 x 10/5 = 20.
  $x = Judge 'profiled-4c8t' 'B-08' 15
  Check 'a profiled machine passes under its derived limit and says so' ($x.Verdict -eq 'pass' -and $x.Limit -eq 20 -and $x.Line -like 'profile profiled-4c8t B-08: 15 s against 20 s*') $x.Out
  $x = Judge 'profiled-4c8t' 'B-08' 25
  Check 'a profiled machine fails above its derived limit' ($x.Verdict -eq 'fail' -and $x.Limit -eq 20) $x.Out
  $x = Judge 'profiled-4c8t' 'B-09' 200 180
  Check 'a profile that lacks the row prints off-reference' ($x.Verdict -eq 'off' -and $x.Line -like 'off-reference profiled-4c8t B-09:*') $x.Out
  # A machine faster than the reference keeps the reference limit.
  Set-Content -Path $toml -Value (((Get-Content $toml -Raw) -replace '"B-08" = 10.0', '"B-08" = 2.5'))
  $x = Judge 'profiled-4c8t' 'B-08' 12
  Check 'a faster profile never tightens the reference limit' ($x.Verdict -eq 'fail' -and $x.Limit -eq 10) $x.Out
  # No machines file, or none naming a reference: loud.
  $x = Judge 'laptop-6c12t' 'B-08' 12 10 (Join-Path $tmp 'absent.toml')
  Check 'a missing machines file fails loudly' ($x.Code -eq 2 -and ($x.Out -join ' ') -like 'FAIL machine_id:*absent.toml*') $x.Out
  Set-Content -Path (Join-Path $tmp 'noref.toml') -Value '[reference_median]'
  $x = Judge 'laptop-6c12t' 'B-08' 12 10 (Join-Path $tmp 'noref.toml')
  Check 'a machines file without a reference fails loudly' ($x.Code -eq 2 -and ($x.Out -join ' ') -like '*has no*reference*') $x.Out

  # The PowerShell verdict function over the same rule, with the id handed in.
  $script:ArMachineIdForTest = 'fakebox-4c8t'
  $r = Resolve-ArTimingRow -Row 'B-12' -Measured 0.6 -Limit 0.41
  Check 'Resolve-ArTimingRow gives OFF and the line for a fake machine' ($r.Verdict -eq 'OFF' -and $r.Line -like 'off-reference fakebox-4c8t B-12: 0.6 s against 0.41 s') $r
  $r = Resolve-ArTimingRow -Row 'B-12' -Measured 0.3 -Limit 0.41
  Check 'Resolve-ArTimingRow passes a row within its limit' ($r.Verdict -eq 'PASS' -and $r.Line -eq '') $r
  $script:ArMachineIdForTest = $ref
  $r = Resolve-ArTimingRow -Row 'B-12' -Measured 0.6 -Limit 0.41
  Check 'Resolve-ArTimingRow fails the reference machine' ($r.Verdict -eq 'FAIL' -and $r.Line -eq '') $r
  $r = Resolve-ArTimingRow -Row 'B-12' -Measured 0.6 -Limit 0.41 -NoMachine
  Check 'Resolve-ArTimingRow -NoMachine (hosted ci) fails against the limit alone' ($r.Verdict -eq 'FAIL') $r

  # The standing handed to the tests that time themselves.
  $env:ASTRORAY_OFF_REFERENCE = 'stale'
  $script:ArMachineIdForTest = $ref
  $null = Set-ArMachineStanding
  Check 'the reference machine clears ASTRORAY_OFF_REFERENCE' (-not $env:ASTRORAY_OFF_REFERENCE)
  $script:ArMachineIdForTest = 'fakebox-4c8t'
  $null = Set-ArMachineStanding
  Check 'another machine sets ASTRORAY_OFF_REFERENCE to its id' ($env:ASTRORAY_OFF_REFERENCE -eq 'fakebox-4c8t')
  $null = Set-ArMachineStanding -NoMachine
  Check 'hosted ci has no standing' (-not $env:ASTRORAY_OFF_REFERENCE)

  # The fast tier's step: a failed run is a failure on both machines and never a re-run; a slow run is Over only
  # where it fails (an off-reference overrun is not Over, so it does not re-run either).
  function Invoke-FastWith([string]$Id, [int]$Code, [double]$Budget) {
    $script:ArMachineIdForTest = $Id
    $script:ArSteps = @([pscustomobject]@{ name = 'x'; seconds = 0.0; pass = $true })
    Start-ArSummary 'fast'
    $out = @('  0.900000 s: slowcase', '  0.100000 s: other')
    Invoke-ArFastTests -PerTest 0.41 -Budget $Budget -Log '' -Names @('fxnone') -IntervalMs 100 -Tests { } -Step { param($n, $b) [pscustomobject]@{ Code = $Code; Out = $out } }
  }
  foreach ($id in $ref, 'fakebox-4c8t') {
    $t = Invoke-FastWith $id 1 10
    Check "a failed test run is not a timing result on $id" ($t.Result.Code -eq 1 -and -not $t.Over -and -not $t.Rerun)
  }
  $t = Invoke-FastWith $ref 0 10
  Check 'B-12 over its limit is Over on the reference machine' ($t.Over)
  $t = Invoke-FastWith 'fakebox-4c8t' 0 10
  Check 'B-12 over its limit is not Over on another machine' (-not $t.Over)

  # The Python side: timing CHECKs of tests, B-05, B-16, `record`.
  $out = @(& $py -I -c @"
import os, sys
sys.path.insert(0, r'$root/tools')
import machine_id
os.environ.pop('ASTRORAY_OFF_REFERENCE', None)
assert machine_id.timing_ok('B-21 x', 4.0, 5.0, 'ms') is True
assert machine_id.timing_ok('B-21 x', 6.0, 5.0, 'ms') is False
os.environ['ASTRORAY_OFF_REFERENCE'] = 'fakebox-4c8t'
assert machine_id.timing_ok('B-21 x', 6.0, 5.0, 'ms') is True
"@ 2>&1 | ForEach-Object { "$_" })
  Check 'timing_ok fails on an overrun unset and prints it set' ($LASTEXITCODE -eq 0 -and ($out -join ' ') -eq 'off-reference fakebox-4c8t B-21 x: 6 ms against 5 ms') $out
  $out = @(& $py -I -c @"
import sys
sys.path.insert(0, r'$root/tools')
import kernelgate
rows = [('shade_surface', 9.0, 8.0), ('shade_background', 2.0, 3.0)]
f, lines = kernelgate.timing_verdicts(rows, 'fakebox-4c8t')
assert f == [] and lines == ['off-reference fakebox-4c8t B-05 shade_surface: 9 s against 8 s'], (f, lines)
f, lines = kernelgate.timing_verdicts(rows, 'rtx5070ti-8c16t')
assert len(f) == 1 and f[0].startswith('B-05: shade_surface'), f
"@ 2>&1 | ForEach-Object { "$_" })
  Check 'B-05 is judged by the machine rule' ($LASTEXITCODE -eq 0) $out
  $out = @(& $py -I -c @"
import sys
sys.path.insert(0, r'$root/tools')
import integrator_checks as ic
ok, line = ic.tripwire_verdict('fakebox-4c8t', 'rtx5070ti-8c16t', 64, 3.0e-05, 2.0e-05, None)
assert ok and line.startswith('off-reference fakebox-4c8t B-16 tripwire 64 spp:'), (ok, line)
ok, line = ic.tripwire_verdict('rtx5070ti-8c16t', 'rtx5070ti-8c16t', 64, 3.0e-05, 2.0e-05, 2.0e-05)
assert not ok and 'limit x1.25' in line, (ok, line)
ok, line = ic.tripwire_verdict('fakebox-4c8t', 'rtx5070ti-8c16t', 64, 3.0e-05, 2.0e-05, 2.9e-05)
assert ok and 'limit x1.25' in line, (ok, line)
ok, line = ic.tripwire_verdict('fakebox-4c8t', 'rtx5070ti-8c16t', 64, 4.0e-05, 2.0e-05, 2.9e-05)
assert not ok, (ok, line)
"@ 2>&1 | ForEach-Object { "$_" })
  Check 'B-16 tripwire is keyed by machine' ($LASTEXITCODE -eq 0) $out
  Set-Content -Path $toml -Value (((Get-Content $toml -Raw) -replace '"B-08" = 2.5', '"B-08" = 10.0'))
  $out = @(& $py -I (Join-Path $root 'tools/machine_id.py') record --id newbox-2c4t --row B-08 --value 11.5 --machines $toml 2>&1)
  $out2 = @(& $py -I (Join-Path $root 'tools/machine_id.py') record --id profiled-4c8t --row B-08 --value 9 --machines $toml 2>&1)
  $t = Get-Content $toml -Raw
  Check 'record adds a machine and rewrites one, keeping the rest' ($LASTEXITCODE -eq 0 -and $t -match '(?s)reference = "refbox-8c16t".*\[machine\."newbox-2c4t"\]\s+"B-08" = 11\.5' -and $t -match '"B-08" = 9\b' -and $t -notmatch '"B-08" = 10\b') @($out + $out2 + $t)
  $x = @(& $py -I (Join-Path $root 'tools/machine_id.py') record --id refbox-8c16t --row B-08 --value 9 --machines $toml 2>&1)
  Check 'record refuses the reference machine' ($LASTEXITCODE -eq 2 -and ($x -join ' ') -like 'FAIL machine_id:*reference machine*') $x
  # No CLI: the id fails loudly naming build.ps1.
  $x = @(& $py -I -c "import sys; sys.path.insert(0, r'$root/tools'); import machine_id; machine_id.machine_id(r'$tmp/absent.exe')" 2>&1)
  Check 'a missing CLI names tools/build.ps1' (($x -join ' ') -like '*MachineError*build.ps1*') $x
} finally {
  $script:ArMachineIdForTest = ''
  if ($null -ne $saveEnv) { $env:ASTRORAY_OFF_REFERENCE = $saveEnv } else { Remove-Item Env:ASTRORAY_OFF_REFERENCE -ErrorAction SilentlyContinue }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { exit 1 }
Write-Host 'PASS fast_machine_profile'
