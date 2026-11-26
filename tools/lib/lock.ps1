# SPDX-License-Identifier: Apache-2.0
# Machine mutex and per-tree lock (0009 d8). Dot-source this file.
# The outermost holder sets ASTRORAY_LOCK_PID so nested tool calls do not re-lock.
# Each holder records itself in an owner file so a waiter can name it. The machine owner file lives in a
# machine-wide directory (every worktree and agent shell must see it; TEMP and the profile are per user or
# sandbox); the tree lock's sits beside its lock file. Environment, read at each call so tests never touch
# the real lock: ASTRORAY_LOCK_NAME (mutex), ASTRORAY_LOCK_DIR (owner file), ASTRORAY_LOCK_MESSAGE_S
# (seconds between wait ticks; the first three print), ASTRORAY_LOCK_TIMEOUT_S (longest a holder may keep
# the lock while someone waits; set, it replaces the default limit below), ASTRORAY_LOCK_CEILING_S (the
# holder's own hold ceiling; set, it replaces the table below). Hold ceilings and the short-job rule: 0009 d8.
$script:ArMutex = $null
$script:ArOwnsLockPid = $false
$script:ArOwnerPath = $null
$script:ArHold = $null
$script:ArHeldAt = ''

# Longest a tool may hold the machine lock before its own watchdog stops it (seconds). Each row is 2 x the
# longest hold measured in out/land/history.jsonl (canary_s 1496.6, blender_suites_s 628.1, gate_s 375.3,
# perf_s 375.7) and out/check/*.json (gate 184 s, full 637 s net), rounded up to a whole minute; rows with no
# recorded long hold (build, test, bench, gate_runs) are a stated estimate. `other` keeps the old 3600 s.
$script:ArLockCeilings = @{
  'build' = 600; 'test' = 900; 'check-gate' = 780; 'check-full' = 1440; 'canary' = 3000; 'blender' = 1140
  'perf' = 780; 'bench' = 1800; 'gate_runs' = 1200; 'other' = 3600
}
$script:ArLockJobByScript = @{
  'build.ps1' = 'build'; 'test.ps1' = 'test'; 'canary.ps1' = 'canary'; 'blender.ps1' = 'blender'
  'perf.ps1' = 'perf'; 'bench.ps1' = 'bench'; 'gate_runs.ps1' = 'gate_runs'
}
# A long tool about to take the free mutex waits while a short job's registration is younger than this.
$script:ArShortYieldS = 300
# A waiter fails at the live holder's ceiling plus this.
$script:ArWaiterGraceS = 60

function Get-ArLockSetting([string]$name, [double]$default) {
  $v = [Environment]::GetEnvironmentVariable($name)
  if (-not $v) { return $default }
  $d = 0.0
  if (-not [double]::TryParse($v, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$d) -or $d -le 0) {
    throw "FAIL lock: $name='$v' is not a positive number of seconds"
  }
  return $d
}

function Get-ArLockDir {
  if ($env:ASTRORAY_LOCK_DIR) { return $env:ASTRORAY_LOCK_DIR }
  return (Join-Path $env:ProgramData 'astroray2')
}

# The table row for the innermost known script on the call stack, for a caller that names no job.
function Get-ArLockJob {
  foreach ($f in @(Get-PSCallStack)) {
    if ($f.ScriptName) {
      $leaf = Split-Path -Leaf $f.ScriptName
      if ($script:ArLockJobByScript.ContainsKey($leaf)) { return $script:ArLockJobByScript[$leaf] }
    }
  }
  return 'other'
}

# Short jobs waiting for the machine register a file here; a long tool yields to a live, young one.
function Get-ArShortDir { return (Join-Path (Get-ArLockDir) 'shortwait') }

function Get-ArShortWaiting {
  $dir = Get-ArShortDir
  if (-not (Test-Path -LiteralPath $dir)) { return 0 }
  $n = 0
  foreach ($f in [IO.Directory]::GetFiles($dir, 'short.*.wait')) {
    $age = ([DateTime]::UtcNow - [IO.File]::GetLastWriteTimeUtc($f)).TotalSeconds
    $id = [int](([IO.Path]::GetFileName($f) -replace '^short\.', '') -replace '\.wait$', '')
    if ($age -lt $script:ArShortYieldS -and (Get-Process -Id $id -ErrorAction SilentlyContinue)) { $n++ }
  }
  return $n
}

function Register-ArShortWait {
  try {
    New-Item -ItemType Directory -Force (Get-ArShortDir) -ErrorAction Stop | Out-Null
    $f = Join-Path (Get-ArShortDir) "short.$PID.wait"
    [IO.File]::WriteAllText($f, 'waiting')
    return $f
  } catch { Write-Host "WARN lock: short-job registration not written: $($_.Exception.Message)"; return $null }
}

function Unregister-ArShortWait($f) { if ($f) { Remove-Item -Force -LiteralPath $f -ErrorAction SilentlyContinue } }

# The tool a person would recognise: the outermost script outside tools/lib, else the process name.
function Get-ArLockTool {
  $frames = @(Get-PSCallStack | Where-Object { $_.ScriptName -and (Split-Path -Parent $_.ScriptName) -ne $PSScriptRoot })
  if ($frames.Count) { return (Split-Path -Leaf $frames[-1].ScriptName) }
  return (Get-Process -Id $PID).ProcessName
}

# Written to a temporary file and moved, so a reader never sees half a file. Never fatal: the lock works
# without it, only the waiting message loses its name.
function Write-ArLockOwner([string]$path, [hashtable]$Extra = @{}, [string]$Started = '') {
  try {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) -ErrorAction Stop | Out-Null
    $cmd = [Environment]::CommandLine
    if ($cmd.Length -gt 120) { $cmd = $cmd.Substring($cmd.Length - 120) }
    $o = [ordered]@{ pid = $PID; tool = (Get-ArLockTool); cmd = $cmd; started = $(if ($Started) { $Started } else { [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH:mm:sszzz') }) }
    foreach ($k in $Extra.Keys) { $o[$k] = $Extra[$k] }
    $tmp = "$path.$PID.tmp"
    [IO.File]::WriteAllText($tmp, ($o | ConvertTo-Json -Compress))
    [IO.File]::Move($tmp, $path, $true)
    return $true
  } catch {
    Write-Host "WARN lock: owner file not written: $($_.Exception.Message)"
    return $false
  }
}

# Only our own record: a missing or unreadable file, or another pid's, is not ours to delete. A record that is
# ours but stays behind would name this process to the next waiter, so that failure is shown.
function Remove-ArLockOwner([string]$path) {
  if (-not $path) { return }
  try { $o = Get-Content -Raw $path -ErrorAction Stop | ConvertFrom-Json; $ours = ([int]$o.pid -eq $PID) } catch { return }
  if (-not $ours) { return }
  try { Remove-Item -Force $path -ErrorAction Stop } catch { Write-Host "WARN lock: owner file not removed: $($_.Exception.Message)" }
}

# The recorded holder, or $null. Alive: its pid runs and that process started before the record (a reused
# pid started later).
function Read-ArLockOwner([string]$path) {
  # A malformed record reads as no live owner: the wait loop must go on, never throw while it may hold the
  # mutex.
  try {
    $o = Get-Content -Raw $path -ErrorAction Stop | ConvertFrom-Json
    if (-not $o -or -not $o.pid -or -not $o.started) { return $null }
    # ConvertFrom-Json turns an ISO string into a DateTime already.
    $started = if ($o.started -is [DateTime]) { [DateTimeOffset]$o.started } else { [DateTimeOffset]::Parse("$($o.started)", [Globalization.CultureInfo]::InvariantCulture) }
    $opid = [int]$o.pid
  } catch { return $null }
  $p = Get-Process -Id $opid -ErrorAction SilentlyContinue
  $alive = [bool]$p
  if ($p) { try { $alive = ([DateTimeOffset]$p.StartTime) -le $started.AddSeconds(1) } catch { } }
  $text = $started.ToLocalTime().ToString('yyyy-MM-ddTHH:mm:sszzz')
  return [pscustomobject]@{ Pid = $opid; Tool = "$($o.tool)"; Started = $started; Text = $text; Alive = $alive; Step = "$($o.step)"; CeilingS = $(if ($o.ceiling_s) { [double]$o.ceiling_s } else { 0.0 }) }
}

function Format-ArLockWait($owner) {
  if ($owner -and $owner.Alive) {
    $t = "waiting for $($owner.Tool) (pid $($owner.Pid)) since $($owner.Text)"
    if ($owner.Step) { $t += ", step '$($owner.Step)'" }
    $held = ([DateTimeOffset]::Now - $owner.Started).TotalSeconds
    if ($owner.CeilingS -gt 0 -and $held -gt $owner.CeilingS) { $t += ", held $([math]::Round($held)) s, PAST its $($owner.CeilingS) s ceiling: hung" }
    return $t
  }
  return 'waiting for unknown holder (owner.json missing or its pid not running)'
}

# Whether a wait tick prints: the first three ticks name the holder, then one line per 300 s keeps a long,
# legitimate queue from filling the console.
function Test-ArLockSay($state, [double]$elapsedS) {
  if ($state.Said -lt 3 -or $elapsedS - $state.Last -ge 300) { $state.Said++; $state.Last = $elapsedS; return $true }
  return $false
}

# The thread that owns the machine mutex: a .NET Mutex is thread-affine, so the thread that takes it is the only
# one that may release it. It lives in its own runspace (one dedicated thread for the whole script block), takes
# the mutex, holds it until Exit-ArMachineLock signals Release, and past the ceiling releases it itself after
# stopping the holder's own child processes. Ref: System.Threading.Mutex (ReleaseMutex from the owning thread).
$script:ArHolderScript = {
  param($s)
  function Say([string]$t) { [Console]::Out.WriteLine($t); [Console]::Out.Flush() }
  $m = New-Object System.Threading.Mutex($false, $s.Name)
  $taken = $false
  while (-not $s.Cancel -and -not $taken) {
    if (-not $s.Try) { Start-Sleep -Milliseconds 25; continue }
    # A short job blocks in the wait, so it is first in line at a release; a long tool polls and so never
    # queues ahead of a short job that registered after it.
    $slice = if ($s.Short) { 100 } else { 0 }
    try {
      if ($m.WaitOne($slice)) { $taken = $true; $s.State = 'held' }
      elseif (-not $s.Short) { Start-Sleep -Milliseconds 25 }
    } catch [System.Threading.AbandonedMutexException] { $taken = $true; $s.State = 'abandoned' }
  }
  if (-not $taken) { $m.Dispose(); $s.State = 'cancelled'; return }
  $s.Acquired.Set() | Out-Null
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not $s.Release -and $sw.Elapsed.TotalSeconds -lt $s.CeilingS) { Start-Sleep -Milliseconds 25 }
  if (-not $s.Release) {
    $s.Expired = $true
    $step = if ($s.Step) { $s.Step } else { 'unknown' }
    Say "FAIL lock: held $([math]::Round($sw.Elapsed.TotalSeconds)) s, over the $($s.Job) ceiling of $($s.CeilingS) s (step: $step; pid $($s.Pid))"
    # Only this process's descendants, newer than this process (a reused parent pid is not ours).
    try {
      $all = @(Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId, CreationDate)
      $mine = [DateTime]::new($s.StartTicks)
      $todo = [Collections.Generic.Queue[int]]::new(); $todo.Enqueue($s.Pid)
      $kill = @()
      while ($todo.Count) {
        $id = $todo.Dequeue()
        foreach ($c in @($all | Where-Object { $_.ParentProcessId -eq $id -and $_.ProcessId -ne $s.Pid -and $_.CreationDate -ge $mine })) { $kill += $c.ProcessId; $todo.Enqueue([int]$c.ProcessId) }
      }
      [array]::Reverse($kill)
      foreach ($id in $kill) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
      Say "lock: stopped $($kill.Count) child process(es) of pid $($s.Pid)"
    } catch { Say "WARN lock: could not stop the holder's children: $($_.Exception.Message)" }
    # Only our own record, as Remove-ArLockOwner.
    try {
      if ($s.OwnerPath -and ([int](Get-Content -Raw $s.OwnerPath | ConvertFrom-Json).pid) -eq $s.Pid) { Remove-Item -Force $s.OwnerPath }
    } catch { }
    try { $m.ReleaseMutex() } catch { }
    $m.Dispose()
    [Environment]::Exit(1)
  }
  try { $m.ReleaseMutex() } catch { }
  $m.Dispose()
}

function Enter-ArMachineLock {
  # Returns seconds waited; 0 when nested under a live lock holder.
  # -Job names the hold-ceiling row (else the innermost known script on the stack); -CeilingS replaces the row;
  # -Short registers this caller as a short job (build, one test case) that long tools yield to.
  param([string]$Job = '', [double]$CeilingS = 0, [switch]$Short)
  $held = $env:ASTRORAY_LOCK_PID
  if ($held -and (Get-Process -Id ([int]$held) -ErrorAction SilentlyContinue)) { return 0.0 }
  $name = if ($env:ASTRORAY_LOCK_NAME) { $env:ASTRORAY_LOCK_NAME } else { 'Global\astroray2' }
  $msgS = Get-ArLockSetting 'ASTRORAY_LOCK_MESSAGE_S' 30
  $limitEnv = [bool][Environment]::GetEnvironmentVariable('ASTRORAY_LOCK_TIMEOUT_S')
  $limitS = Get-ArLockSetting 'ASTRORAY_LOCK_TIMEOUT_S' 3600
  if (-not $Job) { $Job = Get-ArLockJob }
  if (-not $script:ArLockCeilings.ContainsKey($Job)) { throw "FAIL lock: no hold ceiling for job '$Job' (tools/lib/lock.ps1 table)" }
  $ceil = Get-ArLockSetting 'ASTRORAY_LOCK_CEILING_S' $(if ($CeilingS -gt 0) { $CeilingS } else { [double]$script:ArLockCeilings[$Job] })
  $ownerPath = Join-Path (Get-ArLockDir) 'owner.json'
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $s = [hashtable]::Synchronized(@{
      Name = $name; Try = $false; Cancel = $false; Release = $false; Short = [bool]$Short; State = 'waiting'
      Expired = $false; Step = ''; CeilingS = $ceil; Job = $Job; Pid = $PID; OwnerPath = $ownerPath
      StartTicks = (Get-Process -Id $PID).StartTime.Ticks; Acquired = (New-Object System.Threading.ManualResetEvent($false)) })
  $rs = [runspacefactory]::CreateRunspace()
  $rs.Open()
  $ps = [powershell]::Create()
  $ps.Runspace = $rs
  $null = $ps.AddScript($script:ArHolderScript.ToString()).AddArgument($s)
  $handle = $ps.BeginInvoke()
  $reg = if ($Short) { Register-ArShortWait } else { $null }
  $say = @{ Said = 0; Last = 0.0 }
  $nextTick = $msgS
  try {
    while ($true) {
      # A long tool yields while a short job waits; a short job never does.
      $yield = (-not $Short) -and (Get-ArShortWaiting) -gt 0
      $s.Try = -not $yield
      if ($s.Acquired.WaitOne(100)) { break }
      if ($sw.Elapsed.TotalSeconds -lt $nextTick) { continue }
      $nextTick += $msgS
      $o = Read-ArLockOwner $ownerPath
      if (Test-ArLockSay $say $sw.Elapsed.TotalSeconds) {
        Write-Host $(if ($yield) { 'waiting for a short job (build or one test case) that is queued ahead of long tools' } else { Format-ArLockWait $o })
      }
      # The limit is on the live holder's hold, not this caller's wait: a long queue of legitimate holders is not
      # a hang. A live holder's own ceiling plus a minute is the limit unless ASTRORAY_LOCK_TIMEOUT_S is set;
      # without a live record this caller's own wait is the clock.
      $live = $o -and $o.Alive
      $holdS = if ($live) { ([DateTimeOffset]::Now - $o.Started).TotalSeconds } else { $sw.Elapsed.TotalSeconds }
      $lim = if (-not $limitEnv -and $live -and $o.CeilingS -gt 0) { $o.CeilingS + $script:ArWaiterGraceS } else { $limitS }
      $why = if (-not $limitEnv -and $live -and $o.CeilingS -gt 0) { "the holder's $($o.CeilingS) s ceiling + $script:ArWaiterGraceS s" } else { 'ASTRORAY_LOCK_TIMEOUT_S' }
      if ($holdS -ge $lim) {
        $who = if ($live) { "$($o.Tool) (pid $($o.Pid)), holding $([math]::Round($holdS)) s since $($o.Text)$(if ($o.Step) { ", step '$($o.Step)'" })" } else { 'unknown holder' }
        throw "FAIL lock: waited $([math]::Round($sw.Elapsed.TotalSeconds)) s for $who, over the $lim s limit ($why); owner file $ownerPath"
      }
    }
  } catch {
    $s.Cancel = $true
    $null = $handle.AsyncWaitHandle.WaitOne(2000)
    $ps.Dispose(); $rs.Dispose()
    Unregister-ArShortWait $reg
    throw
  }
  Unregister-ArShortWait $reg
  if ($s.State -eq 'abandoned') {
    # This process now holds the mutex; the record, if any, is the dead holder's.
    $o = Read-ArLockOwner $ownerPath
    $who = if ($o) { "pid $($o.Pid) ($($o.Tool), started $($o.Text))" } else { 'pid unknown (no owner.json)' }
    Write-Host "WARN lock: abandoned by ${who}: the holder died without releasing"
  }
  $script:ArHold = @{ Sync = $s; Ps = $ps; Rs = $rs; Handle = $handle; Job = $Job }
  $script:ArMutex = $true
  $env:ASTRORAY_LOCK_PID = [string]$PID
  $script:ArOwnsLockPid = $true
  $script:ArHeldAt = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH:mm:sszzz')
  if (Write-ArLockOwner $ownerPath @{ ceiling_s = $ceil; job = $Job } $script:ArHeldAt) { $script:ArOwnerPath = $ownerPath }
  return $sw.Elapsed.TotalSeconds
}

# Names the running step: the FAIL line of a watchdog stop and a waiter's message carry it. A no-op when this
# process holds no machine lock.
function Set-ArLockStep([string]$Step) {
  if (-not $script:ArHold) { return }
  $script:ArHold.Sync.Step = $Step
  if ($script:ArOwnerPath) {
    $null = Write-ArLockOwner $script:ArOwnerPath @{ ceiling_s = $script:ArHold.Sync.CeilingS; job = $script:ArHold.Job; step = $Step } $script:ArHeldAt
  }
}

function Exit-ArMachineLock {
  if ($script:ArMutex) {
    # Before the release: afterwards the file may already be the next holder's.
    Remove-ArLockOwner $script:ArOwnerPath
    $script:ArOwnerPath = $null
    # The owning thread releases; wait for it so the mutex is free when this returns.
    $h = $script:ArHold
    $h.Sync.Release = $true
    $null = $h.Handle.AsyncWaitHandle.WaitOne(5000)
    $h.Ps.Dispose(); $h.Rs.Dispose()
    $script:ArHold = $null
    $script:ArMutex = $null
  }
  if ($script:ArOwnsLockPid) {
    $env:ASTRORAY_LOCK_PID = $null
    $script:ArOwnsLockPid = $false
  }
}

$script:ArTreeStream = $null
$script:ArTreeOwnerPath = $null

function Enter-ArTreeLock {
  param([Parameter(Mandatory)][string]$Dir)
  New-Item -ItemType Directory -Force $Dir | Out-Null
  $path = Join-Path $Dir '.tree.lock'
  $ownerPath = Join-Path $Dir '.tree.owner.json'
  $msgS = Get-ArLockSetting 'ASTRORAY_LOCK_MESSAGE_S' 30
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $next = $msgS
  $say = @{ Said = 0; Last = 0.0 }
  while ($true) {
    try {
      $script:ArTreeStream = [IO.File]::Open($path, 'OpenOrCreate', 'ReadWrite', 'None')
      break
    } catch [System.IO.IOException] {
      Start-Sleep -Milliseconds 200
      if ($sw.Elapsed.TotalSeconds -ge $next) {
        $next += $msgS
        if (Test-ArLockSay $say $sw.Elapsed.TotalSeconds) { Write-Host (Format-ArLockWait (Read-ArLockOwner $ownerPath)) }
      }
    }
  }
  if (Write-ArLockOwner $ownerPath) { $script:ArTreeOwnerPath = $ownerPath }
  return $sw.Elapsed.TotalSeconds
}

function Exit-ArTreeLock {
  if ($script:ArTreeStream) {
    Remove-ArLockOwner $script:ArTreeOwnerPath
    $script:ArTreeOwnerPath = $null
    $script:ArTreeStream.Dispose()
    $script:ArTreeStream = $null
  }
}

# Fast window (0009 d8): the gate's `fast` step is timed against B-08 while other worktrees compile without the
# machine lock (canary, test.ps1 and build.ps1 -NoMachineLock use every core; a 6 s test run took 21 s).
# Builds hold a share for their duration; the gate holds the window exclusively for its fast step. The window
# is a mutex ("<machine mutex>.fastwin") that a build passes through only to register: it creates its share
# file while holding it, so a build starting during the window waits, and the gate waits for the registered
# shares to end. A share is a file opened with FileShare.None and DeleteOnClose under <lock dir>/fastwin: a
# holder that dies releases it with its handle, so nothing is left to expire. ASTRORAY_FASTWIN_PID marks the
# process tree that owns the window; its own builds (the fast tier's) do not wait for it.
$script:ArShare = $null
$script:ArWinMutex = $null
$script:ArWinOwnsPid = $false

function Test-ArFastWinNested {
  $held = $env:ASTRORAY_FASTWIN_PID
  return [bool]($held -and (Get-Process -Id ([int]$held) -ErrorAction SilentlyContinue))
}

# Takes the window mutex and returns it, owned by this thread. Waits are reported like the machine lock's.
function Wait-ArFastWinMutex($sw, $say, [string]$what) {
  $msgS = Get-ArLockSetting 'ASTRORAY_LOCK_MESSAGE_S' 30
  $limitS = Get-ArLockSetting 'ASTRORAY_LOCK_TIMEOUT_S' 3600
  $name = $(if ($env:ASTRORAY_LOCK_NAME) { $env:ASTRORAY_LOCK_NAME } else { 'Global\astroray2' }) + '.fastwin'
  $m = New-Object System.Threading.Mutex($false, $name)
  while ($true) {
    try { if ($m.WaitOne([TimeSpan]::FromSeconds($msgS))) { return $m } }
    catch [System.Threading.AbandonedMutexException] {
      # This thread now owns it; the gate that held it died, so its window is gone with it.
      Write-Host "WARN lock: the fast window was abandoned by a gate that died without releasing"
      return $m
    }
    if (Test-ArLockSay $say $sw.Elapsed.TotalSeconds) { Write-Host "waiting for the gate's fast window ($what)" }
    if ($sw.Elapsed.TotalSeconds -ge $limitS) {
      $m.Dispose()
      throw "FAIL lock: waited $([math]::Round($sw.Elapsed.TotalSeconds)) s for the gate's fast window, over the $limitS s limit (ASTRORAY_LOCK_TIMEOUT_S)"
    }
  }
}

function Enter-ArBuildShare {
  # Returns seconds waited; 0 when nested under the window's owner.
  if (Test-ArFastWinNested) { return 0.0 }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $dir = Join-Path (Get-ArLockDir) 'fastwin'
  New-Item -ItemType Directory -Force $dir | Out-Null
  $m = Wait-ArFastWinMutex $sw @{ Said = 0; Last = 0.0 } 'a build waits for it'
  try {
    $script:ArShare = New-Object IO.FileStream((Join-Path $dir "share.$PID.lock"), 'OpenOrCreate', 'ReadWrite', 'None', 4096, [IO.FileOptions]::DeleteOnClose)
  } finally { $m.ReleaseMutex(); $m.Dispose() }
  return $sw.Elapsed.TotalSeconds
}

function Exit-ArBuildShare {
  if ($script:ArShare) { $script:ArShare.Dispose(); $script:ArShare = $null }
}

function Enter-ArFastWindow {
  # Returns seconds waited (for the mutex and for the registered builds to end).
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $say = @{ Said = 0; Last = 0.0 }
  $dir = Join-Path (Get-ArLockDir) 'fastwin'
  New-Item -ItemType Directory -Force $dir | Out-Null
  $m = Wait-ArFastWinMutex $sw $say 'another gate holds it'
  $msgS = Get-ArLockSetting 'ASTRORAY_LOCK_MESSAGE_S' 30
  $limitS = Get-ArLockSetting 'ASTRORAY_LOCK_TIMEOUT_S' 3600
  $next = $msgS
  $warned = [Collections.Generic.HashSet[string]]::new()
  while ($true) {
    $live = 0
    # A listing error must not read as "no builds": it stops the gate.
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -Filter 'share.*.lock' -ErrorAction Stop)) {
      # Read access with no sharing conflicts with a build's open share (a sharing violation) and writes
      # nothing, so the probe cannot disturb the file it tests.
      try { [IO.File]::Open($f.FullName, 'Open', 'Read', 'None').Dispose() }
      catch [System.IO.FileNotFoundException] { }
      catch [System.UnauthorizedAccessException] {
        # A file being deleted, or one we may not read, is not provably free: it counts as a live share (the
        # limit still ends the wait) and is said once.
        $live++
        if ($warned.Add($f.Name)) { Write-Host "WARN lock: cannot probe $($f.Name) ($($_.Exception.Message)); counted as a running build" }
      }
      catch [System.IO.IOException] { $live++ }
    }
    if ($live -eq 0) { break }
    Start-Sleep -Milliseconds 100
    if ($sw.Elapsed.TotalSeconds -ge $next) {
      $next += $msgS
      if (Test-ArLockSay $say $sw.Elapsed.TotalSeconds) { Write-Host "waiting for $live build(s) to end before the fast window" }
    }
    if ($sw.Elapsed.TotalSeconds -ge $limitS) {
      $m.ReleaseMutex(); $m.Dispose()
      throw "FAIL lock: $live build(s) still running after $([math]::Round($sw.Elapsed.TotalSeconds)) s, over the $limitS s limit (ASTRORAY_LOCK_TIMEOUT_S)"
    }
  }
  $script:ArWinMutex = $m
  $env:ASTRORAY_FASTWIN_PID = [string]$PID
  $script:ArWinOwnsPid = $true
  return $sw.Elapsed.TotalSeconds
}

function Exit-ArFastWindow {
  if ($script:ArWinMutex) {
    try { $script:ArWinMutex.ReleaseMutex() } catch { }
    $script:ArWinMutex.Dispose()
    $script:ArWinMutex = $null
  }
  if ($script:ArWinOwnsPid) { $env:ASTRORAY_FASTWIN_PID = $null; $script:ArWinOwnsPid = $false }
}
