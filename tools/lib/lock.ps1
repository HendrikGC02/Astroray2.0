# SPDX-License-Identifier: Apache-2.0
# Machine mutex and per-tree lock (0009 d8). Dot-source this file.
# The outermost holder sets ASTRORAY_LOCK_PID so nested tool calls do not re-lock.
$script:ArMutex = $null
$script:ArOwnsLockPid = $false

function Enter-ArMachineLock {
  # Returns seconds waited; 0 when nested under a live lock holder.
  $held = $env:ASTRORAY_LOCK_PID
  if ($held -and (Get-Process -Id ([int]$held) -ErrorAction SilentlyContinue)) { return 0.0 }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $m = New-Object System.Threading.Mutex($false, 'Global\astroray2')
  try { $null = $m.WaitOne() } catch [System.Threading.AbandonedMutexException] { }
  $script:ArMutex = $m
  $env:ASTRORAY_LOCK_PID = [string]$PID
  $script:ArOwnsLockPid = $true
  return $sw.Elapsed.TotalSeconds
}

function Exit-ArMachineLock {
  if ($script:ArMutex) {
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

function Enter-ArTreeLock {
  param([Parameter(Mandatory)][string]$Dir)
  New-Item -ItemType Directory -Force $Dir | Out-Null
  $path = Join-Path $Dir '.tree.lock'
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($true) {
    try {
      $script:ArTreeStream = [IO.File]::Open($path, 'OpenOrCreate', 'ReadWrite', 'None')
      break
    } catch [System.IO.IOException] {
      Start-Sleep -Milliseconds 200
    }
  }
  return $sw.Elapsed.TotalSeconds
}

function Exit-ArTreeLock {
  if ($script:ArTreeStream) {
    $script:ArTreeStream.Dispose()
    $script:ArTreeStream = $null
  }
}
