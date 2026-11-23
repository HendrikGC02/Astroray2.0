# SPDX-License-Identifier: Apache-2.0
# CPU-only checks of tools/lib/landing.ps1 on a real throwaway repo: cleanup (a worktree whose out/ holds an open
# file ends in LEFTOVER lines and returns, a clean one is removed), exact branch choice, and the history line in the
# main checkout. Run by tests/tools/run_fast.ps1 (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/lib/landing.ps1')
$bad = 0
function Check([string]$what, [bool]$ok) { if (-not $ok) { Write-Host "FAIL fast_landing: $what"; $script:bad++ } }
function G { & git -C $main @args 2>&1 | Out-Null; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }

$tmp = Join-Path ([IO.Path]::GetTempPath()) "astroray_fast_landing_$PID"
$main = Join-Path $tmp 'main'
$stream = $null
try {
  New-Item -ItemType Directory -Force $tmp | Out-Null
  & git init -q --bare -b main (Join-Path $tmp 'origin.git')
  & git clone -q (Join-Path $tmp 'origin.git') $main 2>$null
  G -c user.email=t@example.invalid -c user.name=t commit -q --allow-empty -m base
  G push -q origin HEAD:main
  G branch task/12-x
  G worktree add -q -b task/120-y (Join-Path $tmp 'task_120-y')
  & git -C (Join-Path $tmp 'origin.git') branch task/120-y main
  G worktree add -q -b task/121-z (Join-Path $tmp 'task_121-z')
  G push -q origin task/121-z
  $mainPath = Get-LandingMain $main
  Check 'main checkout is the first worktree' ((Resolve-Path $mainPath).Path -eq (Resolve-Path $main).Path)

  # The exact branch: task/12-x is not 120, nothing is 999.
  $t120 = Find-LandingTask $main '120'
  Check 'task/120-y is chosen for 120' ($t120.Branch -eq 'task/120-y')
  Check 'task/12-x is not chosen for 120' ($t120.Branch -ne 'task/12-x')
  Check 'no branch for 999' ($null -eq (Find-LandingTask $main '999'))

  # A worktree whose out/ holds an open file: LEFTOVER lines, the count returned, nothing thrown, the other task untouched.
  $wt120 = Join-Path $tmp 'task_120-y'
  New-Item -ItemType Directory -Force (Join-Path $wt120 'out') | Out-Null
  $stream = [IO.File]::Open((Join-Path $wt120 'out/held.bin'), 'Create', 'ReadWrite', 'None')
  $o = @(Invoke-LandingCleanup -Main $main -Task $t120 -SleepS 0 6>&1 | ForEach-Object { "$_" })
  Check 'the leftover count is the last output' ([int]$o[-1] -gt 0)
  Check 'a LEFTOVER line names the worktree with its fix' (@($o | Where-Object { $_ -match '^LEFTOVER: worktree .*task_120-y; fix: git worktree remove -f -f ' }).Count -eq 1)
  Check 'the remote branch is deleted although the worktree is not' (@(& git -C $main ls-remote --heads origin task/120-y).Count -eq 0)
  & git -C $main show-ref --verify --quiet refs/heads/task/12-x
  Check 'task/12-x survives the cleanup of 120' ($LASTEXITCODE -eq 0)
  $stream.Dispose(); $stream = $null

  # A clean worktree: removed, local and remote branch deleted, nothing left over.
  $t121 = Find-LandingTask $main '121'
  $o = @(Invoke-LandingCleanup -Main $main -Task $t121 -SleepS 0 6>&1 | ForEach-Object { "$_" })
  Check 'a clean cleanup returns 0' ([int]$o[-1] -eq 0)
  Check 'the worktree directory is gone' (-not (Test-Path (Join-Path $tmp 'task_121-z')))
  & git -C $main show-ref --verify --quiet refs/heads/task/121-z
  Check 'the local branch is deleted' ($LASTEXITCODE -ne 0)
  Check 'the remote branch is deleted' (@(& git -C $main ls-remote --heads origin task/121-z).Count -eq 0)

  # The running-land marker is per repository: claimed, reclaimed from a dead owner, released.
  Check 'the marker is claimed' ($null -eq (Enter-LandRunning $main))
  Check 'the marker file exists' (Test-Path $script:LandMarker)
  Exit-LandRunning
  $dead = @{ pid = 999999; start = (Get-Date).ToString('o') } | ConvertTo-Json -Compress
  Set-Content (Get-LandingMarker $main) $dead
  Check 'a dead owner is reclaimed' ($null -eq (Enter-LandRunning $main))
  Exit-LandRunning
  Check 'the marker is released' (-not (Test-Path (Get-LandingMarker $main)))

  # History: one JSON line per landing in the main checkout; the last gate_s comes back.
  Check 'no history, no previous gate' ($null -eq (Get-LandingLastGate $main))
  Add-LandingHistory $main ([ordered]@{ task = 5; sha = 'abc'; gate_s = 41.5 })
  Add-LandingHistory $main ([ordered]@{ task = 7; sha = 'def'; gate_s = 43.0 })
  $lines = @(Get-Content (Join-Path $main 'out/land/history.jsonl'))
  Check 'two history lines' ($lines.Count -eq 2)
  $last = Get-LandingLastGate $main
  Check 'the previous landing is the last line' ($last.task -eq 7 -and $last.gate_s -eq 43.0)
} finally {
  Set-Location $root
  if ($stream) { $stream.Dispose() }
  & git -C $main worktree prune 2>$null
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { exit 1 }
Write-Host 'PASS fast_landing'
