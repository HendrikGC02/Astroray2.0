# SPDX-License-Identifier: Apache-2.0
# tools/lib/lock.ps1 against a private mutex and owner directory (never the real Global\astroray2):
# a waiter names the holder every message interval and fails loudly once the holder has held past the
# limit; a holder stopped by its recorded pid leaves an abandoned mutex the waiter takes over, and the
# owner file is gone after the new holder releases. Run by tests/tools/run_fast.ps1 (gate step `tools tests`).
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
$env:ASTRORAY_LOCK_MESSAGE_S = '1'
$env:ASTRORAY_LOCK_TIMEOUT_S = '3'
$holder = Join-Path $tmp 'holder.ps1'
Set-Content -Path $holder -Value "param([int]`$s)`n. '$lockLib'`n`$null = Enter-ArMachineLock`nStart-Sleep -Seconds `$s`nExit-ArMachineLock"
$waiter = Join-Path $tmp 'waiter.ps1'
Set-Content -Path $waiter -Value ". '$lockLib'`n`$w = Enter-ArMachineLock`n'ACQUIRED'`nExit-ArMachineLock"
$procs = @()

# Starts a holder on its own mutex and owner directory; returns once its owner file names it.
function Start-Holder([string]$tag, [int]$holdS) {
  $env:ASTRORAY_LOCK_NAME = "Local\astroray2-test-$PID-$tag"
  $env:ASTRORAY_LOCK_DIR = Join-Path $tmp $tag
  $p = Start-Process pwsh -ArgumentList @('-NoProfile', '-File', $holder, $holdS) -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $tmp "$tag.out") -RedirectStandardError (Join-Path $tmp "$tag.err")
  $script:procs += $p
  $owner = Join-Path $env:ASTRORAY_LOCK_DIR 'owner.json'
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt 15) {
    if (Test-Path $owner) { try { if ((Get-Content -Raw $owner | ConvertFrom-Json).pid -eq $p.Id) { return $p } } catch { } }
    Start-Sleep -Milliseconds 25
  }
  throw "holder $tag wrote no owner file in 15 s"
}
function Use-Lock([string]$tag) { $env:ASTRORAY_LOCK_NAME = "Local\astroray2-test-$PID-$tag"; $env:ASTRORAY_LOCK_DIR = Join-Path $tmp $tag }

try {
  # Case 1: the holder keeps the lock 6 s; with a 3 s limit the waiter gives up naming it.
  $h1 = Start-Holder 'timeout' 6
  $out = @(& pwsh -NoProfile -File $waiter 2>&1 | ForEach-Object { "$_" })
  $code = $LASTEXITCODE
  # pwsh wraps an uncaught error to the console width behind a `|` gutter: join the pieces again.
  $txt = ($out -join "`n") -replace '\r?\n\s*\|?\s*', ' '
  Check 'timeout: the waiter exits non-zero' ($code -ne 0) $out
  Check 'timeout: the waiter never acquires' (-not $txt.Contains('ACQUIRED')) $out
  Check 'timeout: waiting message names the holder' ($txt -match "waiting for holder\.ps1 \(pid $($h1.Id)\) since \d{4}-") $out
  $m = [regex]::Match($txt, "FAIL lock: waited (\d+) s for holder\.ps1 \(pid $($h1.Id)\), holding (\d+) s since [^,]+, over the 3 s limit")
  # The limit is on the holder's hold, so this waiter's own wait (rounded) never exceeds it.
  Check 'timeout: FAIL lock names the holder and its hold' ($m.Success -and [int]$m.Groups[2].Value -ge 3 -and [int]$m.Groups[1].Value -le [int]$m.Groups[2].Value + 1) $out
  Check 'timeout: FAIL lock names the owner file' (($txt -replace '[\s|]', '').Contains(((Join-Path (Join-Path $tmp 'timeout') 'owner.json') -replace '\s', ''))) $out
  Stop-Process -Id $h1.Id -Force -ErrorAction SilentlyContinue

  # Case 2: the holder is stopped by the pid its owner file records. This process keeps a handle on the
  # mutex so it survives the holder as an abandoned mutex, which the waiter then takes over.
  $h2 = Start-Holder 'abandon' 60
  $keep = New-Object System.Threading.Mutex($false, $env:ASTRORAY_LOCK_NAME)
  $owner = Join-Path $env:ASTRORAY_LOCK_DIR 'owner.json'
  Stop-Process -Id ((Get-Content -Raw $owner | ConvertFrom-Json).pid) -Force
  $h2.WaitForExit()
  Use-Lock 'abandon'
  $out = @(& pwsh -NoProfile -File $waiter 2>&1 | ForEach-Object { "$_" })
  $code = $LASTEXITCODE
  $keep.Dispose()
  $txt = $out -join "`n"
  Check 'abandoned: the waiter warns, naming the dead holder' ($txt -match "WARN lock: abandoned by pid $($h2.Id) \(holder\.ps1, started [^)]+\): the holder died without releasing") $out
  Check 'abandoned: the waiter proceeds as holder' ($code -eq 0 -and $txt.Contains('ACQUIRED')) $out
  Check 'abandoned: owner.json is gone after release' (-not (Test-Path $owner)) $out
} finally {
  foreach ($p in $procs) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  foreach ($p in $procs) { $null = $p.WaitForExit(5000) }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { Write-Host "FAIL fast_lock: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_lock'
