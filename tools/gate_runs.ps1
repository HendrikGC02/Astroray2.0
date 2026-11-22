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
$null = Enter-ArMachineLock
try {
  for ($i = 1; $i -le $Runs; $i++) {
    & pwsh -NoProfile -File tools/check.ps1 gate | Select-Object -Last 1 | ForEach-Object { Write-Host "run ${i}: $_" }
    Copy-Item (Join-Path $root 'out/check/gate.json') (Join-Path $dir "run-$i.json") -Force
  }
} finally { Exit-ArMachineLock }
& $py -I tools/gate_table.py --check tools/check.ps1 (1..$Runs | ForEach-Object { Join-Path $dir "run-$_.json" })
exit $LASTEXITCODE
