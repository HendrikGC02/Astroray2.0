# SPDX-License-Identifier: Apache-2.0
# Builds a throwaway local repo (bare origin + clone on branch task/999-fixture) for
# `tools/land.ps1 prepare 999 -DryRun` (task 010). Prints the clone path on the last line.
#   a: test change with an inert canary      b: test change with a real canary
#   c: test change with no canary patch      d: real canary on a mixed line (backticked command,
#                                              parenthesised text, two names, one in suite `gate`)
#   e: real canary patch, but the backticked command names a script that does not exist
# The build and test binary are stand-ins: tools/build.ps1 does nothing and out/bin/astroray_tests_cpu.cmd
# "passes" a case while src/code.txt contains GOOD, so a patch that flips it turns the case red.
param(
  [Parameter(Mandatory)][ValidateSet('a', 'b', 'c', 'd', 'e')][string]$Case,
  [Parameter(Mandatory)][string]$Dir
)
$ErrorActionPreference = 'Stop'
if (Test-Path $Dir) { Remove-Item -Recurse -Force $Dir }
New-Item -ItemType Directory -Force $Dir | Out-Null
$Dir = (Resolve-Path $Dir).Path
$origin = Join-Path $Dir 'origin.git'
$work = Join-Path $Dir 'work'
function G { & git -C $work @args 2>&1 | Out-Null; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }
function Put([string]$rel, [string]$text) {
  $p = Join-Path $work $rel
  New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null
  [IO.File]::WriteAllText($p, $text.Replace("`r`n", "`n"))
}

& git init -q --bare -b main $origin
& git init -q -b main $work
G config user.email fixture@example.invalid
G config user.name fixture
G config core.autocrlf false
G remote add origin $origin

$canary = switch ($Case) {
  'd' { 'tasks/999.canary.patch must turn red: `pwsh -NoProfile -File tools/fixture_selftest.ps1` (the patch flips GOOD, which, also, breaks the cases), astroray_tests_cpu "gate_fixture/gate case" and "fixture/new case".' }
  'e' { 'tasks/999.canary.patch must turn red: `pwsh -NoProfile -File tools/no_such_script.ps1`, astroray_tests_cpu "fixture/new case".' }
  default { 'tasks/999.canary.patch must turn red: astroray_tests_cpu "fixture/new case".' }
}
Put '.gitignore' "out/`n"
Put 'src/code.txt' "GOOD`n"
Put 'src/other.txt' "x`n"
Put 'tests/cpu/test_base.cpp' "TEST_CASE(`"base/other`") {}`n"
Put 'tools/build.ps1' "# stand-in for the real build: nothing to build in the fixture`nparam([string[]]`$Target)`nWrite-Host 'PASS build 0.0 s (fixture)'`n"
Put 'tools/fixture_selftest.ps1' "if ((Get-Content src/code.txt -Raw) -match 'GOOD') { exit 0 } else { exit 1 }`n"
Put 'tools/fixture_tests.ps1' @'
param([Parameter(ValueFromRemainingArguments)]$a)
$name = (($a | Where-Object { $_ -like '-tc=*' }) -replace '^-tc=', '' -replace '\\,', ',')
$n = 0
foreach ($f in Get-ChildItem tests/cpu -Filter *.cpp) {
  $n += ([regex]::Matches((Get-Content $f.FullName -Raw), 'TEST_CASE\("' + [regex]::Escape($name) + '"')).Count
}
$pass = (Get-Content src/code.txt -Raw) -match 'GOOD'
$failed = if ($n -gt 0 -and -not $pass) { $n } else { 0 }
Write-Host "[doctest] test cases: $n | $($n - $failed) passed | $failed failed | 0 skipped"
exit $(if ($failed) { 1 } else { 0 })
'@
Put 'tasks/999-fixture.md' "# 999 Fixture task`nGoal: fixture.`nLook at: none`nCanary: $canary`nProgress:`n"
G add -A
G commit -q -m 'fixture base'
G push -q origin main
G checkout -q -b task/999-fixture

Put 'tests/cpu/test_fixture.cpp' "TEST_SUITE(`"gate`") { TEST_CASE(`"gate_fixture/gate case`") {} }`nTEST_CASE(`"fixture/new case`") {}`n"
if ($Case -ne 'c') {
  # Canary patch: a touches an unrelated file (inert); b, d and e flip the code under test.
  $f = if ($Case -eq 'a') { 'src/other.txt' } else { 'src/code.txt' }
  Put $f $(if ($Case -eq 'a') { "y`n" } else { "BAD`n" })
  & git -C $work diff "--output=$(Join-Path $work 'tasks/999.canary.patch')"
  G checkout -q -- $f
}
G add -A
G commit -q -m 'fixture task work'

# Stand-in test binary (untracked: out/ is ignored).
$bin = Join-Path $work 'out/bin'
New-Item -ItemType Directory -Force $bin | Out-Null
[IO.File]::WriteAllText((Join-Path $bin 'astroray_tests_cpu.cmd'),
  "@echo off`r`npwsh -NoProfile -File `"%~dp0..\..\tools\fixture_tests.ps1`" %*`r`n")
Write-Output $work
