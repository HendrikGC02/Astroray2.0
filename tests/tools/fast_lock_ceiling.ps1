# SPDX-License-Identifier: Apache-2.0
# Hold ceilings of tools/lib/lock.ps1 (task 231) against a private mutex and owner directory:
# (1) a holder that hangs past its ceiling prints `FAIL lock: held N s, over the <job> ceiling` with its step,
#     stops its own child (and that child's child) and nothing else, removes its owner file, frees the mutex
#     (taken from the thread that owns it) and exits non-zero, so the next waiter acquires;
# (2) a waiter names the hung holder's step and that it is past its ceiling, and fails at the holder's ceiling
#     plus the grace when ASTRORAY_LOCK_TIMEOUT_S is unset;
# (3) every lock-taking tool has a ceiling row. Run by tests/tools/run_fast.ps1.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$lockLib = Join-Path $root 'tools/lib/lock.ps1'
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) {
  if (-not $ok) { Write-Host "FAIL fast_lock_ceiling: $what"; @($out) | Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }; $script:bad++ }
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_lock_ceiling_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:ASTRORAY_LOCK_PID = $null
$env:ASTRORAY_LOCK_MESSAGE_S = '0.3'
$env:ASTRORAY_LOCK_TIMEOUT_S = $null
$env:ASTRORAY_LOCK_NAME = "Local\astroray2-ceiling-$PID"
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
function Wait-File([string]$f) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (Test-Path $f)) { if ($sw.Elapsed.TotalSeconds -gt 15) { throw "$f not written in 15 s" }; Start-Sleep -Milliseconds 10 }
}
function Test-Alive([int]$id) { [bool](Get-Process -Id $id -ErrorAction SilentlyContinue) }

# A holder that hangs: a child pwsh that starts a grandchild, both sleeping, then itself sleeping.
$hang = Join-Path $tmp 'hang.ps1'
Set-Content -Path $hang -Value @"
param([string]`$dir)
. '$lockLib'
`$null = Enter-ArMachineLock -Job test
Set-ArLockStep 'hung step'
`$c = Start-Process pwsh -ArgumentList '-NoProfile','-File','$tmp/kid.ps1',`$dir -PassThru
Set-Content "`$dir/child.pid" `$c.Id
Set-Content "`$dir/ready" ready
Start-Sleep 120
"@
$kid = Join-Path $tmp 'kid.ps1'
Set-Content -Path $kid -Value @'
param($dir)
$p = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', 'Start-Sleep 120' -PassThru
Set-Content "$dir/grand.pid" $p.Id
Start-Sleep 120
'@
$wait = Join-Path $tmp 'wait.ps1'
Set-Content -Path $wait -Value @"
. '$lockLib'
`$script:ArWaiterGraceS = [double]`$env:FAST_GRACE
`$null = Enter-ArMachineLock -Job test
'ACQUIRED'
Exit-ArMachineLock
"@
# Holds the mutex with no watchdog (as a holder whose watchdog died would), its record naming a live pid.
$stuck = Join-Path $tmp 'stuck.ps1'
Set-Content -Path $stuck -Value @"
param([string]`$dir)
. '$lockLib'
`$m = New-Object System.Threading.Mutex(`$false, `$env:ASTRORAY_LOCK_NAME)
`$null = `$m.WaitOne()
`$null = Write-ArLockOwner (Join-Path `$env:ASTRORAY_LOCK_DIR 'owner.json') @{ ceiling_s = 1; step = 'stuck step' }
Set-Content "`$dir/ready" ready
Start-Sleep 120
"@

try {
  # An unrelated process must survive the watchdog.
  $bystander = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', 'Start-Sleep 120' -PassThru -NoNewWindow
  $procs += $bystander
  # 1. Ceiling stop.
  $d1 = Join-Path $tmp 'c1'; New-Item -ItemType Directory -Force $d1 | Out-Null
  $env:ASTRORAY_LOCK_CEILING_S = '2'
  $h = Start-Pwsh 'hang' @($hang, $d1)
  $env:ASTRORAY_LOCK_CEILING_S = $null
  Wait-File (Join-Path $d1 'ready'); Wait-File (Join-Path $d1 'grand.pid')
  $child = [int](Get-Content (Join-Path $d1 'child.pid')); $grand = [int](Get-Content (Join-Path $d1 'grand.pid'))
  Check 'the holder, its child and grandchild run before the ceiling' ((Test-Alive $child) -and (Test-Alive $grand))
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $null = $h.WaitForExit(15000)
  $out = Read-Out 'hang'
  Check 'the hung holder exits' $h.HasExited $out
  Check 'the hung holder exits non-zero' ($h.HasExited -and $h.ExitCode -ne 0) $out
  Check 'the FAIL line names the held seconds, the job ceiling and the step' (($out -join "`n") -match 'FAIL lock: held \d+ s, over the test ceiling of 2 s \(step: hung step; pid \d+\)') $out
  Check "the holder's child and grandchild are stopped" (-not (Test-Alive $child) -and -not (Test-Alive $grand)) $out
  Check 'an unrelated process is untouched' (Test-Alive $bystander.Id)
  Check 'the owner file is gone' (-not (Test-Path (Join-Path $env:ASTRORAY_LOCK_DIR 'owner.json')))
  $w = Start-Pwsh 'wait1' @($wait)
  $null = $w.WaitForExit(15000)
  Check 'the mutex is free for the next waiter (released by its owning thread, no abandonment)' ((Read-Out 'wait1') -contains 'ACQUIRED' -and -not ((Read-Out 'wait1') -match 'abandoned')) (Read-Out 'wait1')

  # 2. A waiter behind a holder past its ceiling and without a watchdog names it, then fails at ceiling + grace.
  $d2 = Join-Path $tmp 'c2'; New-Item -ItemType Directory -Force $d2 | Out-Null
  $s = Start-Pwsh 'stuck' @($stuck, $d2)
  Wait-File (Join-Path $d2 'ready')
  $env:FAST_GRACE = '1'
  $w = Start-Pwsh 'wait2' @($wait)
  $null = $w.WaitForExit(20000)
  $o2 = Read-Out 'wait2'
  Check 'the waiter fails instead of waiting for the unset 3600 s default' ($w.HasExited -and $w.ExitCode -ne 0) $o2
  Check "the waiter's failure names the holder's step and its ceiling plus grace" (($o2 -join ' ') -match "FAIL lock: waited .* step 'stuck step'.*over the 2 s limit \(the holder's 1 s ceiling \+ 1 s\)") $o2
  Check 'the waiter says the holder is past its ceiling' (($o2 -join ' ') -match "step 'stuck step', held \d+ s, PAST its 1 s ceiling") $o2

  # 3. Every lock-taking tool names a ceiling row that exists.
  . $lockLib
  foreach ($row in 'build', 'test', 'check-gate', 'check-full', 'canary', 'blender', 'perf', 'bench', 'gate_runs', 'other') {
    Check "ceiling row $row exists and is positive" ($script:ArLockCeilings.ContainsKey($row) -and $script:ArLockCeilings[$row] -gt 0)
  }
  foreach ($t in 'bench.ps1', 'perf.ps1', 'gate_runs.ps1', 'build.ps1', 'test.ps1', 'check.ps1', 'lib/blender.ps1', 'lib/canary.ps1') {
    $txt = Get-Content -Raw (Join-Path $root "tools/$t")
    Check "tools/$t passes a job to Enter-ArMachineLock" ($txt -notmatch 'Enter-ArMachineLock(?! -Job)' -and $txt -match 'Enter-ArMachineLock -Job')
  }
} finally {
  foreach ($p in $procs) { if ($p -and -not $p.HasExited) { & taskkill /PID $p.Id /T /F 2>&1 | Out-Null } }
  foreach ($p in $procs) { $null = $p.WaitForExit(5000) }
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { Write-Host "FAIL fast_lock_ceiling: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_lock_ceiling'
