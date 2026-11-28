# SPDX-License-Identifier: Apache-2.0
# tools/perf.ps1 [-Scene b1m,b5m] [-Size 1920x1080] [-Record] [-Cumulative <sha>] [-MeasureTolerance [-Runs 5]]
# Pass-time regression check: runs bench.ps1 on b1m and b5m under one machine-lock hold and compares each
# `pass_ms` with the last bench/history line of the same machine and scene against B-38 (tools/budgets.toml).
# Refuses while another GPU user runs (blender, astroray_cli, astroray_tests_gpu, nsys outside this process
# tree): a shared GPU makes the number meaningless. It also refuses a binary that is dirty or built from other
# code than HEAD. The comparison and its decision table are tools/perf.py.
# -Record appends the measured pass_ms lines of a PASS to bench/history/<year>.jsonl (the lead's commit path);
# it refuses when build inputs are uncommitted.
# -Cumulative <sha> adds, per scene, the ratio of pass_ms and kernel_ms_per_sample to that commit's history
# lines (perf.py compare --cumulative; information, no limit).
# -MeasureTolerance runs the bench -Runs times back to back and prints the spreads that set B-38.
# Foreign load (lib/load.ps1: compilers, linkers, builds, Blender, GPU compute outside this process tree) seen during a
# bench run that then fails B-38 re-runs that bench once, inside the same lock hold and after Assert-QuietGpu, and
# prints `RERUN B-38 perf: load <processes>` with both results; the second decides. Without load the first decides
# (0009 d13). Load seen during the deciding run, pass or fail, refuses -Record (exit 2): a baseline recorded beside
# foreign load would loosen B-38.
# It is not a `check` tier: tiers write the 20-line summary and out/check/<tier>.json that report.py reads and
# budget B-08..B-10 time; perf is one more caller of bench.ps1. Ref: 0009 d13 (B-38), d18.
# -Shares replaces the pass-time check by the share check of tools/perf_shares.py: under the same lock hold,
# per scene one `nsys profile --trace=cuda --sample=none --cpuctxsw=none` capture of the bench (64 passes after
# 8 burn-in), `nsys export --type sqlite`, and the GPU idle share and the queue_build plus sort share of the
# pass (B-19), each against the last bench/history line of this machine within B-19's measured tolerance
# (tools/budgets.toml). -Shares -Record appends the lines of a PASS; -Shares -MeasureTolerance -Runs 5 prints the
# spreads that set the tolerance. A capture hung for 5 minutes is killed and refused (the lock is released).
# nsys: $env:ASTRORAY_NSYS or the newest installed one (lib/nsys.ps1). Ref: tasks/160; 0009 d13 (B-19), d18.
# Output: at most 20 summary lines plus the nsys commands; exit 0 pass, 1 a scene failed, 2 refused or unusable.
param(
  [string]$Cumulative = '',
  [string[]]$Scene = @('b1m', 'b5m'),
  [string]$Size = '1920x1080',
  [switch]$Record,
  [switch]$MeasureTolerance,
  [int]$Runs = 5,
  [switch]$Shares
)
$ErrorActionPreference = 'Stop'
$Scene = @($Scene | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/nsys.ps1')
. (Join-Path $PSScriptRoot 'lib/load.ps1')
$py = Get-ArPython
$tmp = Join-Path $root 'out/tmp/140'
if ($Shares) {
  $tmp = Join-Path $root 'out/tmp/perf-shares'
  try { $nsys = Get-ArNsys } catch { Write-Host "REFUSE perf: $($_.Exception.Message)"; exit 2 }
  # bench.ps1 builds the recordings before taking its lock; the captures run under ours, so do it first.
  & $py -I (Join-Path $PSScriptRoot 'scenes.py') @($Scene)
  if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL perf: scene recordings'; exit 1 }
}
New-Item -ItemType Directory -Force $tmp | Out-Null

# Inputs of the shipped binary: what a commit must not have touched since the build, and what must be clean to
# record a baseline. abigen.py emits the recorder and replayer (CMake DEPENDS), data/ is embedded as RCDATA
# (cmake/data.cmake). opsgen.py is not listed: its outputs (_ops.py for the extension, coverage.csv) never reach
# the CLI, and the inputs it reads that do (ops.def, spectrum_ref.h) are under src/. Tests, the budgets file and
# the other generators feed only test binaries or committed tables.
$buildInputs = '^(src/|cli/|include/|cmake/|data/|third_party/|CMakeLists\.txt|CMakePresets\.json|tools/abigen\.py)'
if ($Record) {
  $dirty = @(& git status --porcelain --untracked-files=all | ForEach-Object { $_.Substring(3) } | Where-Object { $_ -match $buildInputs })
  if ($dirty.Count) { Write-Host "REFUSE perf: -Record with uncommitted build inputs ($($dirty[0])); commit them and rebuild first"; exit 2 }
}

# Other GPU users: every process of these names that is not this process or one of its ancestors (land.ps1 and
# pwsh parents run this script). Checked after taking the lock, so a holder that queued behind us cannot start
# one, and again before every bench run. nvidia-smi is not used: WDDM lists no compute processes reliably.
# Ref: PR 133 ("nothing else on the GPU, checked before each capture").
$ancestors = @($PID)
$p = $PID
for ($i = 0; $i -lt 16; $i++) {
  $ci = Get-CimInstance Win32_Process -Filter "ProcessId=$p" -ErrorAction SilentlyContinue
  if (-not $ci -or -not $ci.ParentProcessId) { break }
  $p = [int]$ci.ParentProcessId; $ancestors += $p
}
function Assert-QuietGpu {
  $others = @(Get-Process -Name blender, astroray_cli, astroray_tests_gpu, nsys -ErrorAction SilentlyContinue |
      Where-Object { $ancestors -notcontains $_.Id })
  if ($others.Count) {
    Write-Host ("REFUSE perf: other GPU users running: " + (($others | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ') + '; wait for a quiet machine (tools/status.ps1)')
    exit 2
  }
}

# One bench run: the history lines it appended (the CLI stamps the neutral hardware id, cli/bench.cpp).
function Invoke-PerfBench([int]$r) {
  $before = @{}
  $dir = Join-Path $root 'out/bench'
  if (Test-Path $dir) { Get-ChildItem $dir -Filter 'history-*.jsonl' | ForEach-Object { $before[$_.FullName] = @(Get-Content $_.FullName).Count } }
  $log = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'bench.ps1') -Scene ($Scene -join ',') -Size $Size 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  if ($code -ne 0) { $log | Select-Object -Last 8 | ForEach-Object { Write-Host $_ }; Write-Host "FAIL perf: bench run $r exited $code"; exit 1 }
  $got = @()
  Get-ChildItem $dir -Filter 'history-*.jsonl' | ForEach-Object {
    $skip = if ($before.ContainsKey($_.FullName)) { $before[$_.FullName] } else { 0 }
    $got += @(Get-Content $_.FullName | Select-Object -Skip $skip)
  }
  return $got
}

$cli = Join-Path $root 'out/bin/astroray_cli.exe'
$null = Enter-ArMachineLock -Job perf
$sw = [Diagnostics.Stopwatch]::StartNew()   # lock wait excluded
$recordLoad = @()   # foreign load seen during the deciding bench runs
$newLines = @()   # per bench run: the history lines it appended
try {
  Assert-QuietGpu
  # The binary under test must be this tree's clean build: its stamped HEAD may trail HEAD only through commits
  # that touch no build input (a landing's deletion commit, docs), else the timing belongs to other code.
  if (-not (Test-Path $cli)) { Write-Host "REFUSE perf: $cli is missing; run tools/build.ps1"; exit 2 }
  $info = @(& $cli info 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match '^build: sha=' })
  if (-not $info -or $info[0] -notmatch '^build: sha=([0-9a-f]+) dirty=(\d+)') { Write-Host 'REFUSE perf: astroray_cli info prints no build line'; exit 2 }
  $binSha = $Matches[1]; $binDirty = $Matches[2]
  $head = (& git rev-parse HEAD).Trim()
  if ($binDirty -ne '0') { Write-Host "REFUSE perf: out/bin/astroray_cli.exe was built from a dirty tree ($($binSha.Substring(0, 9))); commit, then tools/build.ps1"; exit 2 }
  if ($binSha -ne $head) {
    $changed = @(& git diff --name-only $binSha $head 2>$null)
    if ($LASTEXITCODE -ne 0) { Write-Host "REFUSE perf: out/bin/astroray_cli.exe is stamped with $binSha, unknown to this tree; run tools/build.ps1"; exit 2 }
    $stale = @($changed | Where-Object { $_ -match $buildInputs })
    if ($stale.Count) { Write-Host "REFUSE perf: out/bin/astroray_cli.exe was built at $($binSha.Substring(0, 9)), HEAD $($head.Substring(0, 9)) changed $($stale[0]); run tools/build.ps1"; exit 2 }
  }
  Write-Host "binary build $($binSha.Substring(0, 9)) clean, HEAD $($head.Substring(0, 9))"

  $n = if ($MeasureTolerance) { $Runs } else { 1 }
  for ($r = 1; $r -le $n; $r++) {
    Assert-QuietGpu
    if ($Shares) {
      $exports = @()
      foreach ($s in $Scene) {
        Assert-QuietGpu
        $base = Join-Path $tmp "$s-$r"
        $rec = Join-Path $root "out/scenes/$s.arrec"
        # 64 passes after 8 burn-in, as bench.ps1 and PR 94's captures; the burn-in is perf_shares.py's BURN_IN.
        $cap = @('profile', '--trace=cuda', '--sample=none', '--cpuctxsw=none', '--force-overwrite=true', '-o', $base,
          $cli, 'bench', $rec, '--size', $Size, '--passes', '64', '--warmup', '8', '--device', 'cuda')
        Write-Host "nsys $($cap -join ' ')"
        $proc = Start-Process -FilePath $nsys -ArgumentList ($cap | ForEach-Object { '"' + $_ + '"' }) -NoNewWindow -PassThru `
          -RedirectStandardOutput "$base.capture.log" -RedirectStandardError "$base.capture.err"
        if (-not $proc.WaitForExit(300000)) {
          & taskkill /F /T /PID $proc.Id 2>&1 | Out-Null
          Write-Host "REFUSE perf: nsys capture of $s ran over 300 s and was killed (a hung bench?); log $base.capture.log"; exit 2
        }
        if ($proc.ExitCode -ne 0 -or -not (Test-Path "$base.nsys-rep")) {
          Get-Content "$base.capture.log", "$base.capture.err" -ErrorAction SilentlyContinue | Select-Object -Last 6 | ForEach-Object { Write-Host $_ }
          Write-Host "FAIL perf: nsys capture of $s exited $($proc.ExitCode)"; exit 1
        }
        Write-Host "nsys export --type sqlite $base.nsys-rep"
        $exp = & $nsys export --type sqlite --force-overwrite=true -o "$base.sqlite" "$base.nsys-rep" 2>&1 | ForEach-Object { "$_" }
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$base.sqlite")) { $exp | Select-Object -Last 4 | ForEach-Object { Write-Host $_ }; Write-Host "FAIL perf: nsys export of $s"; exit 1 }
        $exports += "$s=$base.sqlite"
      }
      # The id is the CLI's own, read from the bench lines of a minimal run (nsys keeps the target's stdout in the
      # report, not in the capture log): one source.
      $probeOut = & $cli bench $rec --size 320x180 --passes 1 --warmup 0 --device cuda 2>&1 | ForEach-Object { "$_" }
      $machine = ($probeOut | Where-Object { $_ -like '{*' } | Select-Object -First 1 | ConvertFrom-Json).machine
      if (-not $machine) { Write-Host 'FAIL perf: the CLI printed no bench line to read the machine id from'; exit 1 }
      $out = Join-Path $tmp "shares-$r.jsonl"
      & $py -I (Join-Path $PSScriptRoot 'perf_shares.py') lines @exports --sha $binSha --machine $machine --out $out
      if ($LASTEXITCODE -ne 0) { Write-Host "FAIL perf: perf_shares.py exited $LASTEXITCODE"; exit 2 }
      $newLines += , @(Get-Content $out)
      continue
    }
    if ($MeasureTolerance) { $lines = Invoke-PerfBench $r } else {
      $tm = Invoke-ArPerfCheck -Bench { Invoke-PerfBench $r } -Quiet { Assert-QuietGpu } -Compare {
        param($ln)
        $probe = Join-Path $tmp "probe-$r.jsonl"
        [IO.File]::WriteAllText($probe, ((@($ln) -join "`n") + "`n"))
        $o = @(& $py -I (Join-Path $PSScriptRoot 'perf.py') compare $probe 2>&1 | ForEach-Object { "$_" })
        @{ Code = $LASTEXITCODE; Out = $o }
      }
      $lines = $tm.Result
      $recordLoad += @($tm.DecidingLoad)
    }
    $newLines += , $lines
  }
} finally {
  Exit-ArMachineLock
}
$wall = $sw.Elapsed.TotalSeconds
if ($Record -and $recordLoad.Count) {
  Write-Host "REFUSE perf: -Record with foreign load seen during the run ($($recordLoad -join ', ')); a baseline needs a quiet machine"
  exit 2
}

$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmss')
$files = @()
for ($r = 0; $r -lt $newLines.Count; $r++) {
  $f = Join-Path $tmp "run-$stamp-$($r + 1).jsonl"
  [IO.File]::WriteAllText($f, ((@($newLines[$r]) -join "`n") + "`n"))
  $files += $f
}
if ($Shares) {
  $psh = Join-Path $PSScriptRoot 'perf_shares.py'
  if ($MeasureTolerance) {
    & $py -I $psh spread @files
    Write-Host ("wall {0:N1} s for {1} captures per scene" -f $wall, $newLines.Count)
    exit $LASTEXITCODE
  }
  $recArgs = @()
  if ($Record) { $recArgs = @('--record', (Join-Path $root "bench/history/$([DateTime]::UtcNow.Year).jsonl")) }
  & $py -I $psh compare $files[0] @recArgs
  $code = $LASTEXITCODE
  Write-Host ("perf shares wall {0:N1} s" -f $wall)
  exit $code
}
if ($MeasureTolerance) {
  & $py -I (Join-Path $PSScriptRoot 'perf.py') spread @files
  Write-Host ("wall {0:N1} s for {1} runs" -f $wall, $newLines.Count)
  exit $LASTEXITCODE
}

# -Record goes through perf.py so that only a PASS is written (a FAIL becomes a baseline only through a ruling
# naming old and new); the lines are compact, as bench writes them.
$rec = @()
if ($Record) { $rec = @('--record', (Join-Path $root "bench/history/$([DateTime]::UtcNow.Year).jsonl")) }
$cum = @()
if ($Cumulative) { $cum = @('--cumulative', $Cumulative) }
& $py -I (Join-Path $PSScriptRoot 'perf.py') compare $files[0] @rec @cum
$code = $LASTEXITCODE
Write-Host ("perf wall {0:N1} s{1}" -f $wall, $(if ($wall -gt 60) { ' (ABOVE 60 s: stop and ask the lead)' } else { '' }))
Write-Host 'shares are not checked here (pass_ms alone misses a longer idle share): run tools/perf.ps1 -Shares'
exit $code
