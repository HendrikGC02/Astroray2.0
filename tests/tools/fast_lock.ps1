# SPDX-License-Identifier: Apache-2.0
# tools/lib/lock.ps1 against a private mutex and owner directory (never the real Global\astroray2):
# a waiter names the holder (three ticks, then every 300 s) and fails loudly once the holder has held past the
# limit; a holder stopped by its recorded pid leaves an abandoned mutex the waiter takes over, and the
# owner file is gone after the new holder releases; with no live owner record (a reused pid) the waiter names
# no holder and its own wait is the clock; a tree-lock waiter names the holder too. Run by tests/tools/run_fast.ps1 (gate step `tools tests`).
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
function Read-Out([string]$name) {
  $o = @(foreach ($s in 'out', 'err') { $p = Join-Path $tmp "$name.$s"; if (Test-Path $p) { Get-Content $p } })
  return $o
}

try {
  # Waiting messages: three ticks name the holder, then one line per 300 s.
  . $lockLib
  $st = @{ Said = 0; Last = 0.0 }
  $printed = @(foreach ($s in (1..40 | ForEach-Object { $_ * 30.0 })) { if (Test-ArLockSay $st $s) { $s } })
  Check 'messages: three ticks, then every 300 s' (($printed -join ',') -eq '30,60,90,390,690,990')

  # Both holders start together: 'timeout' holds limit + 1.5 s, 'abandon' until it is stopped.
  $h1 = Start-On 'timeout' 'h1' @($holder, '3', (Join-Path $tmp 'timeout.ready'))
  $h2 = Start-On 'abandon' 'h2' @($holder, '60', (Join-Path $tmp 'abandon.ready'))
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
} finally {
  foreach ($p in $procs) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  foreach ($p in $procs) { $null = $p.WaitForExit(5000) }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { Write-Host "FAIL fast_lock: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_lock'
