# SPDX-License-Identifier: Apache-2.0
# tools/perf.ps1 [-Scene b1m,b5m] [-Size 1920x1080] [-Record] [-MeasureTolerance [-Runs 5]]
# Pass-time regression check: runs bench.ps1 on b1m and b5m under one machine-lock hold and compares each
# `pass_ms` with the last bench/history line of the same machine and scene against B-38 (tools/budgets.toml).
# Refuses while another GPU user runs (blender, astroray_cli, astroray_tests_gpu, nsys outside this process
# tree): a shared GPU makes the number meaningless. It also refuses a binary that is dirty or built from other
# code than HEAD. The comparison and its decision table are tools/perf.py.
# -Record appends the measured pass_ms lines of a PASS to bench/history/<year>.jsonl (the lead's commit path);
# it refuses when build inputs are uncommitted.
# -MeasureTolerance runs the bench -Runs times back to back and prints the spreads that set B-38.
# It is not a `check` tier: tiers write the 20-line summary and out/check/<tier>.json that report.py reads and
# budget B-08..B-10 time; perf is one more caller of bench.ps1. Ref: 0009 d13 (B-38), d18.
# Output: at most 20 summary lines plus the nsys commands; exit 0 pass, 1 a scene failed, 2 refused or unusable.
param(
  [string[]]$Scene = @('b1m', 'b5m'),
  [string]$Size = '1920x1080',
  [switch]$Record,
  [switch]$MeasureTolerance,
  [int]$Runs = 5
)
$ErrorActionPreference = 'Stop'
$Scene = @($Scene | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
$py = Get-ArPython
$tmp = Join-Path $root 'out/tmp/140'
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

# The CLI stamps the computer name as `machine`; a history line carries a neutral hardware id instead
# (gpu, cores, threads), the one the first lines of bench/history use, so no host or user name is committed.
$gpu = ((Get-CimInstance Win32_VideoController | Where-Object { $_.Name -match 'NVIDIA' } | Select-Object -First 1).Name -replace '(?i)nvidia|geforce|\s', '').ToLower()
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$machine = "$gpu-$($cpu.NumberOfCores)c$($cpu.NumberOfLogicalProcessors)t"

$cli = Join-Path $root 'out/bin/astroray_cli.exe'
$null = Enter-ArMachineLock
$sw = [Diagnostics.Stopwatch]::StartNew()   # lock wait excluded
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
    $before = @{}
    $dir = Join-Path $root 'out/bench'
    if (Test-Path $dir) { Get-ChildItem $dir -Filter 'history-*.jsonl' | ForEach-Object { $before[$_.FullName] = @(Get-Content $_.FullName).Count } }
    $log = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'bench.ps1') -Scene ($Scene -join ',') -Size $Size 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { $log | Select-Object -Last 8 | ForEach-Object { Write-Host $_ }; Write-Host "FAIL perf: bench run $r exited $LASTEXITCODE"; exit 1 }
    $got = @()
    Get-ChildItem $dir -Filter 'history-*.jsonl' | ForEach-Object {
      $skip = if ($before.ContainsKey($_.FullName)) { $before[$_.FullName] } else { 0 }
      $got += @(Get-Content $_.FullName | Select-Object -Skip $skip)
    }
    $newLines += , @($got | ForEach-Object { $_ -replace '"machine":\s*"[^"]*"', ('"machine":"' + $machine + '"') })
  }
} finally {
  Exit-ArMachineLock
}
$wall = $sw.Elapsed.TotalSeconds

$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmss')
$files = @()
for ($r = 0; $r -lt $newLines.Count; $r++) {
  $f = Join-Path $tmp "run-$stamp-$($r + 1).jsonl"
  [IO.File]::WriteAllText($f, ((@($newLines[$r]) -join "`n") + "`n"))
  $files += $f
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
& $py -I (Join-Path $PSScriptRoot 'perf.py') compare $files[0] @rec
$code = $LASTEXITCODE
Write-Host ("perf wall {0:N1} s{1}" -f $wall, $(if ($wall -gt 60) { ' (ABOVE 60 s: stop and ask the lead)' } else { '' }))
Write-Host 'shares (not checked here; pass_ms alone misses a longer idle share): nsys profile --trace=cuda --sample=none --cpuctxsw=none -o out/tmp/perf out/bin/astroray_cli.exe bench out/scenes/b1m.arrec --size 1920x1080; nsys export --type sqlite out/tmp/perf.nsys-rep'
exit $code
