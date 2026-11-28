# SPDX-License-Identifier: Apache-2.0
# Owner decision 47 (c): a timing row over its reference limit is judged by the machine it ran on. A fake machine id
# gives `off-reference` lines and no failure; the reference id still fails; an id with a profile is judged against
# the limit derived from it (reference limit x median / reference median, never below the reference limit); an id
# with a profile that lacks the row, or no profile, prints; a missing machines file or CLI fails loudly; a failed
# test run fails on both machines; the measuring runs of `check.ps1 profile` leave B-08 unjudged and nothing else.
# Also the Python twins (timing_ok for the timing CHECKs of tests, the B-05 and B-16 verdicts, `record` and
# `record-reference` for `check.ps1 profile`) and the standing a tier hands to those tests. No CLI, no GPU: the
# fixture hands the id in. The Python cases share one process (startup is most of this fixture's time). Run by
# tests/tools/run_fast.ps1.
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
$ref = 'rtx5070ti-8c16t'
$saveEnv = $env:ASTRORAY_OFF_REFERENCE
try {
  # The Python cases, one process. Each failed assertion prints a line starting FAIL.
  $code = @'
import os, sys, tempfile, traceback
root = sys.argv[1]
sys.path.insert(0, os.path.join(root, "tools"))
import machine_id
import kernelgate
import integrator_checks as ic

tmp = tempfile.mkdtemp()
toml = os.path.join(tmp, "machines.toml")
def write(text):
    open(toml, "w", newline="\n").write(text)
BASE = 'reference = "refbox-8c16t"\n\n[reference_median]\n"B-08" = 5.0\n\n[machine."profiled-4c8t"]\n"B-08" = 10.0\n'
write(BASE)
def judge(mid, row, m, lim=10, f=None):
    return machine_id.judge(row, m, lim, mid, "s", f or toml)
def case(name):
    def deco(fn):
        try:
            fn()
        except Exception:
            print("FAIL " + name + ": " + traceback.format_exc().strip().splitlines()[-1])
    return deco

@case("the file names the reference machine")
def _():
    assert machine_id.reference(os.path.join(root, "bench", "machines.toml")) == "rtx5070ti-8c16t"
@case("the reference id fails an overrun silently, a row within its limit passes")
def _():
    assert judge("refbox-8c16t", "B-08", 12) == ("fail", 10, "")
    assert judge("refbox-8c16t", "B-08", 9.9)[0] == "pass"
@case("a machine without a profile prints off-reference with its id and the row")
def _():
    assert judge("laptop-6c12t", "B-08", 12) == ("off", 10, "off-reference laptop-6c12t B-08: 12 s against 10 s")
@case("a profile derives the limit 10 x 10/5 = 20 and says so")
def _():
    v, lim, line = judge("profiled-4c8t", "B-08", 15)
    assert v == "pass" and lim == 20 and line.startswith("profile profiled-4c8t B-08: 15 s against 20 s"), (v, lim, line)
    v, lim, line = judge("profiled-4c8t", "B-08", 25)
    assert v == "fail" and lim == 20, (v, lim)
@case("a profile that lacks the row prints off-reference")
def _():
    v, _, line = judge("profiled-4c8t", "B-09", 200, 180)
    assert v == "off" and line.startswith("off-reference profiled-4c8t B-09:"), (v, line)
@case("a faster profile never tightens the reference limit")
def _():
    write(BASE.replace('"B-08" = 10.0', '"B-08" = 2.5'))
    v, lim, _ = judge("profiled-4c8t", "B-08", 12)
    assert v == "fail" and lim == 10, (v, lim)
    write(BASE)
@case("a missing machines file and a file without a reference fail loudly")
def _():
    for f, want in ((os.path.join(tmp, "absent.toml"), "absent.toml"), (None, "reference")):
        if f is None:
            f = os.path.join(tmp, "noref.toml")
            open(f, "w").write("[reference_median]\n")
        try:
            judge("laptop-6c12t", "B-08", 12, 10, f)
        except machine_id.MachineError as e:
            assert want in str(e), str(e)
        else:
            raise AssertionError("no error for " + f)
@case("timing_ok fails an overrun unset and prints it set")
def _():
    os.environ.pop("ASTRORAY_OFF_REFERENCE", None)
    assert machine_id.timing_ok("B-21 x", 4.0, 5.0, "ms") is True
    assert machine_id.timing_ok("B-21 x", 6.0, 5.0, "ms") is False
    os.environ["ASTRORAY_OFF_REFERENCE"] = "fakebox-4c8t"
    import io, contextlib
    b = io.StringIO()
    with contextlib.redirect_stdout(b):
        assert machine_id.timing_ok("B-21 x", 6.0, 5.0, "ms") is True
    assert b.getvalue().strip() == "off-reference fakebox-4c8t B-21 x: 6 ms against 5 ms", b.getvalue()
    os.environ.pop("ASTRORAY_OFF_REFERENCE")
@case("B-05 is judged by the machine rule")
def _():
    rows = [("shade_surface", 9.0, 8.0), ("shade_background", 2.0, 3.0)]
    f, n = kernelgate.timing_verdicts(rows, "fakebox-4c8t")
    assert f == [] and n == ["off-reference fakebox-4c8t B-05 shade_surface: 9 s against 8 s"], (f, n)
    f, n = kernelgate.timing_verdicts(rows, "rtx5070ti-8c16t")
    assert len(f) == 1 and f[0].startswith("B-05: shade_surface"), f
@case("B-16 tripwire is keyed by machine")
def _():
    R = "rtx5070ti-8c16t"
    ok, line = ic.tripwire_verdict("fakebox-4c8t", R, 64, 3.0e-05, 2.0e-05, None)
    assert ok and line.startswith("off-reference fakebox-4c8t B-16 tripwire 64 spp:"), (ok, line)
    ok, line = ic.tripwire_verdict(R, R, 64, 3.0e-05, 2.0e-05, 2.0e-05)
    assert not ok and "limit x1.25" in line, (ok, line)
    ok, line = ic.tripwire_verdict("fakebox-4c8t", R, 64, 3.0e-05, 2.0e-05, 2.9e-05)
    assert ok and "limit x1.25" in line, (ok, line)
    ok, line = ic.tripwire_verdict("fakebox-4c8t", R, 64, 4.0e-05, 2.0e-05, 2.9e-05)
    assert not ok, (ok, line)
@case("record adds a machine and rewrites one, keeping the rest; it refuses the reference machine")
def _():
    machine_id.record("newbox-2c4t", "B-08", 11.5, toml)
    machine_id.record("profiled-4c8t", "B-08", 9, toml)
    t = open(toml).read()
    assert 'reference = "refbox-8c16t"' in t and '[machine."newbox-2c4t"]\n"B-08" = 11.5\n' in t, t
    assert '"B-08" = 9\n' in t and "10.0" not in t, t
    try:
        machine_id.record("refbox-8c16t", "B-08", 9, toml)
    except machine_id.MachineError as e:
        assert "reference machine" in str(e)
    else:
        raise AssertionError("the reference machine was recorded")
    write(BASE)
@case("record-reference replaces the row and its comment, keeping the machines")
def _():
    machine_id.record_reference("B-08", 6.25, "median of five, sha abc", toml)
    machine_id.record_reference("B-09", 120, "gate", toml)
    t = open(toml).read()
    assert '# median of five, sha abc\n"B-08" = 6.25\n' in t and '"B-08" = 5.0' not in t, t
    assert '"B-09" = 120\n' in t and '[machine."profiled-4c8t"]' in t, t
    assert machine_id.load_machines(toml)["reference_median"] == {"B-08": 6.25, "B-09": 120}
@case("a missing CLI names tools/build.ps1")
def _():
    try:
        machine_id.machine_id(os.path.join(tmp, "absent.exe"))
    except machine_id.MachineError as e:
        assert "build.ps1" in str(e), str(e)
    else:
        raise AssertionError("no error")
'@
  $out = @(& $py -I -c $code $root 2>&1 | ForEach-Object { "$_" })
  Check 'the Python cases' ($LASTEXITCODE -eq 0 -and -not ($out | Where-Object { $_ })) $out

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
  $env:ASTRORAY_OFF_REFERENCE = 'stale'
  $null = Set-ArMachineStanding -NoMachine
  Check 'hosted ci has no standing, and clears a stray one' (-not $env:ASTRORAY_OFF_REFERENCE)

  # The fast tier's step: a failed run is a failure on both machines and never a re-run; a slow run is Over only
  # where it fails (an off-reference overrun is not Over, so it does not re-run either); -WaiveB08 (the measuring runs of
  # `profile`) leaves B-08 unjudged and B-12 judged.
  function Invoke-FastWith([string]$Id, [int]$Code, [double]$Budget, [string[]]$Lines, [switch]$Waive) {
    $script:ArMachineIdForTest = $Id
    $script:ArSteps = @([pscustomobject]@{ name = 'x'; seconds = 0.0; pass = $true })
    Start-ArSummary 'fast'
    Invoke-ArFastTests -PerTest 0.41 -Budget $Budget -Log '' -Names @('fxnone') -IntervalMs 100 -WaiveB08:$Waive -Tests { } -Step { param($n, $b) [pscustomobject]@{ Code = $Code; Out = $Lines } }
  }
  $slow = @('  0.900000 s: slowcase', '  0.100000 s: other')
  $quick = @('  0.100000 s: other')
  foreach ($id in $ref, 'fakebox-4c8t') {
    $t = Invoke-FastWith $id 1 10 $slow
    Check "a failed test run is not a timing result on $id" ($t.Result.Code -eq 1 -and -not $t.Over -and -not $t.Rerun)
  }
  $t = Invoke-FastWith $ref 0 10 $slow
  Check 'B-12 over its limit is Over on the reference machine' ($t.Over)
  $t = Invoke-FastWith 'fakebox-4c8t' 0 10 $slow
  Check 'B-12 over its limit is not Over on another machine' (-not $t.Over)
  $t = Invoke-FastWith $ref 0 0.0000001 $quick
  Check 'B-08 over its limit is Over on the reference machine' ($t.Over)
  $t = Invoke-FastWith $ref 0 0.0000001 $quick -Waive
  Check 'the profile measuring runs leave B-08 unjudged' (-not $t.Over)
  $t = Invoke-FastWith $ref 0 0.0000001 $slow -Waive
  Check 'the profile measuring runs still judge B-12' ($t.Over)
} finally {
  $script:ArMachineIdForTest = ''
  if ($null -ne $saveEnv) { $env:ASTRORAY_OFF_REFERENCE = $saveEnv } else { Remove-Item Env:ASTRORAY_OFF_REFERENCE -ErrorAction SilentlyContinue }
}
if ($bad) { exit 1 }
Write-Host 'PASS fast_machine_profile'
