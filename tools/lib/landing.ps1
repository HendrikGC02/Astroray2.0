# SPDX-License-Identifier: Apache-2.0
# Landing helpers for tools/land.ps1: busy checks, task lookup, cleanup, timing and history. Dot-source this file.
# Ref: docs/decisions/0009-build-test-process.md d26 (landing); tasks/138 (the retry, deadline and history keys).

function Get-LandingMain([string]$Dir) {
  # The first entry of `git worktree list` is the main checkout; never a literal path.
  foreach ($l in (& git -C $Dir worktree list --porcelain)) { if ($l -like 'worktree *') { return $l.Substring(9) } }
  throw 'git worktree list printed no main checkout'
}

function Find-LandingTask([string]$Main, [string]$Number) {
  # The exact branch task/NNN-* checked out in a worktree, as {Branch; Worktree}; $null when none.
  # Anchored on `refs/heads/task/NNN-` so that task/12-x is never taken for 120.
  $found = @(); $cur = $null
  foreach ($l in (& git -C $Main worktree list --porcelain)) {
    if ($l -like 'worktree *') { $cur = $l.Substring(9) }
    elseif ($l -match "^branch refs/heads/(task/$Number-.+)$") { $found += [pscustomobject]@{ Branch = $Matches[1]; Worktree = $cur } }
  }
  if ($found.Count -gt 1) { throw "more than one worktree holds a branch task/$Number-*: $(($found.Branch) -join ', ')" }
  if ($found.Count -eq 1) { return $found[0] }
  $refs = @(& git -C $Main for-each-ref --format='%(refname:short)' "refs/heads/task/$Number-*")
  if ($refs.Count -gt 1) { throw "more than one branch task/$Number-*: $($refs -join ', ')" }
  if ($refs.Count -eq 1) { return [pscustomobject]@{ Branch = $refs[0]; Worktree = $null } }
  return $null
}

function Get-LandingMarker([string]$Root) {
  # One marker per repository (its common git dir, shared by every worktree): a land.ps1 on another repository,
  # such as a fixture under a temp dir, is not "another land.ps1" for this one.
  $c = (& git -C $Root rev-parse --git-common-dir).Trim()
  if (-not [IO.Path]::IsPathRooted($c)) { $c = Join-Path $Root $c }
  return (Join-Path $c 'astroray-land.json')
}

function Test-LandingMarkerLive($m) {
  if ([int]$m.pid -eq $PID) { return $false }

  $p = Get-Process -Id ([int]$m.pid) -ErrorAction SilentlyContinue
  if (-not $p -or $p.StartTime.ToUniversalTime().Ticks -ne ([datetime]$m.start).ToUniversalTime().Ticks) { return $false }
  $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$([int]$m.pid)"
  return [bool]($cim -and $cim.CommandLine -match 'land\.ps1')
}

function Wait-LandRunning([string]$Root) {
  # `submit -Queue`: takes the marker once the land.ps1 that holds it is gone, polling the same claim a refusal
  # makes. Returns the seconds waited (the caller adds them to lock_wait_s). A wait line names the owner on the
  # first three polls, then at most one per 300 s. ASTRORAY_LAND_QUEUE_POLL_S (default 15) is the poll interval.
  $pollS = 15.0
  if ($env:ASTRORAY_LAND_QUEUE_POLL_S) {
    if (-not [double]::TryParse($env:ASTRORAY_LAND_QUEUE_POLL_S, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$pollS) -or $pollS -le 0) {
      throw "ASTRORAY_LAND_QUEUE_POLL_S='$($env:ASTRORAY_LAND_QUEUE_POLL_S)' is not a positive number of seconds"
    }
  }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $said = 0; $last = 0.0
  while ($true) {
    $run = Enter-LandRunning $Root
    if (-not $run) { return $sw.Elapsed.TotalSeconds }
    if ($said -lt 3 -or $sw.Elapsed.TotalSeconds - $last -ge 300) {
      $said++; $last = $sw.Elapsed.TotalSeconds
      Write-Host "queue: waiting for another land.ps1 ($run), $([math]::Round($sw.Elapsed.TotalSeconds)) s so far"
    }
    Start-Sleep -Milliseconds ([int]($pollS * 1000))
  }
}

function Remove-LandingStaleMarker([string]$File, [string]$StaleText) {
  # Two land.ps1 starting together can both read the same stale marker; a plain delete by the slower one would
  # remove the marker the faster one has just claimed. The rename is atomic, so only one process takes the file,
  # and it is deleted only while it still holds the stale text read before; anything else is put back.
  $grab = "$File.stale.$PID.$([guid]::NewGuid().ToString('N'))"
  try { [IO.File]::Move($File, $grab) } catch { return }
  $now = ''
  try { $now = [IO.File]::ReadAllText($grab) } catch { }
  if ($now -eq $StaleText) { Remove-Item -LiteralPath $grab -Force -ErrorAction SilentlyContinue; return }
  # It may be another process's live marker: never delete it here.
  try { [IO.File]::Move($grab, $File) } catch { Write-Host "WARN land: could not put back the running-land marker; it is left as $grab (delete it once no land.ps1 runs)" }
}

function Enter-LandRunning([string]$Root, [int]$YoungS = 5) {
  # Claims the marker for this process. Returns $null when claimed, else a description of the live owner.
  $f = Get-LandingMarker $Root
  $me = @{ pid = $PID; start = (Get-Process -Id $PID).StartTime.ToString('o') } | ConvertTo-Json -Compress
  for ($try = 0; $try -lt 3; $try++) {
    try {
      $fs = [IO.File]::Open($f, 'CreateNew', 'Write', 'Read')
      $bytes = [Text.Encoding]::UTF8.GetBytes($me); $fs.Write($bytes, 0, $bytes.Length); $fs.Dispose()
      $script:LandMarker = $f
      return $null
    } catch [System.IO.IOException] {
      $text = $null; $m = $null
      try { $text = [IO.File]::ReadAllText($f) } catch { continue }   # gone or locked since: try again
      try { $m = $text | ConvertFrom-Json -ErrorAction Stop } catch { }
      if (-not $m) {
        # Empty or half written: the owner may be between CreateNew and its write. Young means live.
        $fi = Get-Item -LiteralPath $f -ErrorAction SilentlyContinue
        if (-not $fi) { continue }
        $age = ((Get-Date).ToUniversalTime() - $fi.LastWriteTimeUtc).TotalSeconds
        if ($age -lt $YoungS) { return ('pid unknown, marker written {0:N1} s ago' -f $age) }
      } elseif (Test-LandingMarkerLive $m) { return "pid $($m.pid), started $(([datetime]$m.start).ToString('yyyy-MM-dd HH:mm:ss'))" }
      Remove-LandingStaleMarker $f $text   # stale: its process is gone, or an empty marker left long ago
    }
  }
  return 'unknown (the marker file could not be claimed)'
}

function Exit-LandRunning {
  if ($script:LandMarker) { Remove-Item $script:LandMarker -Force -ErrorAction SilentlyContinue; $script:LandMarker = $null }
}

function Find-DllHolder([string]$Root) {
  # Who has this worktree's out/bin/astroray_core.dll mapped (a leftover Blender makes the link step fail with
  # LNK1168). A mapped image cannot be opened for writing, which is the cheap test; only then are modules listed.
  $dll = Join-Path $Root 'out/bin/astroray_core.dll'
  if (-not (Test-Path $dll)) { return $null }
  try { [IO.File]::Open($dll, 'Open', 'ReadWrite', 'None').Dispose(); return $null } catch { }
  $full = (Resolve-Path $dll).Path
  foreach ($p in (Get-Process)) {
    try { $mods = $p.Modules } catch { continue }   # a process that denies access is skipped
    foreach ($m in $mods) { if ($m.FileName -ieq $full) { return "$($p.ProcessName) (pid $($p.Id))" } }
  }
  return $null
}

function Invoke-LandingStep([scriptblock]$Body, [int]$SleepS) {
  # Three tries; true when the native command in $Body exited 0.
  for ($i = 1; $i -le 3; $i++) {
    $global:LASTEXITCODE = 0
    $null = & $Body 2>&1
    if ($LASTEXITCODE -eq 0) { return $true }
    if ($i -lt 3 -and $SleepS -gt 0) { Start-Sleep -Seconds $SleepS }
  }
  return $false
}

function Test-LandingWorktreeRegistered([string]$Main, [string]$Wt) {
  $want = [IO.Path]::GetFullPath($Wt).TrimEnd('\', '/')
  foreach ($l in (& git -C $Main worktree list --porcelain)) {
    if ($l -like 'worktree *' -and [IO.Path]::GetFullPath($l.Substring(9)).TrimEnd('\', '/') -ieq $want) { return $true }
  }
  return $false
}

function Remove-LandingWorktree([string]$Main, [string]$Wt, [int]$SleepS) {
  # 'removed', 'registered' (git still lists it), 'directory' (unregistered, deleting the directory failed) or
  # 'outside' (unregistered, the directory lies outside .claude/worktrees/ and is not deleted here).
  # `git worktree remove` unregisters the tree even when deleting its directory fails (a held file), and a retry
  # then fails with "is not a working tree"; from there only the directory is deleted, and only when it lies under
  # the repository's .claude/worktrees/ (it was found in `git worktree list` for this task by the caller).
  $full = [IO.Path]::GetFullPath($Wt).TrimEnd('\', '/')
  $agents = [IO.Path]::GetFullPath((Join-Path $Main '.claude/worktrees')).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
  $mayDelete = $full.StartsWith($agents, [StringComparison]::OrdinalIgnoreCase)
  for ($i = 1; $i -le 3; $i++) {
    if (Test-LandingWorktreeRegistered $Main $full) { $null = & git -C $Main worktree remove -f -f $full 2>&1 }
    elseif ($mayDelete -and (Test-Path -LiteralPath $full)) { Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue }
    $reg = Test-LandingWorktreeRegistered $Main $full
    if (-not $reg -and -not (Test-Path -LiteralPath $full)) { return 'removed' }
    if (-not $reg -and -not $mayDelete) { return 'outside' }   # nothing left that this tool may do
    if ($i -lt 3 -and $SleepS -gt 0) { Start-Sleep -Seconds $SleepS }
  }
  if ($reg) { return 'registered' } else { return 'directory' }
}

function Invoke-LandingCleanup {
  # After the squash is on origin/main. Never throws and never fails the landing: prints
  # `LEFTOVER: <what>; fix: <command>` for each step that did not work and returns their count.
  param([Parameter(Mandatory)][string]$Main, [Parameter(Mandatory)]$Task, [int]$SleepS = 5)
  Set-Location $Main   # the cwd may be inside the worktree being removed
  $left = [Collections.Generic.List[string]]::new()
  function Add-Leftover([string]$what, [string]$fix) { Write-Host "LEFTOVER: $what; fix: $fix"; $left.Add($what) }
  $branch = $Task.Branch; $wt = $Task.Worktree
  try {
    $remote = @(& git -C $Main ls-remote --heads origin $branch 2>$null)
    if ($LASTEXITCODE -eq 0 -and $remote.Count -gt 0) {
      if (Invoke-LandingStep { & git -C $Main push origin --delete $branch } $SleepS) { Write-Host "cleanup: remote branch $branch deleted" }
      else { Add-Leftover "remote branch $branch" "git push origin --delete $branch" }
    }
    if ($wt) {
      $null = & git -C $Main worktree unlock $wt 2>&1   # fails when not locked; a Claude agent's lock is what matters
      $r = Remove-LandingWorktree $Main $wt $SleepS
      if ($r -eq 'removed') { Write-Host "cleanup: worktree $wt removed" }
      elseif ($r -eq 'registered') { Add-Leftover "worktree $wt" "git worktree remove -f -f '$wt'" }
      elseif ($r -eq 'outside') { Add-Leftover "worktree directory $wt (unregistered; outside .claude/worktrees/, so not deleted here)" "check it, then Remove-Item -LiteralPath '$wt' -Recurse -Force" }
      else { Add-Leftover "worktree directory $wt (unregistered; a process holds a file in it)" "close that process, then Remove-Item -LiteralPath '$wt' -Recurse -Force" }
      $null = & git -C $Main worktree prune 2>&1
    }
    # Only the exact branch found for this task: never a name derived from it.
    if (Invoke-LandingStep { & git -C $Main branch -D $branch } $SleepS) { Write-Host "cleanup: local branch $branch deleted" }
    else { Add-Leftover "local branch $branch" "git branch -D $branch" }
  } catch { Add-Leftover "cleanup stopped: $($_.Exception.Message)" "delete $branch (remote and local) and the worktree by hand" }
  return $left.Count
}

# Phase clocks (seconds, summed over attempts). lock_wait is the sum of Enter-ArMachineLock waits seen in this process.
$script:LandTimes = [ordered]@{ rebase_s = 0.0; scenes_s = 0.0; canary_s = 0.0; blender_suites_s = 0.0; gate_s = 0.0; ci_wait_s = 0.0; merge_s = 0.0; lock_wait_s = 0.0 }
function Measure-LandPhase([string]$Key, [scriptblock]$Body) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  try { & $Body } finally { $script:LandTimes[$Key] += $sw.Elapsed.TotalSeconds }
}

function Get-LandingLastGate([string]$Main) {
  $f = Join-Path $Main 'out/land/history.jsonl'
  if (-not (Test-Path $f)) { return $null }
  $last = @(Get-Content $f | Where-Object { $_.Trim() }) | Select-Object -Last 1
  if (-not $last) { return $null }
  return ($last | ConvertFrom-Json)
}

function Add-LandingHistory([string]$Main, $Record) {
  $dir = Join-Path $Main 'out/land'
  New-Item -ItemType Directory -Force $dir | Out-Null
  [IO.File]::AppendAllText((Join-Path $dir 'history.jsonl'), (($Record | ConvertTo-Json -Compress) + "`n"), [Text.UTF8Encoding]::new($false))
}

function Format-LandingTiming($Times, [int]$Attempts) {
  $parts = foreach ($k in $Times.Keys) { '{0} {1:N1}' -f ($k -replace '_s$', ''), $Times[$k] }
  return "timing: $($parts -join ', ') (s); retries $($Attempts - 1)"
}

# A GitHub 5xx is transient: retry with these delays (seconds) so a passing gate or a half-done landing is never
# lost to it. fast_landing sets them to zero.
$script:GhDelays = @(5, 15, 30)
function GhX {
  # Any other failure, or a 5xx that persists, throws.
  for ($i = 0; ; $i++) {
    $o = & gh @args 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -eq 0) { return ($o -join "`n") }
    $txt = $o -join ' '
    if ($i -lt $script:GhDelays.Count -and $txt -match 'HTTP 5\d\d|Bad Gateway|Service Unavailable|Gateway Time-?out') {
      Write-Host "gh $($args[0]) got a 5xx; retry $($i + 1) of $($script:GhDelays.Count) in $($script:GhDelays[$i]) s"
      Start-Sleep -Seconds $script:GhDelays[$i]
      continue
    }
    throw "gh $($args -join ' ') failed: $txt"
  }
}

function Invoke-LandingPrMerge([int]$Pr, [string]$Sha) {
  # Squash-merges PR $Pr at $Sha; returns the merge commit. A merge call is not idempotent: a 5xx can come back
  # after GitHub merged, and its retry then fails. Before reporting a failure the PR itself is asked.
  try { $null = GhX pr merge $Pr --squash --match-head-commit $Sha }
  catch {
    $err = $_.Exception.Message
    $v = $null
    try { $v = (GhX pr view $Pr --json 'state,mergeCommit') | ConvertFrom-Json } catch { }
    if (-not ($v -and $v.state -eq 'MERGED' -and $v.mergeCommit.oid)) { throw $err }
    Write-Host "gh pr merge reported a failure, but PR #$Pr is merged as $($v.mergeCommit.oid.Substring(0, 9))"
    return $v.mergeCommit.oid
  }
  return ((GhX pr view $Pr --json mergeCommit) | ConvertFrom-Json).mergeCommit.oid
}

function Invoke-LandingPrCreate([string]$Branch, [string]$Title, [string]$Body) {
  # Opens the PR for $Branch; returns its number. As with the merge, a create that failed after a 5xx may have
  # opened the PR: the open PRs for the head are listed again before the failure is reported.
  try {
    $url = GhX pr create --base main --head $Branch --title $Title --body $Body
    $n = [regex]::Match($url, '(\d+)\s*$').Groups[1].Value
    if ($n) { return [int]$n }
    $err = "gh pr create printed no PR number: $url"
  } catch { $err = $_.Exception.Message }
  $pr = @()
  try { $pr = @((GhX pr list --head $Branch --state open --json number) | ConvertFrom-Json) } catch { }
  if ($pr.Count -eq 1) { Write-Host "gh pr create reported a failure, but PR #$($pr[0].number) is open for $Branch"; return [int]$pr[0].number }
  throw $err
}
