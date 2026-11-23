# SPDX-License-Identifier: Apache-2.0
# Runs `land.ps1 prepare 999` on fixture cases a-j, asserts each outcome and exits non-zero on any mismatch
# (tasks 010, 138). Expected, with -DryRun: a refused "canary inert", c refused "missing canary patch", e refused
# (its command names a missing script), b and d accepted, f accepted with a WARN on its inert hunk, g refused (its
# case turns red only with both hunks). i refused while another process has land.ps1 in its command line, j refused
# while a process has out/bin/astroray_core.dll loaded. h runs without -DryRun against a failing gate stub: exit 1,
# the "(land)" commit undone, the task file back, the tree clean. A `gh` stub (fixture bin/) keeps h off GitHub.
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
  h = @{ Exit = 1; Match = 'undid \(land\) commit'; Live = $true }
  i = @{ Exit = 1; Match = 'REFUSE land: another land\.ps1 is running \(pid \d+, started ' }
  j = @{ Exit = 1; Match = 'REFUSE land: out/bin/astroray_core\.dll is loaded by pwsh \(pid \d+\)' }
}
$bad = 0
$helper = $null
foreach ($c in $expect.Keys) {
  $work = & pwsh -NoProfile -File (Join-Path $here 'make_fixture.ps1') -Case $c -Dir (Join-Path $Base $c) | Select-Object -Last 1
  Write-Host "--- case ($c)"
  $e = $expect[$c]
  $extra = @()
  try {
    if ($c -eq 'i') {
      # Not an ancestor of the land.ps1 under test, only a process that mentions it.
      $helper = Start-Process pwsh -ArgumentList @('-NoProfile', '-Command', 'Start-Sleep 120 # land.ps1') -PassThru -WindowStyle Hidden
      Start-Sleep -Milliseconds 800
    }
    if ($c -eq 'j') {
      # A copy of a system DLL under the worktree's DLL name, loaded for real: the file is then held like a leftover Blender holds it.
      $dll = Join-Path $work 'out/bin/astroray_core.dll'
      Copy-Item (Join-Path $env:windir 'System32/msimg32.dll') $dll
      $ready = Join-Path (Join-Path $Base $c) 'ready.txt'
      $cmd = "[Runtime.InteropServices.NativeLibrary]::Load('$dll') | Out-Null; Set-Content '$ready' 1; Start-Sleep 120"
      $helper = Start-Process pwsh -ArgumentList @('-NoProfile', '-Command', $cmd) -PassThru -WindowStyle Hidden
      $hpid = $helper.Id
      $t = [Diagnostics.Stopwatch]::StartNew()
      while (-not (Test-Path $ready) -and $t.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 200 }
      if (-not (Test-Path $ready)) { Write-Host 'MISMATCH case (j): the helper never loaded the DLL' }
    }
    Push-Location $work
    $oldPath = $env:PATH
    try {
      $env:PATH = (Join-Path (Join-Path $Base $c) 'bin') + [IO.Path]::PathSeparator + $env:PATH
      $landArgs = @('prepare', '999') + $(if ($e.Live) { @() } else { @('-DryRun') })
      $out = & pwsh -NoProfile -File $land @landArgs 2>&1 | ForEach-Object { "$_" }
      $code = $LASTEXITCODE
    } finally { $env:PATH = $oldPath; Pop-Location }
    if ($e.Live) {
      $subject = (& git -C $work log -1 --format=%s)
      if ($subject -match '\(land\)$') { $extra += "a '(land)' commit is left on the branch: $subject" }
      if (-not (Test-Path (Join-Path $work 'tasks/999-fixture.md'))) { $extra += 'the task file is gone' }
      if (& git -C $work status --porcelain) { $extra += 'the tree is not clean' }
    }
  } finally {
    if ($helper) { Stop-Process -Id $helper.Id -Force -ErrorAction SilentlyContinue; $helper = $null }
  }
  $out | ForEach-Object { Write-Host $_ }
  Write-Host "exit $code"
  $hit = @($out | Where-Object { $_ -match $e.Match }).Count -gt 0
  if ($e.Also -and -not @($out | Where-Object { $_ -match $e.Also }).Count) { $hit = $false }
  if ($c -eq 'j' -and $hit -and -not (@($out | Where-Object { $_ -match "loaded by pwsh \(pid $hpid\)" }).Count)) { $hit = $false }
  if ($code -ne $e.Exit -or -not $hit -or $extra.Count) {
    Write-Host "MISMATCH case ($c): expected exit $($e.Exit) and a line matching '$($e.Match)'$(if ($extra.Count) { '; ' + ($extra -join '; ') })"
    $bad++
  }
}
if ($bad -gt 0) { Write-Host "FAIL run_cases: $bad case(s) did not behave as expected"; exit 1 }
Write-Host 'PASS run_cases: a, c, e, g, i, j refused; b, d, f accepted; h gate failure undone'
