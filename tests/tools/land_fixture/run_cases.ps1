# SPDX-License-Identifier: Apache-2.0
# Runs `land.ps1 prepare 999 -DryRun` on fixture cases a-g, asserts each outcome and exits non-zero
# on any mismatch (task 010 acceptance). Expected: a refused "canary inert", c refused "missing canary
# patch", e refused (its command names a missing script), b and d accepted, f accepted with a
# WARN on its inert hunk, g refused (its case turns red only with both hunks).
param([string]$Base = (Join-Path ([IO.Path]::GetTempPath()) 'astroray_land_fixture'))
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$land = Join-Path (Resolve-Path (Join-Path $here '../../../tools')).Path 'land.ps1'
$expect = [ordered]@{
  a = @{ Exit = 1; Match = 'REFUSE land: canary inert: fixture/new case still passes' }
  b = @{ Exit = 0; Match = 'PASS prepare 999' }
  c = @{ Exit = 1; Match = 'REFUSE land: missing canary patch' }
  d = @{ Exit = 0; Match = 'PASS prepare 999' }
  e = @{ Exit = 1; Match = "REFUSE land: canary item errored: .*script 'tools/no_such_script.ps1' does not exist" }
  f = @{ Exit = 0; Match = 'PASS prepare 999'; Also = 'WARN canary: inert hunk src/code.txt:' }
  g = @{ Exit = 1; Match = "REFUSE land: canary 'fixture/new case' turns red only with several hunks together" }
}
$bad = 0
foreach ($c in $expect.Keys) {
  $work = & pwsh -NoProfile -File (Join-Path $here 'make_fixture.ps1') -Case $c -Dir (Join-Path $Base $c) | Select-Object -Last 1
  Write-Host "--- case ($c)"
  Push-Location $work
  try {
    $out = & pwsh -NoProfile -File $land prepare 999 -DryRun 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
  } finally { Pop-Location }
  $out | ForEach-Object { Write-Host $_ }
  Write-Host "exit $code"
  $e = $expect[$c]
  $hit = @($out | Where-Object { $_ -match $e.Match }).Count -gt 0
  if ($e.Also -and -not @($out | Where-Object { $_ -match $e.Also }).Count) { $hit = $false }
  if ($code -ne $e.Exit -or -not $hit) {
    Write-Host "MISMATCH case ($c): expected exit $($e.Exit) and a line matching '$($e.Match)'"
    $bad++
  }
}
if ($bad -gt 0) { Write-Host "FAIL run_cases: $bad case(s) did not behave as expected"; exit 1 }
Write-Host 'PASS run_cases: a, c, e, g refused; b, d, f accepted'
