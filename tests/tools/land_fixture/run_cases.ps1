# SPDX-License-Identifier: Apache-2.0
# Runs `land.ps1 prepare 999` on fixture cases a-k, asserts each outcome and exits non-zero on any mismatch
# (tasks 010, 138). Expected, with -DryRun: a refused "canary inert", c refused "missing canary patch", e refused
# (its command names a missing script), b and d accepted (d, whose canary built `all`, rebuilt `all` after it), f accepted with a WARN on its inert hunk, g refused (its
# case turns red only with both hunks). i refused while a live land.ps1 is registered for the same repository, j refused
# while a process has out/bin/astroray_core.dll loaded. h runs without -DryRun against a failing gate stub: exit 1,
# the pushed "(land)" commit undone by a pushed revert: the task file back locally and on origin, the tree clean. k
# prepares once for real (gate stub passes, its "(land)" commit pushed), then main moves with a conflicting change and
# a second prepare drops that commit and is refused at the rebase, before its own commit: origin must not keep the
# "(land)" commit (task file there, local head contained), no revert in progress. A `gh` stub (fixture bin/) keeps
# h and k off GitHub.
param([string]$Base = (Join-Path ([IO.Path]::GetTempPath()) 'astroray_land_fixture'))
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$land = Join-Path (Resolve-Path (Join-Path $here '../../../tools')).Path 'land.ps1'
$expect = [ordered]@{
  a = @{ Exit = 1; Match = 'REFUSE land: canary inert: fixture/new case still passes' }
  b = @{ Exit = 0; Match = 'PASS prepare 999' }
  c = @{ Exit = 1; Match = 'REFUSE land: missing canary patch' }
  d = @{ Exit = 0; Match = 'PASS prepare 999'; Also = '^PASS build all ' }
  e = @{ Exit = 1; Match = "REFUSE land: canary item errored: .*script 'tools/no_such_script.ps1' does not exist" }
  f = @{ Exit = 0; Match = 'PASS prepare 999'; Also = 'WARN canary: inert hunk src/code.txt:' }
  g = @{ Exit = 1; Match = "REFUSE land: canary 'fixture/new case' turns red only with several hunks together" }
  h = @{ Exit = 1; Match = 'undid \(land\) commit'; Also = '^gate stub args: gate -Landing$'; Live = $true }
  i = @{ Exit = 1; Match = 'REFUSE land: another land\.ps1 is running \(pid \d+, started ' }
  j = @{ Exit = 1; Match = 'REFUSE land: out/bin/astroray_core\.dll is loaded by pwsh \(pid \d+\)' }
  k = @{ Exit = 1; Match = 'REFUSE land: rebase on origin/main conflicts in: tests/cpu/test_fixture\.cpp'; Also = 'undid \(land\) commit [0-9a-f]{9} on origin'; Live = $true }
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
      # A running land.ps1 registers in its repository's marker (lib/landing.ps1 Enter-LandRunning); this one does so for the helper.
      $marker = @{ pid = $helper.Id; start = (Get-Process -Id $helper.Id).StartTime.ToString('o') } | ConvertTo-Json -Compress
      Set-Content (Join-Path $work '.git/astroray-land.json') $marker
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
      if ($c -eq 'k') {
        $first = & pwsh -NoProfile -File $land prepare 999 2>&1 | ForEach-Object { "$_" }
        if (-not @($first | Where-Object { $_ -match '^PASS prepare 999' }).Count) { $extra += "the first prepare did not pass: $(($first | Select-Object -Last 2) -join ' ')" }
        # main moves with a change that conflicts with the task branch, so the second prepare stops at its rebase.
        $other = Join-Path (Join-Path $Base $c) 'other'
        & git clone -q (Join-Path (Join-Path $Base $c) 'origin.git') $other 2>&1 | Out-Null
        [IO.File]::WriteAllText((Join-Path $other 'tests/cpu/test_fixture.cpp'), "// main's own file`n")
        & git -C $other add -A 2>&1 | Out-Null
        & git -C $other -c user.email=f@example.invalid -c user.name=f commit -q -m 'main moves' 2>&1 | Out-Null
        & git -C $other push -q origin main 2>&1 | Out-Null
      }
      $landArgs = @('prepare', '999') + $(if ($e.Live) { @() } else { @('-DryRun') })
      $out = & pwsh -NoProfile -File $land @landArgs 2>&1 | ForEach-Object { "$_" }
      $code = $LASTEXITCODE
    } finally { $env:PATH = $oldPath; Pop-Location }
    if ($e.Live) {
      $subject = (& git -C $work log -1 --format=%s)
      if ($subject -match '\(land\)$') { $extra += "a '(land)' commit is left on the branch: $subject" }
      if (-not (Test-Path (Join-Path $work 'tasks/999-fixture.md'))) { $extra += 'the task file is gone' }
      & git -C (Join-Path (Join-Path $Base $c) 'origin.git') cat-file -e 'task/999-fixture:tasks/999-fixture.md' 2>$null
      if ($LASTEXITCODE -ne 0) { $extra += 'origin task/999-fixture has no task file' }
      & git -C (Join-Path (Join-Path $Base $c) 'origin.git') merge-base --is-ancestor $(& git -C $work rev-parse HEAD) task/999-fixture 2>$null
      if ($LASTEXITCODE -ne 0) { $extra += 'origin task/999-fixture does not carry the local head' }
      if (& git -C $work status --porcelain) { $extra += 'the tree is not clean' }
      if (Test-Path (Join-Path $work '.git/REVERT_HEAD')) { $extra += 'a revert is in progress' }
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
Write-Host 'PASS run_cases: a, c, e, g, i, j refused; b, d, f accepted; h gate failure undone; k origin restored after a re-prepare'
