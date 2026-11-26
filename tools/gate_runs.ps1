# SPDX-License-Identifier: Apache-2.0
# tools/gate_runs.ps1 [-Runs 5]: `check gate` n times under ONE machine lock (nested, so no other
# GPU process can start between runs), each run's out/check/gate.json kept as out/tmp/157/run-N.json,
# then the steps-by-runs table of tools/gate_table.py. Holds the lock for the whole batch (minutes).
param([int]$Runs = 5)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
$py = Get-ArPython
$dir = Join-Path $root 'out/tmp/157'
New-Item -ItemType Directory -Force $dir | Out-Null
Get-ChildItem $dir -Filter 'run-*.json' | Remove-Item
$null = Enter-ArMachineLock -Job gate_runs
$bad = 0
$kept = @()
try {
  for ($i = 1; $i -le $Runs; $i++) {
    # No stale gate.json can stand in for a run that died before writing its own.
    $gj = Join-Path $root 'out/check/gate.json'
    Remove-Item $gj -ErrorAction SilentlyContinue
    $o = @(& pwsh -NoProfile -File tools/check.ps1 gate)
    $code = $LASTEXITCODE
    Write-Host "run ${i}: exit $code, $($o | Select-Object -Last 1)"
    if ($code -ne 0) { $bad++ }
    if (Test-Path $gj) { $dst = Join-Path $dir "run-$i.json"; Copy-Item $gj $dst -Force; $kept += $dst }
    else { $bad++ }
  }
} finally { Exit-ArMachineLock }
if ($kept.Count -gt 0) { & $py -I tools/gate_table.py --check tools/check.ps1 $kept }
if ($bad -gt 0) { Write-Host "FAIL gate_runs: $bad of $Runs runs failed or left no gate.json"; exit 1 }
exit $LASTEXITCODE
