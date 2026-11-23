# SPDX-License-Identifier: Apache-2.0
# Machine mutex and per-tree lock (0009 d8). Dot-source this file.
# The outermost holder sets ASTRORAY_LOCK_PID so nested tool calls do not re-lock.
# Each holder records itself in an owner file so a waiter can name it. The machine owner file lives in a
# machine-wide directory (every worktree and agent shell must see it; TEMP and the profile are per user or
# sandbox); the tree lock's sits beside its lock file. Environment, read at each call so tests never touch
# the real lock: ASTRORAY_LOCK_NAME (mutex), ASTRORAY_LOCK_DIR (owner file), ASTRORAY_LOCK_MESSAGE_S
# (seconds between wait ticks; the first three print), ASTRORAY_LOCK_TIMEOUT_S (longest a holder may keep
# the lock while someone waits).
$script:ArMutex = $null
$script:ArOwnsLockPid = $false
$script:ArOwnerPath = $null

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

# The tool a person would recognise: the outermost script outside tools/lib, else the process name.
function Get-ArLockTool {
  $frames = @(Get-PSCallStack | Where-Object { $_.ScriptName -and (Split-Path -Parent $_.ScriptName) -ne $PSScriptRoot })
  if ($frames.Count) { return (Split-Path -Leaf $frames[-1].ScriptName) }
  return (Get-Process -Id $PID).ProcessName
}

# Written to a temporary file and moved, so a reader never sees half a file. Never fatal: the lock works
# without it, only the waiting message loses its name.
function Write-ArLockOwner([string]$path) {
  try {
    New-Item -ItemType Directory -Force (Split-Path -Parent $path) -ErrorAction Stop | Out-Null
    $cmd = [Environment]::CommandLine
    if ($cmd.Length -gt 120) { $cmd = $cmd.Substring($cmd.Length - 120) }
    $o = [ordered]@{ pid = $PID; tool = (Get-ArLockTool); cmd = $cmd; started = [DateTimeOffset]::Now.ToString('yyyy-MM-ddTHH:mm:sszzz') }
    $tmp = "$path.$PID.tmp"
    [IO.File]::WriteAllText($tmp, ($o | ConvertTo-Json -Compress))
    [IO.File]::Move($tmp, $path, $true)
    return $true
  } catch {
    Write-Host "WARN lock: owner file not written: $($_.Exception.Message)"
    return $false
  }
}

function Remove-ArLockOwner([string]$path) {
  if (-not $path) { return }
  try {
    $o = Get-Content -Raw $path -ErrorAction Stop | ConvertFrom-Json
    if ([int]$o.pid -eq $PID) { Remove-Item -Force $path -ErrorAction Stop }
  } catch { }
}

# The recorded holder, or $null. Alive: its pid runs and that process started before the record (a reused
# pid started later).
function Read-ArLockOwner([string]$path) {
  try { $o = Get-Content -Raw $path -ErrorAction Stop | ConvertFrom-Json } catch { return $null }
  if (-not $o -or -not $o.pid -or -not $o.started) { return $null }
  # ConvertFrom-Json turns an ISO string into a DateTime already.
  $started = if ($o.started -is [DateTime]) { [DateTimeOffset]$o.started } else { [DateTimeOffset]::Parse("$($o.started)", [Globalization.CultureInfo]::InvariantCulture) }
  $p = Get-Process -Id ([int]$o.pid) -ErrorAction SilentlyContinue
  $alive = [bool]$p
  if ($p) { try { $alive = ([DateTimeOffset]$p.StartTime) -le $started.AddSeconds(1) } catch { } }
  $text = $started.ToLocalTime().ToString('yyyy-MM-ddTHH:mm:sszzz')
  return [pscustomobject]@{ Pid = [int]$o.pid; Tool = "$($o.tool)"; Started = $started; Text = $text; Alive = $alive }
}

function Format-ArLockWait($owner) {
  if ($owner -and $owner.Alive) { return "waiting for $($owner.Tool) (pid $($owner.Pid)) since $($owner.Text)" }
  return 'waiting for unknown holder (owner.json missing or its pid not running)'
}

# Whether a wait tick prints: the first three ticks name the holder, then one line per 300 s keeps a long,
# legitimate queue from filling the console.
function Test-ArLockSay($state, [double]$elapsedS) {
  if ($state.Said -lt 3 -or $elapsedS - $state.Last -ge 300) { $state.Said++; $state.Last = $elapsedS; return $true }
  return $false
}

function Enter-ArMachineLock {
  # Returns seconds waited; 0 when nested under a live lock holder.
  $held = $env:ASTRORAY_LOCK_PID
  if ($held -and (Get-Process -Id ([int]$held) -ErrorAction SilentlyContinue)) { return 0.0 }
  $name = if ($env:ASTRORAY_LOCK_NAME) { $env:ASTRORAY_LOCK_NAME } else { 'Global\astroray2' }
  $msgS = Get-ArLockSetting 'ASTRORAY_LOCK_MESSAGE_S' 30
  $limitS = Get-ArLockSetting 'ASTRORAY_LOCK_TIMEOUT_S' 3600
  $ownerPath = Join-Path (Get-ArLockDir) 'owner.json'
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $m = New-Object System.Threading.Mutex($false, $name)
  $say = @{ Said = 0; Last = 0.0 }
  while ($true) {
    try {
      if ($m.WaitOne([TimeSpan]::FromSeconds($msgS))) { break }
    } catch [System.Threading.AbandonedMutexException] {
      # This process now holds the mutex; the record, if any, is the dead holder's.
      $o = Read-ArLockOwner $ownerPath
      $who = if ($o) { "pid $($o.Pid) ($($o.Tool), started $($o.Text))" } else { 'pid unknown (no owner.json)' }
      Write-Host "WARN lock: abandoned by ${who}: the holder died without releasing"
      break
    }
    $o = Read-ArLockOwner $ownerPath
    if (Test-ArLockSay $say $sw.Elapsed.TotalSeconds) { Write-Host (Format-ArLockWait $o) }
    # The limit is on the current holder's hold, not this caller's wait: a long queue of legitimate holders
    # is not a hang. Without a live record, this caller's own wait is the only clock.
    $holdS = if ($o -and $o.Alive) { ([DateTimeOffset]::Now - $o.Started).TotalSeconds } else { $sw.Elapsed.TotalSeconds }
    if ($holdS -ge $limitS) {
      $m.Dispose()
      $who = if ($o -and $o.Alive) { "$($o.Tool) (pid $($o.Pid)), holding $([math]::Round($holdS)) s since $($o.Text)" } else { 'unknown holder' }
      throw "FAIL lock: waited $([math]::Round($sw.Elapsed.TotalSeconds)) s for $who, over the $limitS s limit (ASTRORAY_LOCK_TIMEOUT_S); owner file $ownerPath"
    }
  }
  $script:ArMutex = $m
  $env:ASTRORAY_LOCK_PID = [string]$PID
  $script:ArOwnsLockPid = $true
  if (Write-ArLockOwner $ownerPath) { $script:ArOwnerPath = $ownerPath }
  return $sw.Elapsed.TotalSeconds
}

function Exit-ArMachineLock {
  if ($script:ArMutex) {
    # Before the release: afterwards the file may already be the next holder's.
    Remove-ArLockOwner $script:ArOwnerPath
    $script:ArOwnerPath = $null
    try { $script:ArMutex.ReleaseMutex() } catch { }
    $script:ArMutex.Dispose()
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
