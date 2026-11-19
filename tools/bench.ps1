# SPDX-License-Identifier: Apache-2.0
# tools/bench.ps1 [-Set M1a] [-Scene b1m,b5m] [-Size 1920x1080] [-Passes 64] [-Warmup 8] [-Device cuda]
# Runs `astroray_cli bench` on each bench scene of bench/scenes.toml (the scenes whose `sets` list
# contains -Set; burn-in of -Warmup passes, then -Passes passes) under the machine lock, appends the
# history lines to out/bench/history-<yyyyMMdd>.jsonl with the append `date` that lint requires
# (Ruling 71; metadata, not a measurement) and prints a summary. The lead commits history lines to
# bench/history/ from `full` runs only. Ref: 0009 d22, d23.
# Output: one summary line per scene and metric; non-zero exit when any run fails.
param(
  [string]$Set = 'M1a',
  [string[]]$Scene = @(),
  [string]$Size = '1920x1080',
  [int]$Passes = 64,
  [int]$Warmup = 8,
  [string]$Device = 'cuda'
)
$ErrorActionPreference = 'Stop'
# `pwsh -File bench.ps1 -Scene b1m,b5m` binds the list as the one string "b1m,b5m": split it.
$Scene = @($Scene | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')

# bench/scenes.toml is a flat list of [[scene]] tables; pwsh has no TOML reader, so read the three keys.
function Read-BenchScenes([string]$path) {
  $out = @(); $cur = $null
  foreach ($ln in Get-Content $path) {
    if ($ln -match '^\s*\[\[scene\]\]') { if ($cur) { $out += $cur }; $cur = @{ name = ''; recording = ''; sets = @() }; continue }
    if (-not $cur) { continue }
    if ($ln -match '^\s*name\s*=\s*"([^"]+)"') { $cur.name = $Matches[1] }
    elseif ($ln -match '^\s*recording\s*=\s*"([^"]+)"') { $cur.recording = $Matches[1] }
    elseif ($ln -match '^\s*sets\s*=\s*\[(.*)\]') { $cur.sets = @([regex]::Matches($Matches[1], '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value }) }
  }
  if ($cur) { $out += $cur }
  return $out
}

$scenes = @(Read-BenchScenes (Join-Path $root 'bench/scenes.toml') | Where-Object {
    ($Scene.Count -gt 0 -and $Scene -contains $_.name) -or ($Scene.Count -eq 0 -and $_.sets -contains $Set) })
if ($scenes.Count -eq 0) { Write-Host "FAIL bench: no scene in set '$Set' (scenes: $($Scene -join ','))"; exit 2 }
$cli = Join-Path $root 'out/bin/astroray_cli.exe'
if (-not (Test-Path $cli)) { Write-Host "FAIL bench: $cli is missing; run tools/build.ps1 first"; exit 2 }

$py = Get-ArPython
& $py -I (Join-Path $PSScriptRoot 'scenes.py') @($scenes | ForEach-Object { $_.name })
if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL bench: scene recordings'; exit 1 }

$null = Enter-ArMachineLock
$failed = $false
try {
  $dir = Join-Path $root 'out/bench'
  New-Item -ItemType Directory -Force $dir | Out-Null
  $file = Join-Path $dir ("history-" + [DateTime]::UtcNow.ToString('yyyyMMdd') + '.jsonl')
  foreach ($s in $scenes) {
    $rec = Join-Path $root ("out/scenes/" + $s.recording)
    $lines = & $cli bench $rec --size $Size --passes $Passes --warmup $Warmup --device $Device 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
      Write-Host "FAIL bench: $($s.name) exited $LASTEXITCODE"; $lines | Select-Object -Last 8 | ForEach-Object { Write-Host $_ }
      $failed = $true; continue
    }
    foreach ($ln in $lines) {
      if ($ln -notmatch '^\{') { continue }
      $o = $ln | ConvertFrom-Json -AsHashtable
      $o['date'] = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
      Add-Content -Path $file -Value ($o | ConvertTo-Json -Compress)
      Write-Host ("{0,-6} {1,-22} {2,14:g6} {3} (n={4}, spread {5:g4})" -f $o.scene, $o.metric, $o.value, $o.unit, $o.n, $o.spread)
    }
  }
} finally {
  Exit-ArMachineLock
}
if ($failed) { exit 1 }
Write-Host "PASS bench: history lines in $file"
