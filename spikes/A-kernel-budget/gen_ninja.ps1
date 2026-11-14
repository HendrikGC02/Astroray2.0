# Generates build.ninja + src/variants.inc. Variants: name, width, launch-bounds minBlocks (0 = none), noinline lobes, material by pointer.
param([string]$Arch = 'gencode')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$cuda = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8'
. $PSScriptRoot\variants.ps1
if ($Arch -eq 'gencode') { $archFlag = '-gencode arch=compute_120,code=sm_120' } else { $archFlag = '-arch=sm_120' }

$hdrs = 'include/kl.h include/material.h include/lobes.h include/stages.h include/gpu_scene.h src/kcommon.h'
$sb = New-Object System.Text.StringBuilder
function L([string]$s) { [void]$sb.AppendLine($s) }
L "cudalib = $cuda/lib/x64"
L "cudainc = $cuda/include"
L "rule nvcc"
L "  command = cmd /c `"nvcc $archFlag -O3 -std=c++17 -Xcompiler /MD -Iinclude -Isrc `$flags -Xptxas -v -c `$in -o `$out > `$out.log 2>&1`""
L "  description = NVCC `$out"
L "rule cl"
L "  command = cl /nologo /O2 /EHsc /std:c++17 /MD -Iinclude -I`"`$cudainc`" -c `$in /Fo`$out"
L "  description = CL `$out"
L "rule link"
L "  command = link /nologo /OUT:`$out `$in /LIBPATH:`"`$cudalib`" cudart.lib"
L "  description = LINK `$out"
$objs = @()
function X($list) { ($list | ForEach-Object { "X($($_.n),$($_.w))" }) -join ' ' }
$inc = "#define VARIANT_LIST(X) $(X $variants)`r`n#define PARAM_LIST(X) $(X $paramVariants)`r`n#define FVM_LIST(X) $(X $fvmVariants)`r`n"
Set-Content -Path "$root\src\variants.inc" -Value $inc -NoNewline
foreach ($v in $variants) {
  foreach ($k in 'fused','bsdf','nee') {
    $o = "out/obj/$($v.n)_$k.obj"; $objs += $o
    L "build ${o}: nvcc src/k_$k.cu | $hdrs"
    L "  flags = $(Flags $v)"
  }
}
foreach ($kind in @(@{list=$paramVariants; k='param'}, @{list=$fvmVariants; k='fvm'})) {
  foreach ($v in $kind.list) {
    $vv = @{n=$v.n; w=$v.w; lb=0; ni=0; ptr=0}
    $o = "out/obj/$($v.n)_$($kind.k).obj"; $objs += $o
    L "build ${o}: nvcc src/k_$($kind.k).cu | $hdrs"
    L "  flags = $(Flags $vv)"
  }
}
L "build out/obj/bench.obj: cl src/bench.cpp | $hdrs include/scene.h include/gpu_upload.h src/variants.inc"
L "build out/bench.exe: link out/obj/bench.obj $($objs -join ' ')"
L "build kernels: phony $($objs -join ' ')"
L "default out/bench.exe"
Set-Content -Path "$root\build.ninja" -Value $sb.ToString() -Encoding ascii
