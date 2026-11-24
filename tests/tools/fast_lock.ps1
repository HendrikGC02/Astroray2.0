# SPDX-License-Identifier: Apache-2.0
# tools/lib/lock.ps1 against a private mutex and owner directory (never the real Global\astroray2):
# a waiter names the holder (three ticks, then every 300 s) and fails loudly once the holder has held past the
# limit; a holder stopped by its recorded pid leaves an abandoned mutex the waiter takes over, and the
# owner file is gone after the new holder releases; with no live owner record (a reused pid) the waiter names
# no holder and its own wait is the clock; a tree-lock waiter names the holder too; the fast window of the gate
# (Enter-ArFastWindow) waits for a running build's share and holds new builds back (each other party proven to be
# started and blocked before the release, and to enter after it), a build nested under the window does not wait,
# and a build killed while holding its share leaves nothing behind. The wiring is run for real from a copy of
# tools/: build.ps1 waits for a held window and then holds a share while it runs, and `check.ps1 gate` waits
# for a held window before its fast step. Run by tests/tools/run_fast.ps1 (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lockLib = Join-Path $root 'tools/lib/lock.ps1'
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) {
  if (-not $ok) { Write-Host "FAIL fast_lock: $what"; @($out) | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }; $script:bad++ }
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_lock_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
# The gate runs this under the real lock: a set ASTRORAY_LOCK_PID would make every call here a nested no-op.
$env:ASTRORAY_LOCK_PID = $null
$env:ASTRORAY_FASTWIN_PID = $null
# Scaled down from the defaults (30 s, 3600 s) so the tier stays quick; the logic is the same.
$env:ASTRORAY_LOCK_MESSAGE_S = '0.5'
$env:ASTRORAY_LOCK_TIMEOUT_S = '1.5'
# The holder signals once it holds the mutex; waiters start only then, so a slow pwsh start of the holder
# never eats into the hold.
# Given a directory, both scripts take the tree lock on it instead of the machine lock.
$holder = Join-Path $tmp 'holder.ps1'
Set-Content -Path $holder -Value @"
param([double]`$s, [string]`$ready, [string]`$tree, [string]`$release)
. '$lockLib'
if (`$tree) { `$null = Enter-ArTreeLock -Dir `$tree } else { `$null = Enter-ArMachineLock }
# A build share as well, for the case that checks a killed build leaves none behind.
if (`$env:FAST_LOCK_SHARE) { `$null = Enter-ArBuildShare }
Set-Content -Path `$ready -Value ready
# Given a release file: hold until it appears, at most s seconds.
if (`$release) { `$sw = [Diagnostics.Stopwatch]::StartNew(); while (-not (Test-Path `$release) -and `$sw.Elapsed.TotalSeconds -lt `$s) { Start-Sleep -Milliseconds 20 } }
else { Start-Sleep -Milliseconds ([int](`$s * 1000)) }
if (`$tree) { Exit-ArTreeLock } else { Exit-ArMachineLock }
"@
$waiter = Join-Path $tmp 'waiter.ps1'
Set-Content -Path $waiter -Value @"
param([string]`$tree)
. '$lockLib'
if (`$tree) { `$null = Enter-ArTreeLock -Dir `$tree } else { `$null = Enter-ArMachineLock }
'ACQUIRED'
if (`$tree) { Exit-ArTreeLock } else { Exit-ArMachineLock }
"@
$procs = @()
$wh = @{}

# A child started here and every process it started, by the pid this script launched.
function Stop-Tree($p) { if ($p -and -not $p.HasExited) { & taskkill /PID $p.Id /T /F 2>&1 | Out-Null } }
function Use-Lock([string]$tag) { $env:ASTRORAY_LOCK_NAME = "Local\astroray2-test-$PID-$tag"; $env:ASTRORAY_LOCK_DIR = Join-Path $tmp $tag }
# Starts a pwsh script on the lock of $tag (the child inherits the environment set here).
function Start-On([string]$tag, [string]$name, [string[]]$scriptArgs) {
  Use-Lock $tag
  $p = Start-Process pwsh -ArgumentList (@('-NoProfile', '-File') + $scriptArgs) -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $tmp "$name.out") -RedirectStandardError (Join-Path $tmp "$name.err")
  $null = $p.Handle   # keeps the exit code readable after the process ends
  $script:procs += $p
  return $p
}
function Wait-Ready([string]$tag) {
  $f = Join-Path $tmp "$tag.ready"
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Test-Path $f)) { if ($sw.Elapsed.TotalSeconds -gt 15) { throw "holder $tag not ready in 15 s" }; Start-Sleep -Milliseconds 10 }
}
# A child on a VT console renders its uncaught error with ANSI colour codes, around the wrap gutter too, so
# the codes go before any line is joined or matched.
function Read-Out([string]$name) {
  $o = @(foreach ($s in 'out', 'err') { $p = Join-Path $tmp "$name.$s"; if (Test-Path $p) { Get-Content $p } })
  return @($o | ForEach-Object { $_ -replace '\x1b\[[0-9;?]*[A-Za-z]', '' })
}

try {
  # Waiting messages: three ticks name the holder, then one line per 300 s.
  . $lockLib
  $st = @{ Said = 0; Last = 0.0 }
  $printed = @(foreach ($s in (1..40 | ForEach-Object { $_ * 30.0 })) { if (Test-ArLockSay $st $s) { $s } })
  Check 'messages: three ticks, then every 300 s' (($printed -join ',') -eq '30,60,90,390,690,990')

  # Wiring, started now and judged at the end. A copy of tools/ makes out/ (summaries, the tree lock) private to
  # the test; every lock is on a private name. One thread job per party owns that party's window (a mutex is
  # owned by its thread); the real build.ps1 and `check.ps1 gate` start behind it. The tree lock of the copy is
  # held here so that the build stays alive, and keeps its share, once the window is released.
  $wroot = Join-Path $tmp 'wroot'
  New-Item -ItemType Directory -Force (Join-Path $wroot 'out/build') | Out-Null
  Copy-Item -Recurse (Join-Path $root 'tools') (Join-Path $wroot 'tools')
  $holdJob = { param($lib, $sync) . $lib
    $null = Enter-ArFastWindow
    $sync.held = $true
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $sync.release -and $sw.Elapsed.TotalSeconds -lt 60) { Start-Sleep -Milliseconds 20 }
    Exit-ArFastWindow }
  foreach ($tag in 'wireb', 'wireg') {
    Use-Lock $tag
    $sync = [hashtable]::Synchronized(@{ held = $false; release = $false })
    $wh[$tag] = [pscustomobject]@{ Job = (Start-ThreadJob -ScriptBlock $holdJob -ArgumentList $lockLib, $sync); Sync = $sync }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $sync.held -and $sw.Elapsed.TotalSeconds -lt 15) { Start-Sleep -Milliseconds 5 }
    if (-not $sync.held) { throw "window holder $tag not ready in 15 s" }
  }
  # The holders' Enter-ArFastWindow marked this process as the window's owner; the children are other worktrees.
  $env:ASTRORAY_FASTWIN_PID = $null
  $env:ASTRORAY_LOCK_MESSAGE_S = '0.2'; $env:ASTRORAY_LOCK_TIMEOUT_S = '60'
  $null = Enter-ArTreeLock (Join-Path $wroot 'out/build')
  $wb = Start-On 'wireb' 'wb' @((Join-Path $wroot 'tools/build.ps1'), '-CheckOnly', '-NoMachineLock')
  $wg = Start-On 'wireg' 'wg' @((Join-Path $wroot 'tools/check.ps1'), 'gate')

  # The gate's window modes, through the real check.ps1 of a private copy whose gate table is cut to `build dev`,
  # `kernelgate` and `compile timing` and whose slow parts are stand-ins: the fast step sleeps 0.5 s and writes a
  # fast.json with a 0.5 s build, the build step sleeps 0.5 s, and the kernelgate stand-in records the window
  # owner (ASTRORAY_FASTWIN_PID) it runs under, then holds `--time` (the compile timing step) until released.
  # One gate with -Landing and one without start now, in parallel, and are judged at the end.
  function New-GateCopy([string]$name) {
    $r = Join-Path $tmp $name
    New-Item -ItemType Directory -Force (Join-Path $r 'out/check') | Out-Null
    Copy-Item -Recurse (Join-Path $root 'tools') (Join-Path $r 'tools')
    $f = Join-Path $r 'tools/check.ps1'
    $t = ([IO.File]::ReadAllText($f)) -replace "`r`n", "`n"
    $fast = "Step 'fast' { & pwsh -NoProfile -File tools/check.ps1 fast }"
    $judge = '  # Judge one finished step'
    if (-not $t.Contains($fast) -or -not $t.Contains($judge)) { throw "check.ps1 no longer has the anchors New-GateCopy cuts at ($fast / $judge)" }
    $t = $t.Replace($fast, "Step 'fast' { & pwsh -NoProfile -File tools/stub_fast.ps1 }")
    $t = $t.Replace($judge, "  `$gateSteps = @(`$gateSteps | Where-Object { `$_.Name -in 'build dev', 'kernelgate', 'compile timing' })`n$judge")
    [IO.File]::WriteAllText($f, $t)
    Set-Content (Join-Path $r 'tools/stub_fast.ps1') @'
Start-Sleep -Milliseconds 500
'{"steps":[{"name":"build","seconds":0.5,"pass":true}]}' | Set-Content (Join-Path (Split-Path -Parent $PSScriptRoot) 'out/check/fast.json')
exit 0
'@
    Set-Content (Join-Path $r 'tools/build.ps1') "Start-Sleep -Milliseconds 500`nexit 0"
    Set-Content (Join-Path $r 'tools/kernelgate.py') @'
import os, sys, time
d = os.environ['FAST_LOCK_STUBDIR']
timed = '--time' in sys.argv
with open(os.path.join(d, 'kg_time' if timed else 'kg_row'), 'w') as f:
    f.write(os.environ.get('ASTRORAY_FASTWIN_PID', ''))
end = time.time() + 60
while timed and not os.path.exists(os.path.join(d, 'release')) and time.time() < end:
    time.sleep(0.02)
'@
    return $r
  }
  $gates = @()
  foreach ($landing in $true, $false) {
    $tag = if ($landing) { 'gateL' } else { 'gateN' }
    $dir = Join-Path $tmp "$tag.stub"
    New-Item -ItemType Directory -Force $dir | Out-Null
    $gr = New-GateCopy "$tag.root"
    $env:FAST_LOCK_STUBDIR = $dir
    $argv = @((Join-Path $gr 'tools/check.ps1'), 'gate') + $(if ($landing) { @('-Landing') } else { @() })
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $gates += [pscustomobject]@{ Tag = $tag; Landing = $landing; Dir = $dir; Wall = $sw; Proc = (Start-On $tag "g_$tag" $argv) }
    $env:FAST_LOCK_STUBDIR = $null
  }
  $env:ASTRORAY_LOCK_MESSAGE_S = '0.5'; $env:ASTRORAY_LOCK_TIMEOUT_S = '1.5'

  # Both holders start together: 'timeout' holds limit + 1.5 s, 'abandon' until it is stopped.
  $h1 = Start-On 'timeout' 'h1' @($holder, '3', (Join-Path $tmp 'timeout.ready'))
  $env:FAST_LOCK_SHARE = '1'
  $h2 = Start-On 'abandon' 'h2' @($holder, '60', (Join-Path $tmp 'abandon.ready'))
  $env:FAST_LOCK_SHARE = $null
  # 'unknown' holds past the waiter's own limit; 'tree' holds the tree lock until its waiter has named it.
  $h3 = Start-On 'unknown' 'h3' @($holder, '60', (Join-Path $tmp 'unknown.ready'))
  $treeDir = Join-Path $tmp 'tree'
  $treeRelease = Join-Path $tmp 'tree.release'
  $h4 = Start-On 'tree' 'h4' @($holder, '10', (Join-Path $tmp 'tree.ready'), $treeDir, $treeRelease)
  # Case 1: past the 1.5 s limit the waiter gives up, naming the holder.
  Wait-Ready 'timeout'
  $w1 = Start-On 'timeout' 'w1' @($waiter)
  # Case 3: the record names the live holder's pid, but a start long before that process began, as a reused
  # pid would: no live owner, so the waiter's own wait is the clock and the holder is unknown.
  Wait-Ready 'unknown'
  Use-Lock 'unknown'
  $stale = [ordered]@{ pid = $h3.Id; tool = 'holder.ps1'; cmd = 'stale'; started = '2000-01-01T00:00:00+00:00' }
  [IO.File]::WriteAllText((Join-Path $env:ASTRORAY_LOCK_DIR 'owner.json'), ($stale | ConvertTo-Json -Compress))
  # A limit of five 0.2 s ticks: a clock taken from the stale record (26 years old) would fail on the first
  # tick, before three waiting lines and before 1 s.
  $env:ASTRORAY_LOCK_MESSAGE_S = '0.2'; $env:ASTRORAY_LOCK_TIMEOUT_S = '1.0'
  $w3 = Start-On 'unknown' 'w3' @($waiter)
  $env:ASTRORAY_LOCK_MESSAGE_S = '0.5'; $env:ASTRORAY_LOCK_TIMEOUT_S = '1.5'
  # Case 4: a waiter on the tree lock names the holder from .tree.owner.json, then acquires after release.
  # The holder is released only once the waiting line is out (below), so a slow start cannot miss the hold.
  Wait-Ready 'tree'
  $env:ASTRORAY_LOCK_MESSAGE_S = '0.2'
  $w4 = Start-On 'tree' 'w4' @($waiter, $treeDir)
  $env:ASTRORAY_LOCK_MESSAGE_S = '0.5'
  # Case 2: the holder is stopped by the pid its owner file records. This process keeps a handle on the
  # mutex so it survives the holder as an abandoned mutex, which the waiter then takes over.
  Wait-Ready 'abandon'
  Use-Lock 'abandon'
  $keep = New-Object System.Threading.Mutex($false, $env:ASTRORAY_LOCK_NAME)
  $owner = Join-Path $env:ASTRORAY_LOCK_DIR 'owner.json'
  $abandonShares = Join-Path $env:ASTRORAY_LOCK_DIR 'fastwin'
  Check 'window: the holder holds a build share before it is stopped' (@(Get-ChildItem $abandonShares -Filter 'share.*.lock').Count -eq 1)
  Stop-Process -Id ((Get-Content -Raw $owner | ConvertFrom-Json).pid) -Force
  $h2.WaitForExit()
  $w2 = Start-On 'abandon' 'w2' @($waiter)
  $sw4 = [Diagnostics.Stopwatch]::StartNew()
  while ($sw4.Elapsed.TotalSeconds -lt 10 -and -not ((Read-Out 'w4') -match 'waiting for')) { Start-Sleep -Milliseconds 20 }
  Set-Content -Path $treeRelease -Value release
  $w1.WaitForExit(); $w2.WaitForExit(); $w3.WaitForExit(); $w4.WaitForExit()
  $keep.Dispose()

  $out = Read-Out 'w1'
  # pwsh wraps an uncaught error to the console width behind a `|` gutter: join the pieces again.
  $txt = ($out -join "`n") -replace '\r?\n\s*\|?\s*', ' '
  Check 'timeout: the waiter exits non-zero' ($w1.ExitCode -ne 0) $out
  Check 'timeout: the waiter never acquires' (-not $txt.Contains('ACQUIRED')) $out
  Check 'timeout: waiting message names the holder' ($txt -match "waiting for holder\.ps1 \(pid $($h1.Id)\) since \d{4}-") $out
  $m = [regex]::Match($txt, "FAIL lock: waited (\d+) s for holder\.ps1 \(pid $($h1.Id)\), holding (\d+) s since [^,]+, over the 1\.5 s limit")
  # The limit is on the holder's hold, so this waiter's own wait (rounded) never exceeds it.
  Check 'timeout: FAIL lock names the holder and its hold' ($m.Success -and [int]$m.Groups[2].Value -ge 1.5 -and [int]$m.Groups[1].Value -le [int]$m.Groups[2].Value + 1) $out
  Check 'timeout: FAIL lock names the owner file' (($txt -replace '[\s|]', '').Contains(((Join-Path (Join-Path $tmp 'timeout') 'owner.json') -replace '\s', ''))) $out

  $out = Read-Out 'w2'
  $txt = $out -join "`n"
  Check 'abandoned: the waiter warns, naming the dead holder' ($txt -match "WARN lock: abandoned by pid $($h2.Id) \(holder\.ps1, started [^)]+\): the holder died without releasing") $out
  Check 'abandoned: the waiter proceeds as holder' ($w2.ExitCode -eq 0 -and $txt.Contains('ACQUIRED')) $out
  Check 'abandoned: owner.json is gone after release' (-not (Test-Path $owner)) $out

  $out = Read-Out 'w3'
  $txt = ($out -join "`n") -replace '\r?\n\s*\|?\s*', ' '
  Check 'unknown: the waiter exits non-zero without acquiring' ($w3.ExitCode -ne 0 -and -not $txt.Contains('ACQUIRED')) $out
  Check 'unknown: waiting message names no holder' ($txt.Contains('waiting for unknown holder (owner.json missing or its pid not running)') -and -not $txt.Contains('waiting for holder.ps1')) $out
  # The waiter's own wait is the clock: five ticks to the 1 s limit, the first three printed, then the FAIL.
  $said = @($out | Where-Object { $_ -eq 'waiting for unknown holder (owner.json missing or its pid not running)' }).Count
  Check 'unknown: exactly three waiting lines before the FAIL' ($said -eq 3) $out
  $m = [regex]::Match($txt, 'FAIL lock: waited (\d+) s for unknown holder, over the 1 s limit')
  Check 'unknown: FAIL lock after the waiter''s own wait' ($m.Success -and [int]$m.Groups[1].Value -ge 1 -and [int]$m.Groups[1].Value -le 2) $out

  $out = Read-Out 'w4'
  $txt = $out -join "`n"
  Check 'tree: waiting message names the holder' ($txt -match "waiting for holder\.ps1 \(pid $($h4.Id)\) since \d{4}-") $out
  Check 'tree: the waiter acquires after release' ($w4.ExitCode -eq 0 -and $txt.Contains('ACQUIRED')) $out
  Check 'tree: .tree.owner.json is gone after release' (-not (Test-Path (Join-Path $treeDir '.tree.owner.json'))) $out

  # Fast window. A job runs the other party in its own runspace on the same private mutex and directory. It
  # signals just before it calls Enter-*, so that the pause after the signal is spent inside the call; it stamps
  # the time it got in (Stopwatch ticks, one clock for every thread). A party counts as blocked only if it
  # started, still runs and has not got in after that pause, and as released only if it got in after the stamp
  # this thread took just before releasing.
  Use-Lock 'win'
  $fwDir = Join-Path $tmp 'win/fastwin'
  $job = { param($lib, $what, $sync) . $lib
    $sync.started = $true
    if ($what -eq 'window') { $null = Enter-ArFastWindow; $sync.entered = [Diagnostics.Stopwatch]::GetTimestamp(); Exit-ArFastWindow }
    else { $null = Enter-ArBuildShare; $sync.entered = [Diagnostics.Stopwatch]::GetTimestamp(); Exit-ArBuildShare }
    $sync.entered }
  function Start-Other([string]$what) {
    $sync = [hashtable]::Synchronized(@{ started = $false; entered = $null })
    $j = Start-ThreadJob -ScriptBlock $job -ArgumentList $lockLib, $what, $sync
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while (-not $sync.started -and $sw.Elapsed.TotalSeconds -lt 10) { Start-Sleep -Milliseconds 5 }
    Start-Sleep -Milliseconds 60
    return [pscustomobject]@{ Job = $j; Sync = $sync }
  }
  function Test-Blocked($o) { return [bool]($o.Sync.started -and $o.Job.State -eq 'Running' -and $null -eq $o.Sync.entered) }
  # Ticks of the release (to be taken before releasing) against the other party's entry.
  function Test-EnteredAfter($o, [long]$released) {
    $null = Wait-Job $o.Job -Timeout 10
    $err = @(Receive-Job $o.Job -ErrorAction SilentlyContinue 2>&1)
    $state = $o.Job.State
    Remove-Job $o.Job -Force
    return ($state -eq 'Completed' -and $null -ne $o.Sync.entered -and [long]$o.Sync.entered -gt $released)
  }
  # A share held here keeps the window out until it ends.
  $null = Enter-ArBuildShare
  Check 'window: a share file exists while a build runs' (@(Get-ChildItem $fwDir -Filter 'share.*.lock').Count -eq 1)
  $o = Start-Other 'window'
  Check 'window: waits while a build holds a share' (Test-Blocked $o)
  $released = [Diagnostics.Stopwatch]::GetTimestamp()
  Exit-ArBuildShare
  Check 'window: enters only after the build ended' (Test-EnteredAfter $o $released)
  Check 'window: a share leaves no file behind' (@(Get-ChildItem $fwDir -Filter 'share.*.lock').Count -eq 0)

  # While the window is held, a build waits. The environment is process-wide, so the owner's marker is cleared
  # here to let the job behave as another worktree's build.
  $w = Enter-ArFastWindow
  Check 'window: the owner marks its process tree' ($env:ASTRORAY_FASTWIN_PID -eq "$PID")
  Check 'window: a build nested under the window takes no share and does not wait' ((Enter-ArBuildShare) -eq 0.0 -and @(Get-ChildItem $fwDir -Filter 'share.*.lock').Count -eq 0)
  $env:ASTRORAY_FASTWIN_PID = $null
  $o = Start-Other 'share'
  Check 'window: a build starting during the window waits' (Test-Blocked $o)
  Check 'window: no share was taken while it waited' (@(Get-ChildItem $fwDir -Filter 'share.*.lock').Count -eq 0)
  $released = [Diagnostics.Stopwatch]::GetTimestamp()
  Exit-ArFastWindow
  Check 'window: the build proceeds only after the window closed' (Test-EnteredAfter $o $released)
  Check 'window: closing it clears the owner marker' (-not $env:ASTRORAY_FASTWIN_PID)

  # A build killed while holding its share: the OS closes the handle, so the window is not held up and the
  # share file is gone.
  # h2 held a share when it was stopped (the abandoned-mutex case above waited for its death).
  Use-Lock 'abandon'
  $t = [Diagnostics.Stopwatch]::StartNew()
  $null = Enter-ArFastWindow
  Exit-ArFastWindow
  Check 'window: a killed build leaves no share behind' ($t.Elapsed.TotalSeconds -lt 1.0 -and @(Get-ChildItem $abandonShares -Filter 'share.*.lock').Count -eq 0) @("took $($t.Elapsed.TotalSeconds) s")

  # Wiring, judged. Both children print the window's wait line within seconds (a poll of 8 s: a start under
  # load), and neither has taken anything yet.
  function Wait-Line($p, [string]$name, [string]$pattern) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt 8) {
      if (@(Read-Out $name | Where-Object { $_ -match $pattern }).Count) { return $true }
      if ($p.HasExited) { return $false }
      Start-Sleep -Milliseconds 20
    }
    return $false
  }
  $okB = Wait-Line $wb 'wb' "^waiting for the gate's fast window \(a build waits for it\)"
  $okG = Wait-Line $wg 'wg' "^waiting for the gate's fast window \(another gate holds it\)"
  $shareDirB = Join-Path $tmp 'wireb/fastwin'
  Check 'wiring: build.ps1 waits while the window is held' ($okB -and -not $wb.HasExited) (Read-Out 'wb')
  Check 'wiring: build.ps1 took no share while it waited' (@(Get-ChildItem $shareDirB -Filter 'share.*.lock' -ErrorAction SilentlyContinue).Count -eq 0) (Read-Out 'wb')
  Check 'wiring: check.ps1 gate waits for the window before its fast step' ($okG -and -not $wg.HasExited) (Read-Out 'wg')
  Stop-Tree $wg
  # Released: the build takes its share (and is held at the tree lock), which is a file named for its pid.
  $wh['wireb'].Sync.release = $true
  $shareB = Join-Path $shareDirB "share.$($wb.Id).lock"
  $sw5 = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Test-Path $shareB) -and -not $wb.HasExited -and $sw5.Elapsed.TotalSeconds -lt 6) { Start-Sleep -Milliseconds 20 }
  Check 'wiring: build.ps1 holds a share while it runs' ((Test-Path $shareB) -and -not $wb.HasExited) (Read-Out 'wb')
  Stop-Tree $wb
  $sw5.Restart()
  while ((Test-Path $shareB) -and $sw5.Elapsed.TotalSeconds -lt 5) { Start-Sleep -Milliseconds 20 }
  Check 'wiring: a killed build.ps1 leaves no share' (-not (Test-Path $shareB))

  # The gates' window modes. Each gate is held inside its compile timing step; a build started then must wait
  # for the window in both modes, and the owner marker a step runs under says whether the window is held for
  # it: the kernelgate step only under -Landing, the compile timing step always.
  $env:ASTRORAY_LOCK_MESSAGE_S = '0.2'; $env:ASTRORAY_LOCK_TIMEOUT_S = '60'
  foreach ($g in $gates) {
    $gid = "$($g.Proc.Id)"
    $kt = Join-Path $g.Dir 'kg_time'
    $sw6 = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path $kt) -and -not $g.Proc.HasExited -and $sw6.Elapsed.TotalSeconds -lt 40) { Start-Sleep -Milliseconds 20 }
    $mode = $(if ($g.Landing) { 'landing gate' } else { 'gate' })
    Check "$mode`: reaches the compile timing step" (Test-Path $kt) (Read-Out "g_$($g.Tag)")
    if (-not (Test-Path $kt)) { continue }
    Check "$mode`: compile timing runs inside the gate's window" ((Get-Content -Raw $kt) -eq $gid) @("owner marker '$((Get-Content -Raw $kt))', gate pid $gid")
    $row = (Get-Content -Raw (Join-Path $g.Dir 'kg_row'))
    Check "$mode`: the kernelgate step $(if ($g.Landing) { 'runs inside' } else { 'is outside' }) the window" ("$row" -eq $(if ($g.Landing) { $gid } else { '' })) @("owner marker '$row', gate pid $gid")
    $bn = Start-On $g.Tag "b_$($g.Tag)" @((Join-Path $wroot 'tools/build.ps1'), '-CheckOnly', '-NoMachineLock')
    $okW = Wait-Line $bn "b_$($g.Tag)" "^waiting for the gate's fast window \(a build waits for it\)"
    $shares = Join-Path $tmp "$($g.Tag)/fastwin"
    Check "$mode`: a build started during compile timing waits for the window" ($okW -and -not $bn.HasExited) (Read-Out "b_$($g.Tag)")
    Check "$mode`: that build took no share while it waited" (@(Get-ChildItem $shares -Filter 'share.*.lock' -ErrorAction SilentlyContinue).Count -eq 0)
    Set-Content (Join-Path $g.Dir 'release') release
    $null = $g.Proc.WaitForExit(30000)
    $wall = $g.Wall.Elapsed.TotalSeconds
    $out = Read-Out "g_$($g.Tag)"
    Check "$mode`: passes" ($g.Proc.HasExited -and $g.Proc.ExitCode -eq 0) $out
    $m = [regex]::Match(($out -join "`n"), 'PASS gate ([0-9.]+) s \(budget 180(?:\.0)? s, waited [0-9.]+ s\).*build steps ([0-9.]+) s not counted')
    Check "$mode`: the summary prints the tier seconds and the build seconds" $m.Success $out
    if ($m.Success) {
      $tier = [double]$m.Groups[1].Value; $builds = [double]$m.Groups[2].Value
      # 0.5 s of `build dev` and 0.5 s of the fast tier's build. Counted in the tier, X + builds would exceed the
      # gate's wall time by about `builds`; left out, X is wall time minus them.
      Check "$mode`: the build steps (build dev, and the build inside the fast step) are printed" ($builds -ge 0.9) @("builds $builds s")
      Check "$mode`: a slow build step does not count against B-09" ($tier + $builds -le $wall + 0.2) @("tier $tier s + builds $builds s against wall $wall s")
    }
    # The build proceeds once the gate has ended: it takes its share (and then waits at the tree lock).
    $shareN = Join-Path $shares "share.$($bn.Id).lock"
    $sw7 = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path $shareN) -and -not $bn.HasExited -and $sw7.Elapsed.TotalSeconds -lt 8) { Start-Sleep -Milliseconds 20 }
    Check "$mode`: the waiting build proceeds after the gate ended" (Test-Path $shareN) (Read-Out "b_$($g.Tag)")
  }
} finally {
  foreach ($h in @($wh.Values)) { $h.Sync.release = $true; $null = Wait-Job $h.Job -Timeout 5; Remove-Job $h.Job -Force -ErrorAction SilentlyContinue }
  Exit-ArTreeLock
  foreach ($p in $procs) { Stop-Tree $p }
  foreach ($p in $procs) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  foreach ($p in $procs) { $null = $p.WaitForExit(5000) }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { Write-Host "FAIL fast_lock: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_lock'
