# SPDX-License-Identifier: Apache-2.0
# tools/detection_power.ps1 <row>  (one-off for task 078; deleted when 078 lands, 0009 d31)
# Detection power of the gate (0009 d15, Verification rows 14-15; research 09 section 3, spike 3):
# injects one known bug into the working tree, runs `tools/check.ps1 gate`, then, because the gate
# stops at its first failing step, every statistical and analytic suite on its own (CPU tests
# fast and gate, analytic scenes on CPU and CUDA, GPU gate tests), and reverts the bug with
# `git checkout -- <file>` whatever happens. Rows:
#   baseline  no bug (gate wall time before)
#   a  missing cosine in shade_surface's throughput update
#   b  wrong GGX VNDF Jacobian: the 1 / (4 wo.m) factor dropped from the density (pdf and sample)
#   c  1% albedo error in EON: rho * 1.01 in the BRDF value
#   d  flipped front face in the shading context (front = dot(Ng, wo) < 0)
#   e1 hero lane fixed at lane 0 where generate_camera stores it
#   e2 hero lane fixed at lane 0 in the sampler: dimension 6 (DIM_CAM_HERO) draws 0
#   m-gate      a temporary gate test that pushes the gate family past its m
#   m-analytic  [family_members] analytic_scenes raised past its m
# Logs: out/detection_power/<row>/*.log. Holds the machine lock for the whole row.
param([Parameter(Mandatory, Position = 0)][string]$Row)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')

# Each edit: file, the exact text, its replacement, which occurrence (1-based) and how many there must be.
function E($file, $old, $new, [int]$nth = 1, [int]$count = 1) { @{ File = $file; Old = $old; New = $new; Nth = $nth; Count = $count } }
$mTest = @'

// 078 temporary: one test beyond the gate family's m.
TEST_SUITE("gate") {
  TEST_CASE("detection_power/beyond m") {
    const st::Run run = st::begin("detection_power/beyond m");
    const std::vector<double> w(16, 1.0);
    for (int i = 0; i <= run.family->m(); ++i) CHECK(st::energy_test(run, w, 1.0).pass);
  }
}
'@
$bugs = @{
  'baseline' = @()
  'a' = @(E 'src/kernel/stages/shade_surface.h' 'Spec beta_next = beta * (bs.f * (abs_dot(bs.wi, N) / bs.pdf));' 'Spec beta_next = beta * (bs.f * (Real(1) / bs.pdf));')
  'b' = @(
    (E 'src/kernel/material/lobes/ggx.h' 'const Real pdf = ggx_g1(alpha, wo) * ggx_d(alpha, m) / (Real(4) * wo.z);' 'const Real pdf = ggx_g1(alpha, wo) * ggx_d(alpha, m) * dot(wo, m) / wo.z;'),
    (E 'src/kernel/material/lobes/ggx.h' 'const Real pdf = ggx_g1(alpha, wo) * d / (Real(4) * wo.z);' 'const Real pdf = ggx_g1(alpha, wo) * d * cos_o_m / wo.z;'))
  'c' = @(E 'src/kernel/material/lobes/eon.h' 'const Real rho = p.base_color.v[i];' 'const Real rho = p.base_color.v[i] * Real(1.01);' 2 2)
  'd' = @(E 'src/kernel/stages/shade_common.h' 'const bool front = dot(ng, ctx->wo) > Real(0);' 'const bool front = dot(ng, ctx->wo) < Real(0);')
  'e1' = @(E 'src/kernel/stages/generate_camera.h' 'uint32_t hero = uint32_t(camera_draw(kg, key, DIM_CAM_HERO) * Real(4));' 'uint32_t hero = 0u * uint32_t(camera_draw(kg, key, DIM_CAM_HERO) * Real(4));')
  'e2' = @(E 'src/kernel/sample/sobol.h' '  const uint32_t block = d >> 2;' "  if (d == 6u) return 0u;`n  const uint32_t block = d >> 2;")
  'm-gate' = @(E 'tests/cpu/test_stat_harness.cpp' $null $mTest)
  'm-analytic' = @(E 'tools/budgets.toml' 'analytic_scenes = 22' 'analytic_scenes = 257')
}
if (-not $bugs.ContainsKey($Row)) { throw "unknown row '$Row' ($($bugs.Keys -join ', '))" }
$edits = $bugs[$Row]
$files = @($edits | ForEach-Object { $_.File } | Sort-Object -Unique)
foreach ($f in $files) {
  if (& git status --porcelain -- $f) { throw "$f has local changes; the row must start from a clean file" }
}
$dir = Join-Path $root "out/detection_power/$Row"
New-Item -ItemType Directory -Force $dir | Out-Null
$py = Get-ArPython
$null = Enter-ArMachineLock
try {
  foreach ($e in $edits) {
    $p = Join-Path $root $e.File
    $t = [IO.File]::ReadAllText($p)
    if ($null -eq $e.Old) { $t += $e.New }
    else {
      $at = @(); $i = $t.IndexOf($e.Old)
      while ($i -ge 0) { $at += $i; $i = $t.IndexOf($e.Old, $i + 1) }
      if ($at.Count -ne $e.Count) { throw "$($e.File): '$($e.Old)' found $($at.Count) times, expected $($e.Count)" }
      $k = $at[$e.Nth - 1]
      $t = $t.Substring(0, $k) + $e.New + $t.Substring($k + $e.Old.Length)
    }
    [IO.File]::WriteAllText($p, $t)
  }
  Write-Host "row $Row`: $(@($edits).Count) edit(s) in $($files -join ', ')"
  & git diff --stat | Write-Host

  # Build first, so the gate's B-08/B-09 clocks see a warm tree, as on an ordinary second run, and
  # a failure is the bug's, not the rebuild's.
  $b = & pwsh -NoProfile -File tools/build.ps1 -Target all,astroray_kernels_gpu_nofma 2>&1 | ForEach-Object { "$_" }
  $b | Set-Content (Join-Path $dir 'build.log')
  if ($LASTEXITCODE -ne 0) { $b | Select-Object -Last 5 | ForEach-Object { Write-Host $_ }; throw 'build failed' }
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $g =& pwsh -NoProfile -File tools/check.ps1 gate 2>&1 | ForEach-Object { "$_" }
  $gc = $LASTEXITCODE
  $g | Set-Content (Join-Path $dir 'gate.log')
  Write-Host "== check gate: exit $gc, wall $([math]::Round($sw.Elapsed.TotalSeconds, 1)) s"
  $g | Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }
  if ($Row -eq 'baseline' -or $Row -like 'm-*') { return }

  # The gate stops at its first failure: run each suite to the end.
  $suites = [ordered]@{
    'cpu-fast' = { & (Join-Path $root 'out/bin/astroray_tests_cpu.exe') --tier=fast }
    'cpu-gate' = { & (Join-Path $root 'out/bin/astroray_tests_cpu.exe') --tier=gate }
    'analytic-cpu' = { & $py -I tools/analytic.py --gate --device cpu }
    'analytic-cuda' = { & $py -I tools/analytic.py --gate --device cuda }
    'gpu-gate' = { & (Join-Path $root 'out/bin/astroray_tests_gpu.exe') --tier=gate }
  }
  foreach ($n in $suites.Keys) {
    $o = & $suites[$n] 2>&1 | ForEach-Object { "$_" }
    $c = $LASTEXITCODE
    $o | Set-Content (Join-Path $dir "$n.log")
    Write-Host "== $n`: exit $c"
    $o | Where-Object { $_ -match '^TEST CASE:|^\[doctest\] test cases|^(PASS|FAIL) analytic|: FAIL |^family ' } |
      Select-Object -First 30 | ForEach-Object { Write-Host "  $_" }
  }
} finally {
  & git checkout -- @files
  Exit-ArMachineLock
  Write-Host "reverted: $($files -join ', ')"
}
