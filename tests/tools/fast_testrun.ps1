# SPDX-License-Identifier: Apache-2.0
# tools/lib/testrun.ps1 Invoke-ArTestCase on a stand-in test binary, as the land fixture uses one
# (out/bin/astroray_tests_cpu.cmd printing a doctest summary): a passing case, a failing case, and a name
# that matches no case, which test.ps1's verdict must call a FAIL. Run by tests/tools/run_fast.ps1.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/lib/testrun.ps1')
$bad = 0
function Check([string]$what, [bool]$ok) { if (-not $ok) { Write-Host "FAIL fast_testrun: $what"; $script:bad++ } }
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_testrun_$PID"
New-Item -ItemType Directory -Force (Join-Path $tmp 'out/bin') | Out-Null
try {
  # A batch file, not pwsh, so three runs cost milliseconds. cmd splits arguments at '=', so match on %*.
  $standIn = @(
    '@echo off'
    'echo %* | findstr /c:"--tier=all -tc=fixture/pass" >nul && (echo [doctest] test cases: 1 ^| 1 passed ^| 0 failed ^| 0 skipped& exit /b 0)'
    'echo %* | findstr /c:"--tier=all -tc=fixture/fail" >nul && (echo CHECK^( x == 1 ^) is NOT correct!& echo [doctest] test cases: 1 ^| 0 passed ^| 1 failed ^| 0 skipped& exit /b 1)'
    'echo [doctest] test cases: 0 ^| 0 passed ^| 0 failed ^| 0 skipped'
    'exit /b 0'
  ) -join "`r`n"
  [IO.File]::WriteAllText((Join-Path $tmp 'out/bin/astroray_tests_cpu.cmd'), $standIn + "`r`n")

  $r = @(Invoke-ArTestCase -Kind exe -Target astroray_tests_cpu -Case 'fixture/pass' -Root $tmp)
  Check 'pass: one result that ran and passed' ($r.Count -eq 1 -and $r[0].Ran -and -not $r[0].Failed -and -not $r[0].Error)
  Check 'pass: verdict PASS' ((Get-ArTestVerdict $r).Pass)
  Check 'pass: summary line shown' (@(@(Select-ArTestLines $r[0].Output 20) -match 'test cases: 1 \| 1 passed').Count -eq 1)

  $r = @(Invoke-ArTestCase -Kind exe -Target astroray_tests_cpu -Case 'fixture/fail' -Root $tmp)
  Check 'fail: ran and failed' ($r[0].Ran -and $r[0].Failed -and -not $r[0].Error)
  $v = Get-ArTestVerdict $r
  Check 'fail: verdict FAIL' (-not $v.Pass -and $v.Reason -match 'failed')
  Check 'fail: the failing check is shown' (@(@(Select-ArTestLines $r[0].Output 20) -match 'NOT correct').Count -eq 1)

  $r = @(Invoke-ArTestCase -Kind exe -Target astroray_tests_cpu -Case 'fixture/no such case' -Root $tmp)
  Check 'no such case: did not run' (-not $r[0].Ran -and -not $r[0].Error)
  $v = Get-ArTestVerdict $r
  Check 'no such case: verdict FAIL' (-not $v.Pass -and $v.Reason -match 'no matching test case')

  $r = @(Invoke-ArTestCase -Kind exe -Target astroray_tests_gpu -Case 'fixture/pass' -Root $tmp)
  Check 'missing binary: an error, not a pass' ($r[0].Error -match 'missing out/bin/astroray_tests_gpu' -and -not (Get-ArTestVerdict $r).Pass)
  # A Blender test that prints after unittest wrote `... `: its outcome is a bare line of its own.
  $ut = "test_a (__main__.T.test_a) ... render | Saved: x.exr`nRESULT a`nok`ntest_b (__main__.T.test_b) ... printed`nFAIL`nRan 2 tests in 1.0s`nFAILED (failures=1)"
  $r = @(Get-ArCaseResults @('T.test_a', 'T.test_b') $ut '')
  Check 'bare ok after printed lines' ($r[0].Ran -and -not $r[0].Failed)
  Check 'bare FAIL after printed lines' ($r[1].Ran -and $r[1].Failed)
  Check 'unittest summary shown, outcome lines not' ((@(Select-ArTestLines $ut 20) -join '|') -ceq 'Ran 2 tests in 1.0s|FAILED (failures=1)|FAIL')

  # B-12 on a canned --duration output: the slowest case is named, a case over the limit is listed, a case
  # at the limit passes, and an output without duration lines fails loudly.
  $dur = @('[doctest] doctest version is "2.4.11"', '0.020 s: a/quick', '0.380 s: b/slowest ok', '0.410 s: c/at the limit', '0.001 s: d/tiny',
           '[doctest] test cases: 4 | 4 passed | 0 failed | 0 skipped')
  $s = Get-ArSlowTests $dur 0.41
  Check 'B-12: the slowest case is named' ($s.Slowest.Case -ceq 'c/at the limit' -and $s.Slowest.Seconds -eq 0.41)
  Check 'B-12: a case at the limit is not slow' ($s.Slow.Count -eq 0)
  $s = Get-ArSlowTests ($dur + '0.438 s: e/over the limit') 0.41
  Check 'B-12: a case over the limit is listed' ($s.Slow.Count -eq 1 -and $s.Slow[0].Case -ceq 'e/over the limit' -and $s.Slowest.Case -ceq 'e/over the limit')
  $threw = $false
  try { Get-ArSlowTests @('[doctest] test cases: 1 | 1 passed | 0 failed | 0 skipped') 0.41 | Out-Null } catch { $threw = $_.Exception.Message -match 'no duration lines' }
  Check 'B-12: an output without duration lines fails loudly' $threw
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
if ($bad) { Write-Host "FAIL fast_testrun: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_testrun'
