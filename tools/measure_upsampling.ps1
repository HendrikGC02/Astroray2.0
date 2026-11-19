# SPDX-License-Identifier: Apache-2.0
# One-off measurement of task 092 Do 3 (0003 Risks 2: upsampling cost in the shade kernel); deleted
# when the task lands. Kernel time per 1080p sample (astroray_cli bench, B-20's metric) with the
# Jakob-Hanika upsampling as implemented, against a local build that replaces it by the constant
# spectrum 0.5:
#   b1m:  the RGB branch of resolve() (material/slot_resolve.h), the sigmoid per constant slot,
#         which is all the upsampling b1m does on the device (its material folds to constants);
#   b_tx: op_rgb_lift (vm/ops/rgb_lift.h), the coefficient lookup and sigmoid per textured point,
#         which both RGB_LIFT and STORE_SLOT's colour-slot lift call.
# Each edit is undone with `git checkout` in a finally block and the tree rebuilt, so nothing stays.
# Usage: pwsh tools/measure_upsampling.ps1 [-Passes 64] [-Warmup 8] [-Repeats 3]
param([int]$Passes = 64, [int]$Warmup = 8, [int]$Repeats = 3)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
$cli = Join-Path $root 'out/bin/astroray_cli.exe'

function Build {
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') | Select-Object -Last 1 | ForEach-Object { Write-Host "  $_" }
  if ($LASTEXITCODE -ne 0) { throw 'build failed' }
}

# Median over -Repeats bench runs of kernel_ms_per_sample (each run is itself a median of -Passes).
function Bench([string]$rec) {
  $k = @(); $rays = 0
  $null = Enter-ArMachineLock
  try {
    for ($i = 0; $i -lt $Repeats; ++$i) {
      $lines = & $cli bench (Join-Path $root "out/scenes/$rec") --size 1920x1080 --passes $Passes --warmup $Warmup --device cuda 2>&1 | ForEach-Object { "$_" }
      if ($LASTEXITCODE -ne 0) { throw "bench $rec exited $LASTEXITCODE" }
      foreach ($ln in $lines | Where-Object { $_ -match '^\{' }) {
        $o = $ln | ConvertFrom-Json
        if ($o.metric -eq 'kernel_ms_per_sample') { $k += $o.value }
        if ($o.metric -eq 'rays_closest') { $rays = $o.value }
      }
    }
  } finally { Exit-ArMachineLock }
  $m = ($k | Sort-Object)[[int][math]::Floor($k.Count / 2)]
  return @{ k = $m; rays = $rays; runs = ($k | ForEach-Object { '{0:g5}' -f ($_ * 1e6) }) -join ' ' }
}

function Edit-Source([string]$file, [string]$old, [string]$new) {
  $p = Join-Path $root $file
  $t = [IO.File]::ReadAllText($p)
  if (-not $t.Contains($old)) { throw "$file no longer contains the line this measurement replaces" }
  [IO.File]::WriteAllText($p, $t.Replace($old, $new))
}

$py = Get-ArPython
& $py -I (Join-Path $PSScriptRoot 'scenes.py') b1m s3_textured
if ($LASTEXITCODE -ne 0) { throw 'scene recordings' }

$cases = @(
  @{ Scene = 'b1m'; Rec = 'b1m.arrec'; File = 'src/kernel/material/slot_resolve.h'
     Old = '    s = upsampled_eval(u, lam);'; New = '    s = spec_fill(Real(0.5)); u.scale = Real(0); if (u.scale != Real(0)) s = upsampled_eval(u, lam);' },
  @{ Scene = 'b_tx'; Rec = 's3_textured.arrec'; File = 'src/kernel/vm/ops/rgb_lift.h'
     Old = '  uint32_t flags = 0;'; New = "  (void)t; (void)rgb; (void)sem; (void)lam;`n  return spec_fill(Real(0.5));`n#if 0`n  uint32_t flags = 0;"
     Old2 = '  return illuminant(*t.rgb2spec, t.observer, c, lam, &flags);'
     New2 = "  return illuminant(*t.rgb2spec, t.observer, c, lam, &flags);`n#endif" })

Write-Host 'baseline build'
Build
$base = @{}
foreach ($c in $cases) { $base[$c.Scene] = Bench $c.Rec }
$res = @{}
foreach ($c in $cases) {
  Write-Host "constant-spectrum build: $($c.File)"
  try {
    Edit-Source $c.File $c.Old $c.New
    if ($c.Old2) { Edit-Source $c.File $c.Old2 $c.New2 }
    Build
    $res[$c.Scene] = Bench $c.Rec
  } finally {
    & git checkout -- $c.File
  }
}
Write-Host 'restoring build'
Build
foreach ($c in $cases) {
  $b = $base[$c.Scene]; $v = $res[$c.Scene]
  $share = ($b.k - $v.k) / $b.k
  $perRay = (($b.k / $b.rays) - ($v.k / $v.rays)) / ($b.k / $b.rays)
  Write-Host ("{0,-5} kernel ns/sample: as implemented {1:g5} [{2}], constant {3:g5} [{4}]; share {5:p1}; rays_closest {6} vs {7}, share per closest ray {8:p1}" -f `
      $c.Scene, ($b.k * 1e6), $b.runs, ($v.k * 1e6), $v.runs, $share, $b.rays, $v.rays, $perRay)
}
