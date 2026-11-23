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

function Enter-LandRunning([string]$Root) {
  # Claims the marker for this process. Returns $null when claimed, else `pid N, started T` of the live owner.
  $f = Get-LandingMarker $Root
  $me = @{ pid = $PID; start = (Get-Process -Id $PID).StartTime.ToString('o') } | ConvertTo-Json -Compress
  for ($try = 0; $try -lt 3; $try++) {
    try {
      $fs = [IO.File]::Open($f, 'CreateNew', 'Write', 'Read')
      $bytes = [Text.Encoding]::UTF8.GetBytes($me); $fs.Write($bytes, 0, $bytes.Length); $fs.Dispose()
      $script:LandMarker = $f
      return $null
    } catch [System.IO.IOException] {
      $m = $null
      try { $m = Get-Content $f -Raw -ErrorAction Stop | ConvertFrom-Json } catch { }
      if ($m -and (Test-LandingMarkerLive $m)) { return "pid $($m.pid), started $(([datetime]$m.start).ToString('yyyy-MM-dd HH:mm:ss'))" }
      Remove-Item $f -Force -ErrorAction SilentlyContinue   # stale: its process is gone
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
      if (Invoke-LandingStep { & git -C $Main worktree remove -f -f $wt } $SleepS) { Write-Host "cleanup: worktree $wt removed" }
      else { Add-Leftover "worktree $wt" "git worktree remove -f -f $wt" }
      $null = & git -C $Main worktree prune 2>&1
    }
    $names = @($branch)
    if ($wt) {
      $agent = "worktree-$(Split-Path -Leaf $wt)"
      & git -C $Main show-ref --verify --quiet "refs/heads/$agent"
      if ($LASTEXITCODE -eq 0) { $names += $agent }
    }
    foreach ($n in $names) {
      if (Invoke-LandingStep { & git -C $Main branch -D $n } $SleepS) { Write-Host "cleanup: local branch $n deleted" }
      else { Add-Leftover "local branch $n" "git branch -D $n" }
    }
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
