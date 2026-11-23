# SPDX-License-Identifier: Apache-2.0
# CPU-only checks of tools/lib/landing.ps1 on a real throwaway repo: cleanup (a worktree whose out/ holds an open
# file ends in a LEFTOVER line whose fix works once the file is closed; an unregistered leftover directory is
# deleted under .claude/worktrees/ and only reported outside it; a clean worktree is removed), exact branch choice, the running-land marker, the merge and PR-create
# calls after a 5xx (a `gh` stub), and the history line in the main checkout. Run by tests/tools/run_fast.ps1
# (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/lib/landing.ps1')
$bad = 0
function Check([string]$what, [bool]$ok) { if (-not $ok) { Write-Host "FAIL fast_landing: $what"; $script:bad++ } }
function G { & git -C $main @args 2>&1 | Out-Null; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }
function Find-Safe([string]$n) { try { return Find-LandingTask $main $n } catch { Write-Host "fast_landing: Find-LandingTask $n threw: $($_.Exception.Message)"; return $null } }

$tmp = Join-Path ([IO.Path]::GetTempPath()) "astroray_fast_landing_$PID"
$main = Join-Path $tmp 'main'
$wts = Join-Path $main '.claude/worktrees'   # where Claude agent worktrees live; cleanup deletes directories only there
$stream = $null
try {
  New-Item -ItemType Directory -Force $tmp | Out-Null
  & git init -q --bare -b main (Join-Path $tmp 'origin.git')
  & git clone -q (Join-Path $tmp 'origin.git') $main 2>$null
  G -c user.email=t@example.invalid -c user.name=t commit -q --allow-empty -m base
  G push -q origin HEAD:main
  G worktree add -q -b task/1201-q (Join-Path $wts 'task_1201-q')
  G branch task/9990-n
  G worktree add -q -b task/120-y (Join-Path $wts 'task_120-y')
  & git -C (Join-Path $tmp 'origin.git') branch task/120-y main
  G worktree add -q -b task/121-z (Join-Path $wts 'task_121-z')
  G push -q origin task/121-z
  G worktree add -q -b task/1202-o (Join-Path $tmp 'task_1202-o')
  $mainPath = Get-LandingMain $main
  Check 'main checkout is the first worktree' ((Resolve-Path $mainPath).Path -eq (Resolve-Path $main).Path)

  # The exact branch: a worktree on task/1201-q is not 120, a plain branch task/9990-n is not 999.
  $t120 = Find-Safe '120'
  Check 'task/120-y is chosen for 120 (task/1201-q is not)' ($t120 -and $t120.Branch -eq 'task/120-y')
  Check 'no branch for 999 (task/9990-n is not)' ($null -eq (Find-Safe '999'))

  # A worktree whose out/ holds an open file: git unregisters it but cannot delete it. One LEFTOVER line names the
  # directory, which exists; its fix works once the file is closed. The remote branch goes, task/1201-q stays.
  $wt120 = Join-Path $wts 'task_120-y'
  New-Item -ItemType Directory -Force (Join-Path $wt120 'out') | Out-Null
  $stream = [IO.File]::Open((Join-Path $wt120 'out/held.bin'), 'Create', 'ReadWrite', 'None')
  $o = @(Invoke-LandingCleanup -Main $main -Task $t120 -SleepS 0 6>&1 | ForEach-Object { "$_" })
  Check 'the leftover count is the last output' ([int]$o[-1] -gt 0)
  $lo = @($o | Where-Object { $_ -match '^LEFTOVER: worktree .*task_120-y.*; fix: (.+)$' })
  Check 'one LEFTOVER line names the worktree' ($lo.Count -eq 1)
  Check 'the directory it names exists' (Test-Path $wt120)
  Check 'the remote branch is deleted although the worktree is not' (@(& git -C $main ls-remote --heads origin task/120-y).Count -eq 0)
  & git -C $main show-ref --verify --quiet refs/heads/task/1201-q
  Check 'task/1201-q survives the cleanup of 120' ($LASTEXITCODE -eq 0)
  $stream.Dispose(); $stream = $null
  if ($lo.Count -eq 1) {
    $fix = [regex]::Match($lo[0], '; fix: (.+)$').Groups[1].Value -replace '^close that process, then ', ''
    try { Invoke-Expression $fix 2>&1 | Out-Null } catch { }
    Check "the printed fix removes the directory once the file is closed ($fix)" (-not (Test-Path $wt120))
  }

  # Unregistered but still on disk (what a failed `git worktree remove` leaves): the directory itself is deleted.
  # task/1201-q's worktree is reused here: its lookup checks are done.
  $t1201 = Find-Safe '1201'
  Remove-Item -Recurse -Force (Join-Path $main '.git/worktrees/task_1201-q')
  $o = @(Invoke-LandingCleanup -Main $main -Task $t1201 -SleepS 0 6>&1 | ForEach-Object { "$_" })
  Check 'an unregistered leftover directory is deleted' ((-not (Test-Path (Join-Path $wts 'task_1201-q'))) -and [int]$o[-1] -eq 0)

  # The same outside .claude/worktrees/: the directory is never deleted here, one LEFTOVER line gives the fix.
  $wt1202 = Join-Path $tmp 'task_1202-o'
  $t1202 = Find-Safe '1202'
  Remove-Item -Recurse -Force (Join-Path $main '.git/worktrees/task_1202-o')
  $o = @(Invoke-LandingCleanup -Main $main -Task $t1202 -SleepS 0 6>&1 | ForEach-Object { "$_" })
  Check 'an unregistered directory outside .claude/worktrees survives' (Test-Path $wt1202)
  Check 'one LEFTOVER line gives its Remove-Item fix' (@($o | Where-Object { $_ -match "^LEFTOVER: worktree directory .*task_1202-o.*; fix: .*Remove-Item -LiteralPath '.*task_1202-o' -Recurse -Force$" }).Count -eq 1)

  # A clean worktree: removed, local and remote branch deleted, nothing left over.
  $t121 = Find-Safe '121'
  $o = @(Invoke-LandingCleanup -Main $main -Task $t121 -SleepS 0 6>&1 | ForEach-Object { "$_" })
  Check 'a clean cleanup returns 0' ([int]$o[-1] -eq 0)
  Check 'the worktree directory is gone' (-not (Test-Path (Join-Path $wts 'task_121-z')))
  & git -C $main show-ref --verify --quiet refs/heads/task/121-z
  Check 'the local branch is deleted' ($LASTEXITCODE -ne 0)
  Check 'the remote branch is deleted' (@(& git -C $main ls-remote --heads origin task/121-z).Count -eq 0)

  # The running-land marker is per repository: claimed, reclaimed from a dead owner, released. An empty marker is a
  # land.ps1 between creating and writing it while young, stale when old; a stale marker replaced since it was read
  # is not deleted.
  $mk = Get-LandingMarker $main
  Check 'the marker is claimed' ($null -eq (Enter-LandRunning $main))
  Check 'the marker file exists' (Test-Path $script:LandMarker)
  Exit-LandRunning
  $dead = @{ pid = 999999; start = (Get-Date).ToString('o') } | ConvertTo-Json -Compress
  Set-Content $mk $dead
  Check 'a dead owner is reclaimed' ($null -eq (Enter-LandRunning $main))
  Exit-LandRunning
  Check 'the marker is released' (-not (Test-Path $mk))
  [IO.File]::WriteAllText($mk, '')
  Check 'a young empty marker is live' ($null -ne (Enter-LandRunning $main))
  (Get-Item $mk).LastWriteTimeUtc = (Get-Date).ToUniversalTime().AddMinutes(-1)
  Check 'an old empty marker is reclaimed' ($null -eq (Enter-LandRunning $main))
  Exit-LandRunning
  [IO.File]::WriteAllText($mk, 'fresh owner')
  Remove-LandingStaleMarker $mk 'stale owner'
  Check 'a marker replaced since it was read survives' ((Test-Path $mk) -and [IO.File]::ReadAllText($mk) -eq 'fresh owner')
  Remove-LandingStaleMarker $mk 'fresh owner'
  Check 'a marker still holding the stale text is removed' (-not (Test-Path $mk))
  Check 'no renamed marker is left behind' (@(Get-ChildItem (Split-Path $mk) -Filter 'astroray-land.json*').Count -eq 0)

  # Merge and PR create are not idempotent: after a 5xx that persists, the PR itself decides.
  $script:GhDelays = @(0, 0, 0)
  $script:ghState = 'MERGED'
  function gh {
    $a = $args -join ' '
    if ($a -like 'pr merge*' -or $a -like 'pr create*') { $global:LASTEXITCODE = 1; return 'HTTP 502: Bad Gateway' }
    if ($a -like 'pr view*') { $global:LASTEXITCODE = 0; return (@{ state = $script:ghState; mergeCommit = @{ oid = 'c0ffee0123456789' } } | ConvertTo-Json -Compress) }
    if ($a -like 'pr list*') { $global:LASTEXITCODE = 0; return '[{"number":7}]' }
    $global:LASTEXITCODE = 1; return "unexpected gh $a"
  }
  $mc = $null
  try { $mc = Invoke-LandingPrMerge 5 'abc' 6>$null } catch { }
  Check 'a merge that failed after a 5xx but is MERGED returns its merge commit' ($mc -eq 'c0ffee0123456789')
  $script:ghState = 'OPEN'
  $threw = $false
  try { $null = Invoke-LandingPrMerge 5 'abc' 6>$null } catch { $threw = $true }
  Check 'a merge that failed and is not merged fails' $threw
  $n = $null
  try { $n = Invoke-LandingPrCreate 'task/120-y' 't' 'b' 6>$null } catch { }
  Check 'a create that failed after a 5xx uses the PR open for the head' ($n -eq 7)

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
