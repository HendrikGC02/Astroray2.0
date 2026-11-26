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
$env:ASTRORAY_LOCK_MESSAGE_S = '0.1'
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
function Wait-Until([scriptblock]$cond, [string]$what) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  while (-not (& $cond)) { if ($sw.Elapsed.TotalSeconds -gt 15) { throw "timed out: $what" }; Start-Sleep -Milliseconds 10 }
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
`$null = Write-ArLockOwner (Join-Path `$env:ASTRORAY_LOCK_DIR 'owner.json') @{ ceiling_s = 0.5; step = 'stuck step' }
Set-Content "`$dir/ready" ready
Start-Sleep 120
"@

$ends = Join-Path $tmp 'ends.ps1'
Set-Content -Path $ends -Value @"
param([string]`$dir, [string]`$hold)
. '$lockLib'
`$null = Enter-ArMachineLock -Job test
Set-Content "`$dir/ready" ready
if (`$hold -eq 'hold') { Start-Sleep 120 } elseif (`$hold) { Start-Sleep -Milliseconds 700 }
"@

try {
  # Each case has its own mutex and owner directory, so the cases start together and the fixture waits once.
  function Use-Lock([string]$tag) { $env:ASTRORAY_LOCK_NAME = "Local\astroray2-ceiling-$PID-$tag"; $env:ASTRORAY_LOCK_DIR = Join-Path $tmp "lock-$tag" }
  $bystander = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', 'Start-Sleep 120' -PassThru -NoNewWindow
  $procs += $bystander
  # 1. Ceiling stop of a hung holder with a child and a grandchild.
  $d1 = Join-Path $tmp 'c1'; New-Item -ItemType Directory -Force $d1 | Out-Null
  Use-Lock 'a'; $env:ASTRORAY_LOCK_CEILING_S = '2'
  $h = Start-Pwsh 'hang' @($hang, $d1)
  $env:ASTRORAY_LOCK_CEILING_S = $null
  $lockA = $env:ASTRORAY_LOCK_DIR
  # 2. A holder past its ceiling with no watchdog (a hung one whose watchdog failed): the waiter names it.
  $d2 = Join-Path $tmp 'c2'; New-Item -ItemType Directory -Force $d2 | Out-Null
  Use-Lock 'b'
  $s = Start-Pwsh 'stuck' @($stuck, $d2)
  # 3. A holder that ends without Exit-ArMachineLock, and 4. one that is killed: the next waiter gets the mutex
  # at once, with the abandoned-mutex warning.
  $d3 = Join-Path $tmp 'c3'; New-Item -ItemType Directory -Force $d3 | Out-Null
  Use-Lock 'c'
  $e = Start-Pwsh 'ends' @($ends, $d3, 'end')
  $d4 = Join-Path $tmp 'c4'; New-Item -ItemType Directory -Force $d4 | Out-Null
  Use-Lock 'd'
  $k = Start-Pwsh 'killed' @($ends, $d4, 'hold')
  foreach ($d in $d1, $d2, $d3, $d4) { Wait-File (Join-Path $d 'ready') }
  Wait-File (Join-Path $d1 'grand.pid')
  $child = [int](Get-Content (Join-Path $d1 'child.pid')); $grand = [int](Get-Content (Join-Path $d1 'grand.pid'))
  $env:FAST_GRACE = '0.5'
  Use-Lock 'b'; $w2 = Start-Pwsh 'wait2' @($wait)
  Use-Lock 'c'; $w3 = Start-Pwsh 'wait3' @($wait)
  Use-Lock 'd'; $w4 = Start-Pwsh 'wait4' @($wait)
  # A waiter must be blocked on the mutex when its holder dies: the kernel destroys a named mutex with its last
  # handle, and no abandonment is left to report to a waiter that starts later.
  Wait-Until { (Read-Out 'wait4') -match 'waiting for' } 'wait4 waits'
  Stop-Process -Id $k.Id -Force
  $null = $h.WaitForExit(15000)
  $out = Read-Out 'hang'
  Check 'the hung holder exits non-zero' ($h.HasExited -and $h.ExitCode -ne 0) $out
  Check 'the FAIL line names the held seconds, the job ceiling and the step' (($out -join "`n") -match 'FAIL lock: held \d+ s, over the test ceiling of 2 s \(step: hung step; pid \d+\)') $out
  Check "the holder's child and grandchild are stopped" (-not (Test-Alive $child) -and -not (Test-Alive $grand)) $out
  Check 'an unrelated process is untouched' (Test-Alive $bystander.Id)
  Check 'the owner file is gone' (-not (Test-Path (Join-Path $lockA 'owner.json')))
  Use-Lock 'a'; $w1 = Start-Pwsh 'wait1' @($wait)
  foreach ($p in $w1, $w2, $w3, $w4) { $null = $p.WaitForExit(15000) }
  Check 'the mutex is free for the next waiter (released by its owning thread, no abandonment)' ((Read-Out 'wait1') -contains 'ACQUIRED' -and -not ((Read-Out 'wait1') -match 'abandoned')) (Read-Out 'wait1')
  $o2 = Read-Out 'wait2'
  Check 'the waiter fails instead of waiting for the unset 3600 s default' ($w2.HasExited -and $w2.ExitCode -ne 0) $o2
  Check "the waiter's failure names the holder's step and its ceiling plus grace" (($o2 -join ' ') -match "FAIL lock: waited .* step 'stuck step'.*over the 1 s limit \(the holder's 0.5 s ceiling \+ 0.5 s\)") $o2
  Check 'the waiter says the holder is past its ceiling' (($o2 -join ' ') -match "step 'stuck step', held \d+ s, PAST its 0.5 s ceiling") $o2
  Check 'the waiter gets the mutex when its holder ended without Exit-ArMachineLock' ((Read-Out 'wait3') -contains 'ACQUIRED') (Read-Out 'wait3')
  $o4 = Read-Out 'wait4'
  Check 'the waiter gets the mutex when its holder was killed, with the abandoned warning' (($o4 -contains 'ACQUIRED') -and (($o4 -join ' ') -match 'WARN lock: abandoned by')) $o4
  Check 'the holder that ended without Exit-ArMachineLock exited by itself' ($e.HasExited -and $e.ExitCode -eq 0)

  # 5. Only the holder's descendants, created no earlier than the holder, are stopped (rows are synthetic: a
  # stranger's child under a reused parent pid is older than the holder).
  $t0 = [DateTime]::new(2026, 1, 1, 12, 0, 0)
  $rows = @(
    [pscustomobject]@{ ProcessId = 10; ParentProcessId = 1; CreationDate = $t0 },
    [pscustomobject]@{ ProcessId = 11; ParentProcessId = 10; CreationDate = $t0.AddSeconds(1) },
    [pscustomobject]@{ ProcessId = 12; ParentProcessId = 11; CreationDate = $t0.AddSeconds(2) },
    [pscustomobject]@{ ProcessId = 13; ParentProcessId = 10; CreationDate = $t0.AddSeconds(-5) },
    [pscustomobject]@{ ProcessId = 14; ParentProcessId = 13; CreationDate = $t0.AddSeconds(3) },
    [pscustomobject]@{ ProcessId = 20; ParentProcessId = 1; CreationDate = $t0.AddSeconds(1) })
  . $lockLib
  $kill = @(& $script:ArDescendantsFn $rows 10 $t0)
  Check 'the descendants are the child and grandchild, deepest first' (($kill -join ',') -eq '12,11') @("got $($kill -join ',')")

  # 6. A wrapper over Enter-ArMachineLock, as land.ps1's, forwards the job, ceiling and short flag.
  $line = [regex]::Match((Get-Content -Raw (Join-Path $root 'tools/land.ps1')), '(?m)^function Enter-ArMachineLock \{.*\}\s*$').Value
  Check 'land.ps1 wraps Enter-ArMachineLock on one line' ([bool]$line)
  $script:LandTimes = @{ lock_wait_s = 0.0 }
  $script:OrigEnterLock = { param([string]$Job = '', [double]$CeilingS = 0, [switch]$Short) $script:Got = "$Job|$CeilingS|$([bool]$Short)"; return 0.0 }
  Invoke-Expression $line
  $null = Enter-ArMachineLock -Job blender -CeilingS 420 -Short
  Check "land.ps1's wrapper forwards -Job, -CeilingS and -Short" ($script:Got -eq 'blender|420|True') @("got $($script:Got)")

  # 7. Every lock-taking tool names a ceiling row that exists.
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
