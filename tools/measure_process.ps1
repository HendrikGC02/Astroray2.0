# SPDX-License-Identifier: Apache-2.0
# One-off for task 091 (deleted when it lands). Modes:
#   build     clean out/build, run tools/build.ps1 with a 100 ms sampler of nvcc/cicc/ptxas/cudafe++/cl
#             working sets and of cmake.exe lifetimes (configure time); writes scratchpad json.
#   configure delete CMakeCache.txt + CMakeFiles + build.ninja, rebuild, record cmake.exe lifetime.
#   blender   10 empty headless Invoke-ArBlender runs.
#   tier      N consecutive `check.ps1 <Tier>` runs; records seconds, waited, JSON, landing overlap.
param(
  [Parameter(Mandatory)][ValidateSet('build', 'configure', 'blender', 'tier')][string]$Mode,
  [string]$Tier = 'fast', [int]$N = 5, [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
if (-not $Out) { $Out = 'C:/Astroray2.0/scratchpad/t091' }
New-Item -ItemType Directory -Force $Out | Out-Null

function Landing { @(Get-CimInstance Win32_Process -Filter "Name='pwsh.exe'" | Where-Object { $_.CommandLine -match 'land\.ps1' }).Count }

function Sample([string]$what, [scriptblock]$run) {
  $names = 'nvcc', 'cicc', 'ptxas', 'cudafe++', 'cl', 'cmake'
  $peak = @{}; $peakAll = 0.0; $cm = @{}; $maxProcs = @{}; $peakSumNvccCicc = 0.0
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process pwsh -ArgumentList (@('-NoProfile', '-File') + $what) -PassThru -NoNewWindow
  $keep = @{}
  while (-not $p.HasExited) {
    $ps = @(Get-Process -Name $names -ErrorAction SilentlyContinue)
    $sum = 0.0; $sumNC = 0.0; $cnt = @{}
    foreach ($q in $ps) {
      try {
        $ws = $q.WorkingSet64 / 1MB
        $sum += $ws
        if ($q.Name -in 'nvcc', 'cicc') { $sumNC += $ws }
        $cnt[$q.Name] = 1 + [int]$cnt[$q.Name]
        if ($q.Name -eq 'cmake') { $keep[$q.Id] = $q }
        if (-not $peak[$q.Name] -or $q.PeakWorkingSet64 / 1MB -gt $peak[$q.Name]) { $peak[$q.Name] = $q.PeakWorkingSet64 / 1MB }
      } catch { }
    }
    foreach ($k in $cnt.Keys) { if ($cnt[$k] -gt [int]$maxProcs[$k]) { $maxProcs[$k] = $cnt[$k] } }
    if ($sum -gt $peakAll) { $peakAll = $sum }
    if ($sumNC -gt $peakSumNvccCicc) { $peakSumNvccCicc = $sumNC }
    Start-Sleep -Milliseconds 100
  }
  $p.WaitForExit()
  $cmk = @()
  foreach ($k in $keep.Keys) { try { $cmk += [pscustomobject]@{ id = $k; s = [math]::Round(($keep[$k].ExitTime - $keep[$k].StartTime).TotalSeconds, 2) } } catch { } }
  return [ordered]@{
    exit = $p.ExitCode; wall_s = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    peak_nvcc_plus_cicc_ws_mb = [math]::Round($peakSumNvccCicc); peak_all_compilers_ws_mb = [math]::Round($peakAll)
    max_concurrent = $maxProcs; per_process_peak_mb = $peak; cmake_lifetimes = $cmk
    landing_overlap = (Landing)
  }
}

switch ($Mode) {
  'build' {
    if (Test-Path out/build) { Remove-Item -Recurse -Force out/build }
    $r = Sample @('tools/build.ps1') {}
    $r | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out 'build.json'); $r | ConvertTo-Json -Depth 5
  }
  'configure' {
    foreach ($f in 'CMakeCache.txt', 'build.ninja', 'CMakeFiles') { Remove-Item -Recurse -Force (Join-Path out/build $f) -ErrorAction SilentlyContinue }
    $r = Sample @('tools/build.ps1') {}
    $r | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out ("configure-" + (Get-Date -Format HHmmss) + '.json')); $r | ConvertTo-Json -Depth 5
  }
  'blender' {
    . (Join-Path $root 'tools/lib/lock.ps1'); . (Join-Path $root 'tools/lib/proc.ps1'); . (Join-Path $root 'tools/lib/blender.ps1')
    $empty = Join-Path $Out 'empty.py'; Set-Content $empty 'pass'
    $rows = @()
    for ($i = 0; $i -lt $N; $i++) {
      $ov = Landing
      $sw = [Diagnostics.Stopwatch]::StartNew()
      $r = Invoke-ArBlender -Script $empty -TimeoutS 120 | Select-Object -Last 1
      $t = $sw.Elapsed.TotalSeconds - $r.Waited
      $rows += [pscustomobject]@{ run = $i; s = [math]::Round($t, 2); waited = [math]::Round($r.Waited, 2); exit = $r.ExitCode; landing = $ov }
    }
    $rows | ConvertTo-Json | Set-Content (Join-Path $Out 'blender.json'); $rows | Format-Table | Out-String | Write-Host
  }
  'tier' {
    $rows = @()
    for ($i = 0; $i -lt $N; $i++) {
      $t0 = Get-Date; $ov0 = Landing
      & pwsh -NoProfile -File tools/check.ps1 $Tier | Select-Object -Last 2 | Write-Host
      $t1 = Get-Date
      $j = Get-Content out/check/$Tier.json -Raw | ConvertFrom-Json
      $rows += [pscustomobject]@{ run = $i; seconds = $j.seconds; waited = $j.waited; pass = $j.pass; start = $t0.ToString('o'); end = $t1.ToString('o'); landing_at_start = $ov0; landing_at_end = (Landing); steps = ($j.steps | ForEach-Object { "$($_.name)=$($_.seconds)" }) -join '; ' }
    }
    $rows | ConvertTo-Json | Set-Content (Join-Path $Out "tier-$Tier-$(Get-Date -Format HHmmss).json"); $rows | Select-Object run, seconds, waited, pass, landing_at_start, landing_at_end | Format-Table | Out-String | Write-Host
  }
}
