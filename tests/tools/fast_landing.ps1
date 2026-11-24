# SPDX-License-Identifier: Apache-2.0
# CPU-only checks of tools/lib/landing.ps1 on a real throwaway repo: cleanup (a worktree whose out/ holds an open
# file ends in a LEFTOVER line whose fix works once the file is closed; an unregistered leftover directory is
# deleted under .claude/worktrees/ and only reported outside it; a clean worktree is removed), exact branch choice, the running-land marker, the merge and PR-create
# calls after a 5xx (a `gh` stub), the history line in the main checkout, and `submit -Queue` (a second land.ps1
# waits for the running one, a plain second one is refused, and the wait is counted in lock_wait_s; started first
# and judged last so that its time overlaps the other checks), `merge -Queue` (refused), and Wait-LandRunning itself
# (it takes the marker when it returns, and fails past ASTRORAY_LAND_QUEUE_TIMEOUT_S). Run by tests/tools/run_fast.ps1
# (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/lib/landing.ps1')
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) { if (-not $ok) { Write-Host "FAIL fast_landing: $what"; @($out) | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }; $script:bad++ } }
function G { & git -C $main @args 2>&1 | Out-Null; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }
function Find-Safe([string]$n) { try { return Find-LandingTask $main $n } catch { Write-Host "fast_landing: Find-LandingTask $n threw: $($_.Exception.Message)"; return $null } }

$tmp = Join-Path ([IO.Path]::GetTempPath()) "astroray_fast_landing_$PID"
$main = Join-Path $tmp 'main'
$wts = Join-Path $main '.claude/worktrees'   # where Claude agent worktrees live; cleanup deletes directories only there
$stream = $null
$qprocs = @()
try {
  New-Item -ItemType Directory -Force $tmp | Out-Null
  # The queue case. A stand-in script named land.ps1 is the running land.ps1 of a throwaway repository (the real
  # tools/land.ps1 takes a live process whose command line holds land.ps1 for the owner of the marker), and the
  # marker names it at once. A plain submit is then refused and a -Queue submit waits; the stand-in leaves 0.25 s
  # after the queued one says it waits, and the queued one runs on, past the idle check, to the first refusal of
  # an empty repository (not a task branch).
  $qrepo = Join-Path $tmp 'queue_repo'
  & git init -q -b main $qrepo
  New-Item -ItemType Directory -Force (Join-Path $qrepo 'tasks') | Out-Null
  $mkFile = Get-LandingMarker $qrepo
  $qout = Join-Path $tmp 'queue_out'
  New-Item -ItemType Directory -Force $qout | Out-Null
  $standin = Join-Path $tmp 'standin/land.ps1'
  New-Item -ItemType Directory -Force (Split-Path $standin) | Out-Null
  Set-Content $standin @"
param([string]`$marker, [string]`$queueOut)
`$sw = [Diagnostics.Stopwatch]::StartNew()
while (-not ((Test-Path `$queueOut) -and (Select-String -Path `$queueOut -Pattern '^queue: waiting for another land\.ps1' -Quiet)) -and `$sw.Elapsed.TotalSeconds -lt 30) { Start-Sleep -Milliseconds 20 }
Start-Sleep -Milliseconds 250
Remove-Item -Force `$marker
"@
  $env:ASTRORAY_LAND_QUEUE_POLL_S = '0.1'
  $holderP = Start-Process pwsh -ArgumentList @('-NoProfile', '-File', $standin, $mkFile, (Join-Path $qout 'queue.out')) -PassThru -NoNewWindow
  $null = $holderP.Handle; $qprocs += $holderP
  Set-Content $mkFile (@{ pid = $holderP.Id; start = $holderP.StartTime.ToString('o') } | ConvertTo-Json -Compress)
  # Stand-ins for Wait-LandRunning called here: each holds the marker of its own repository until a release file
  # appears, then removes the marker as a finishing land.ps1 does.
  $holdScript = Join-Path $tmp 'standin_hold/land.ps1'
  New-Item -ItemType Directory -Force (Split-Path $holdScript) | Out-Null
  Set-Content $holdScript @"
param([string]`$marker, [string]`$release)
`$sw = [Diagnostics.Stopwatch]::StartNew()
while (-not (Test-Path `$release) -and `$sw.Elapsed.TotalSeconds -lt 60) { Start-Sleep -Milliseconds 20 }
Remove-Item -Force `$marker
"@
  $waitRepos = @{}
  foreach ($k in 'claim', 'timeout') {
    $r = Join-Path $tmp "wait_$k"
    & git init -q -b main $r
    $mk = Get-LandingMarker $r
    $rel = Join-Path $tmp "wait_$k.release"
    $hp = Start-Process pwsh -ArgumentList @('-NoProfile', '-File', $holdScript, $mk, $rel) -PassThru -NoNewWindow
    $null = $hp.Handle; $qprocs += $hp
    Set-Content $mk (@{ pid = $hp.Id; start = $hp.StartTime.ToString('o') } | ConvertTo-Json -Compress)
    $waitRepos[$k] = @{ Repo = $r; Marker = $mk; Release = $rel }
  }
  $landArgs = @('-NoProfile', '-File', (Join-Path $root 'tools/land.ps1'), 'submit', '999', 'PASS', '-DryRun')
  $plainP = Start-Process pwsh -ArgumentList $landArgs -WorkingDirectory $qrepo -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $qout 'plain.out') -RedirectStandardError (Join-Path $qout 'plain.err')
  $queueP = Start-Process pwsh -ArgumentList ($landArgs + '-Queue') -WorkingDirectory $qrepo -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $qout 'queue.out') -RedirectStandardError (Join-Path $qout 'queue.err')
  # merge takes no marker: the switch must be refused, not ignored.
  $mergeP = Start-Process pwsh -ArgumentList @('-NoProfile', '-File', (Join-Path $root 'tools/land.ps1'), 'merge', '999', '-Queue', '-DryRun') -WorkingDirectory $qrepo -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $qout 'merge.out') -RedirectStandardError (Join-Path $qout 'merge.err')
  $null = $plainP.Handle; $null = $queueP.Handle; $null = $mergeP.Handle; $qprocs += $plainP; $qprocs += $queueP; $qprocs += $mergeP
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

  # Wait-LandRunning, called here. Past ASTRORAY_LAND_QUEUE_TIMEOUT_S it fails loudly, naming the limit.
  $w = $waitRepos['timeout']
  $env:ASTRORAY_LAND_QUEUE_TIMEOUT_S = '0.5'
  $msg = ''
  try { $null = Wait-LandRunning $w.Repo 6>$null } catch { $msg = $_.Exception.Message }
  $env:ASTRORAY_LAND_QUEUE_TIMEOUT_S = $null
  Check 'a queue wait past ASTRORAY_LAND_QUEUE_TIMEOUT_S fails, naming it' ($msg -match 'over the 0\.5 s limit \(ASTRORAY_LAND_QUEUE_TIMEOUT_S\)') @($msg)
  Check 'a queue wait that timed out did not take the marker' ((Get-Content $w.Marker -Raw | ConvertFrom-Json).pid -ne $PID)
  Set-Content $w.Release release
  # It returns only once the owner is gone, and then the marker is this process's: another queued land.ps1 must
  # not start beside it. The owner is released 0.4 s after the wait began.
  $w = $waitRepos['claim']
  $rj = Start-ThreadJob -ScriptBlock { param($f) Start-Sleep -Milliseconds 400; Set-Content $f release } -ArgumentList $w.Release
  $waited = Wait-LandRunning $w.Repo 6>$null
  $null = Wait-Job $rj -Timeout 10; Remove-Job $rj -Force
  Check 'Wait-LandRunning returns once the owner has gone, after waiting for it' ($waited -ge 0.3 -and $waited -lt 10) @("waited $waited")
  $mo = $null
  try { $mo = Get-Content $w.Marker -Raw -ErrorAction Stop | ConvertFrom-Json } catch { }
  Check 'Wait-LandRunning owns the marker when it returns (this pid)' ($mo -and [int]$mo.pid -eq $PID) @("marker: $mo")
  Exit-LandRunning

  # Judged last: what the plain and the queued submit printed.
  $null = $plainP.WaitForExit(30000); $null = $queueP.WaitForExit(30000); $null = $mergeP.WaitForExit(30000)
  $po = @(Get-Content (Join-Path $qout 'plain.out') -ErrorAction SilentlyContinue)
  $qo = @(Get-Content (Join-Path $qout 'queue.out') -ErrorAction SilentlyContinue)
  $mo2 = @(Get-Content (Join-Path $qout 'merge.out') -ErrorAction SilentlyContinue)
  Check 'merge -Queue is refused' (@($mo2 | Where-Object { $_ -match '^REFUSE land: -Queue applies to prepare and submit' }).Count -eq 1 -and $mergeP.ExitCode -ne 0) $mo2
  Check 'a submit without -Queue is refused while another land.ps1 runs' (@($po | Where-Object { $_ -match '^REFUSE land: another land\.ps1 is running \(pid \d+' }).Count -eq 1) $po
  Check 'the queued submit says it waits, naming the owner' (@($qo | Where-Object { $_ -match '^queue: waiting for another land\.ps1 \(pid \d+' }).Count -ge 1) $qo
  # The stand-in leaves 0.25 s after the queued submit first said it waits, so the wait is at least that, and the
  # running total of lock_wait_s (nothing else waited in this run) is the wait itself.
  $qm = @($qo | ForEach-Object { [regex]::Match($_, '^queue: waited ([\d.]+) s for another land\.ps1 \(lock_wait_s now ([\d.]+) s\)') } | Where-Object { $_.Success })
  Check 'the queued submit reports its wait once' ($qm.Count -eq 1) $qo
  if ($qm.Count -eq 1) {
    $qw = [double]::Parse($qm[0].Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
    $qt = [double]::Parse($qm[0].Groups[2].Value, [Globalization.CultureInfo]::InvariantCulture)
    Check 'the queue wait is at least the stand-in''s remaining hold' ($qw -ge 0.2) $qo
    Check 'lock_wait_s includes the queue wait' ([math]::Abs($qt - $qw) -le 0.11 -and $qt -ge 0.2) $qo
  }
  Check 'the queued submit then runs past the idle check (first refusal: not a task branch)' (@($qo | Where-Object { $_ -match "^REFUSE land: branch 'main' is not task/999-" }).Count -eq 1) $qo
  Check 'the marker is released after both' (-not (Test-Path $mkFile))
} finally {
  Set-Location $root
  $env:ASTRORAY_LAND_QUEUE_POLL_S = $null
  foreach ($p in $qprocs) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
  if ($stream) { $stream.Dispose() }
  & git -C $main worktree prune 2>$null
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { exit 1 }
Write-Host 'PASS fast_landing'
