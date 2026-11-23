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
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
if ($bad) { Write-Host "FAIL fast_testrun: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_testrun'
