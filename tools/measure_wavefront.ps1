# SPDX-License-Identifier: Apache-2.0
# One-off for task 093 (deleted in the task's final commit): wavefront overhead and state bandwidth.
# tools/measure_wavefront.ps1 -Stage bench|nsys|ncu|blend|cycles|all
#   bench : tools/bench.ps1, b1m and b5m, 1080p, 8 burn-in + 64 passes
#   nsys  : Nsight Systems trace of the same run, analysed by out/land/093/analyze.py
#   ncu   : Nsight Compute dram bytes per k_ kernel of one pass (text output; --csv fails with "bad conversion")
#   blend : .blend files of both scenes (Cycles scenes, --no-extension)
#   cycles: Cycles OptiX per-sample time on those files (65 samples minus 1, over 64, as spike B)
# Every GPU run holds the machine lock for that run only.
param([string]$Stage = 'all')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
$env:PYTHONNOUSERSITE = '1'
$ev = Join-Path $root 'out/evidence/093'
$work = Join-Path $root 'out/land/093'
New-Item -ItemType Directory -Force $ev, $work | Out-Null
$cli = Join-Path $root 'out/bin/astroray_cli.exe'
$nsys = 'C:\Program Files\NVIDIA Corporation\Nsight Systems 2024.6.2\target-windows-x64\nsys.exe'
$ncu = 'C:\Program Files\NVIDIA Corporation\Nsight Compute 2025.1.0\target\windows-desktop-win7-x64\ncu.exe'
$scenes = 'b1m', 'b5m'
$size = '1920x1080'

function Run-Locked([scriptblock]$body) {
  $null = Enter-ArMachineLock
  try { & $body } finally { Exit-ArMachineLock }
}

if ($Stage -in 'bench', 'all') {
  pwsh -NoProfile -File (Join-Path $PSScriptRoot 'bench.ps1') -Passes 64 -Warmup 8 -Size $size
}
if ($Stage -in 'nsys', 'all') {
  foreach ($s in $scenes) {
    Run-Locked {
      & $nsys profile --trace=cuda --sample=none --cpuctxsw=none -o "$ev/$s" -f true $cli bench "out/scenes/$s.arrec" `
        --size $size --passes 64 --warmup 8 --device cuda 2>&1 | Where-Object { $_ -like '{*' -or $_ -like 'bench:*' } | Tee-Object "$ev/$s.nsys_run.txt"
    }
    & $nsys export --type sqlite -o "$ev/$s.sqlite" -f true "$ev/$s.nsys-rep" | Out-Null
    $png = if ($s -eq 'b1m') { "$ev/timeline_b1m.png" } else { '-' }
    # System Python only for the matplotlib plot; the analysis is stdlib.
    python -I (Join-Path $work 'analyze.py') "$ev/$s.sqlite" "$ev/$s.analysis.json" $png 8
  }
}
if ($Stage -in 'ncu', 'all') {
  foreach ($s in $scenes) {
    Run-Locked {
      & $ncu --metrics dram__bytes_read.sum,dram__bytes_write.sum,gpu__time_duration.sum --print-units base --kernel-name 'regex:^k_' `
        $cli bench "out/scenes/$s.arrec" --size $size --passes 1 --warmup 0 --device cuda 2>&1 | Out-File "$ev/$s.ncu.txt" -Encoding utf8
    }
    "ncu $s : $((Select-String -Path "$ev/$s.ncu.txt" -Pattern 'dram__bytes_read').Count) kernels profiled"
  }
}
if ($Stage -in 'blend', 'all') {
  $py = Get-ArPython
  foreach ($s in $scenes) {
    $desc = Join-Path $work "$s.blend"
    & $py -I "scenes/$s/build.py" --out (Join-Path $work "$s.arrec") --blend $desc
    $r = Invoke-ArBlender -Script 'scenes/lib/blend_writer.py' -Args '--no-extension', "$desc.json", $desc
    if ($r.ExitCode -ne 0) { throw "blend_writer $s exited $($r.ExitCode)" }
  }
}
if ($Stage -in 'cycles', 'all') {
  $script = Join-Path $work 'cycles_bench.py'
  @'
import sys, time, json, statistics
import bpy
blend, out = sys.argv[sys.argv.index("--") + 1:][:2]
bpy.ops.wm.open_mainfile(filepath=blend)
sc = bpy.context.scene
cp = bpy.context.preferences.addons["cycles"].preferences
cp.compute_device_type = "OPTIX"
cp.get_devices()
for d in cp.devices:
    d.use = d.type == "OPTIX"
sc.render.engine = "CYCLES"
sc.cycles.device = "GPU"
sc.cycles.use_denoising = False
sc.cycles.use_adaptive_sampling = False
sc.render.use_persistent_data = False
sc.render.resolution_x, sc.render.resolution_y, sc.render.resolution_percentage = 1920, 1080, 100
info = {"devices": [(d.name, d.type, d.use) for d in cp.devices], "max_bounces": sc.cycles.max_bounces,
        "original_samples": sc.cycles.samples, "filter_width": sc.cycles.filter_width}
def render(n):
    sc.cycles.samples = n
    t = time.perf_counter()
    bpy.ops.render.render()
    return (time.perf_counter() - t) * 1e3
render(1)  # cold: device init, kernel load
res = {"info": info, "t1": [], "t65": []}
for _ in range(3):
    res["t1"].append(render(1))
    res["t65"].append(render(65))
res["per_sample_ms"] = [(b - a) / 64 for a, b in zip(res["t1"], res["t65"])]
json.dump(res, open(out, "w"), indent=1)
print("CYCLES", json.dumps(res))
'@ | Set-Content $script -Encoding utf8
  foreach ($s in $scenes) {
    $r = Invoke-ArBlender -Script $script -Args (Join-Path $work "$s.blend"), "$ev/$s.cycles.json" -TimeoutS 900
    if ($r.ExitCode -ne 0) { throw "cycles $s exited $($r.ExitCode)" }
  }
}
