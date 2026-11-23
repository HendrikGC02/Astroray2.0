# SPDX-License-Identifier: Apache-2.0
# tools/perf.ps1 [-Scene b1m,b5m] [-Size 1920x1080] [-Record] [-MeasureTolerance [-Runs 5]]
# Pass-time regression check: runs bench.ps1 on b1m and b5m under one machine-lock hold and compares each
# `pass_ms` with the last bench/history line of the same machine and scene against B-38 (tools/budgets.toml).
# Refuses while another GPU user runs (blender, astroray_cli, astroray_tests_gpu, nsys outside this process
# tree): a shared GPU makes the number meaningless. The comparison and its decision table are tools/perf.py.
# -Record appends the measured pass_ms lines to bench/history/<year>.jsonl (the lead's commit path).
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

# Other GPU users: every process of these names that is not this process or one of its ancestors (land.ps1 and
# pwsh parents run this script; the bench children start after the check). nvidia-smi is not used: WDDM lists
# no compute processes reliably. Ref: PR 133 ("nothing else on the GPU, checked before each capture").
$ancestors = @($PID)
$p = $PID
for ($i = 0; $i -lt 16; $i++) {
  $ci = Get-CimInstance Win32_Process -Filter "ProcessId=$p" -ErrorAction SilentlyContinue
  if (-not $ci -or -not $ci.ParentProcessId) { break }
  $p = [int]$ci.ParentProcessId; $ancestors += $p
}
$others = @(Get-Process -Name blender, astroray_cli, astroray_tests_gpu, nsys -ErrorAction SilentlyContinue |
    Where-Object { $ancestors -notcontains $_.Id })
if ($others.Count) {
  Write-Host ("REFUSE perf: other GPU users running: " + (($others | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ') + '; wait for a quiet machine (tools/status.ps1)')
  exit 2
}

$null = Enter-ArMachineLock
$sw = [Diagnostics.Stopwatch]::StartNew()   # lock wait excluded
$newLines = @()   # per bench run: the history lines it appended
try {
  $n = if ($MeasureTolerance) { $Runs } else { 1 }
  for ($r = 1; $r -le $n; $r++) {
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
    $newLines += , $got
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

& $py -I (Join-Path $PSScriptRoot 'perf.py') compare $files[0]
$code = $LASTEXITCODE
if ($code -le 1 -and $Record) {
  # Only the pass_ms lines, after the comparison so a run is never its own baseline.
  $year = [DateTime]::UtcNow.Year
  $hist = Join-Path $root "bench/history/$year.jsonl"
  $add = @($newLines[0] | Where-Object { $_ -match '"metric":\s*"pass_ms"' })
  $text = if (Test-Path $hist) { [IO.File]::ReadAllText($hist) } else { '' }
  if ($text.Length -and -not $text.EndsWith("`n")) { $text += "`n" }
  [IO.File]::WriteAllText($hist, $text + (($add -join "`n") + "`n"))
  Write-Host "recorded $($add.Count) pass_ms lines in bench/history/$year.jsonl"
}
Write-Host ("perf wall {0:N1} s{1}" -f $wall, $(if ($wall -gt 60) { ' (ABOVE 60 s: stop and ask the lead)' } else { '' }))
Write-Host 'shares (not checked here; pass_ms alone misses a longer idle share): nsys profile --trace=cuda --sample=none --cpuctxsw=none -o out/tmp/perf out/bin/astroray_cli.exe bench out/scenes/b1m.arrec --size 1920x1080; nsys export --type sqlite out/tmp/perf.nsys-rep'
exit $code
