# SPDX-License-Identifier: Apache-2.0
# Short-job priority of tools/lib/lock.ps1 (task 231) against a private mutex and owner directory: a build (a
# short job) queued behind a running gate, and behind a second gate that queued first, takes the lock when the
# first gate releases, before the second gate. A registration of a dead process, or one older than the yield
# window, does not hold a long tool back. Run by tests/tools/run_fast.ps1.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lockLib = Join-Path $root 'tools/lib/lock.ps1'
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) {
  if (-not $ok) { Write-Host "FAIL fast_lock_priority: $what"; @($out) | Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }; $script:bad++ }
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_lock_priority_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:ASTRORAY_LOCK_PID = $null
$env:ASTRORAY_LOCK_MESSAGE_S = '0.1'
$env:ASTRORAY_LOCK_TIMEOUT_S = $null
$env:ASTRORAY_LOCK_CEILING_S = $null
$env:ASTRORAY_LOCK_NAME = "Local\astroray2-priority-$PID"
$env:ASTRORAY_LOCK_DIR = Join-Path $tmp 'lock'
$procs = @()
function Read-Out([string]$name) {
  @(foreach ($s in 'out', 'err') { $p = Join-Path $tmp "$name.$s"; if (Test-Path $p) { Get-Content $p } }) | ForEach-Object { $_ -replace '\x1b\[[0-9;?]*[A-Za-z]', '' }
}
function Start-Pwsh([string]$name, [string[]]$a) {
  $p = Start-Process pwsh -ArgumentList (@('-NoProfile', '-File') + $a) -PassThru -NoNewWindow `
    -RedirectStandardOutput (Join-Path $tmp "$name.out") -RedirectStandardError (Join-Path $tmp "$name.err")
  $null = $p.Handle
  $script:procs += $p
  return $p
}
function Wait-Until([scriptblock]$cond, [string]$what) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (& $cond)) { if ($sw.Elapsed.TotalSeconds -gt 15) { throw "timed out: $what" }; Start-Sleep -Milliseconds 10 }
}

# Takes the lock as a job (short or not), stamps the moment it holds it, holds briefly, releases.
$taker = Join-Path $tmp 'taker.ps1'
Set-Content -Path $taker -Value @"
param([string]`$name, [string]`$job, [switch]`$short, [string]`$release)
. '$lockLib'
Set-Content "$tmp/pre.`$name" x
`$null = Enter-ArMachineLock -Job `$job -Short:`$short
Set-Content "$tmp/acq.`$name" ([DateTime]::UtcNow.Ticks)
if (`$release) { `$sw = [Diagnostics.Stopwatch]::StartNew(); while (-not (Test-Path `$release) -and `$sw.Elapsed.TotalSeconds -lt 20) { Start-Sleep -Milliseconds 20 } }
else { Start-Sleep -Milliseconds 100 }
Exit-ArMachineLock
"@
function Get-Acq([string]$name) { [long](Get-Content (Join-Path $tmp "acq.$name")) }

try {
  # The first gate holds until released.
  $rel = Join-Path $tmp 'release'
  $g1 = Start-Pwsh 'g1' @($taker, 'g1', 'check-gate', '-release', $rel)
  Wait-Until { Test-Path (Join-Path $tmp 'acq.g1') } 'g1 holds the lock'
  # The second gate queues first, the build after it; each is proven to be waiting before the next starts.
  $g2 = Start-Pwsh 'g2' @($taker, 'g2', 'check-gate')
  # g2 is past its marker and into the lock call well before the build's pwsh has started.
  Wait-Until { Test-Path (Join-Path $tmp 'pre.g2') } 'g2 starts'
  $b = Start-Pwsh 'b' @($taker, 'b', 'build', '-short')
  # The build's registration must be visible before the release, as a real queue's would be.
  Wait-Until { @(Get-ChildItem (Join-Path $env:ASTRORAY_LOCK_DIR 'shortwait') -Filter 'short.*.wait' -ErrorAction SilentlyContinue).Count -eq 1 } 'the build registered'
  Set-Content $rel release
  $null = $g1.WaitForExit(15000); $null = $b.WaitForExit(15000); $null = $g2.WaitForExit(15000)
  Check 'all three finish' ($g1.HasExited -and $b.HasExited -and $g2.HasExited) ((Read-Out 'g2') + (Read-Out 'b'))
  Check 'both queued jobs ran' ((Test-Path (Join-Path $tmp 'acq.b')) -and (Test-Path (Join-Path $tmp 'acq.g2'))) ((Read-Out 'g2') + (Read-Out 'b'))
  if ((Test-Path (Join-Path $tmp 'acq.b')) -and (Test-Path (Join-Path $tmp 'acq.g2'))) {
    Check 'the build queued behind a running gate starts before the second gate that queued first' ((Get-Acq 'b') -lt (Get-Acq 'g2')) @("build $(Get-Acq 'b') ticks, gate 2 $(Get-Acq 'g2') ticks")
  }
  Check "the build's registration is removed once it holds the lock" (@(Get-ChildItem (Join-Path $env:ASTRORAY_LOCK_DIR 'shortwait') -Filter 'short.*.wait' -ErrorAction SilentlyContinue).Count -eq 0)

  # A registration of a process that is gone, and one older than the yield window, hold nothing back.
  $dir = Join-Path $env:ASTRORAY_LOCK_DIR 'shortwait'
  Set-Content (Join-Path $dir 'short.999999.wait') 'waiting'
  $old = Join-Path $dir "short.$PID.wait"
  Set-Content $old 'waiting'
  (Get-Item $old).LastWriteTimeUtc = [DateTime]::UtcNow.AddMinutes(-6)
  . $lockLib
  Check 'a dead or stale short registration is not counted' ((Get-ArShortWaiting) -eq 0)
} finally {
  foreach ($p in $procs) { if ($p -and -not $p.HasExited) { & taskkill /PID $p.Id /T /F 2>&1 | Out-Null } }
  foreach ($p in $procs) { $null = $p.WaitForExit(5000) }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { Write-Host "FAIL fast_lock_priority: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_lock_priority'
