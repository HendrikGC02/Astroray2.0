# NVRTC runtime compile/load/launch and OptiX module (PTX vs OptiX-IR) compile/load/launch.
param([int]$Reps = 3)
. $PSScriptRoot\env.ps1; . $PSScriptRoot\common.ps1
Set-Location $PSScriptRoot
$f = 'rtc_optix.txt'; Remove-Item "$PSScriptRoot\out\results\$f" -ErrorAction SilentlyContinue
$oi = "$OPTIX\include"
Log $f "# GPU snapshot before: $(Gpu-Snap)"
cl /nologo /O2 /EHsc /std:c++17 /MD -Iinclude "-I$env:CUDA_PATH\include" src\nvrtc_test.cpp /Fo:out\nvrtc_test.obj /Fe:out\nvrtc_test.exe /link /LIBPATH:"$env:CUDA_PATH\lib\x64" nvrtc.lib cuda.lib cudart.lib *> $null
Log $f "## NVRTC (nvrtc_test.exe $Reps) with driver JIT cache enabled (PTX load may hit the cache)"
.\out\nvrtc_test.exe $Reps 2>&1 | ForEach-Object { Log $f $_ }
Log $f "## NVRTC with CUDA_CACHE_DISABLE=1 (cold driver JIT for the PTX path)"
$env:CUDA_CACHE_DISABLE = '1'; .\out\nvrtc_test.exe $Reps 2>&1 | ForEach-Object { Log $f $_ }; Remove-Item env:CUDA_CACHE_DISABLE
Log $f "## OptiX input compile (nvcc, $Reps runs): nvcc -ptx|-optix-ir -arch=compute_120 -O3 -std=c++17 -Iinclude -Isrc -I<optix>/include src/optix_kernel.cu"
foreach ($m in @(@('ptx','-ptx'), @('optixir','-optix-ir'))) {
  $t = @(); for ($i = 0; $i -lt $Reps; ++$i) { $t += Time-Cmd { nvcc $m[1] -arch=compute_120 -O3 -std=c++17 -Iinclude -Isrc "-I$oi" src\optix_kernel.cu -o "out\optix_kernel.$($m[0])" } }
  $s = Min-Med $t; Log $f "nvcc $($m[1]): min $($s.min) s median $($s.median) s ($($t -join '/')), size $((Get-Item out\optix_kernel.$($m[0])).Length) bytes"
}
cl /nologo /O2 /EHsc /std:c++17 /MD -Iinclude -Isrc "-I$env:CUDA_PATH\include" "-I$oi" src\optix_test.cpp /Fo:out\optix_test.obj /Fe:out\optix_test.exe /link /LIBPATH:"$env:CUDA_PATH\lib\x64" cuda.lib cudart.lib advapi32.lib *> $null
Log $f "## OptiX module create / pipeline / launch, OptiX disk cache enabled (default)"
.\out\optix_test.exe out\optix_kernel.ptx out\optix_kernel.optixir $Reps 2>&1 | ForEach-Object { Log $f $_ }
Log $f "## same with optixDeviceContextSetCacheEnabled(ctx, 0) (cold compile)"
.\out\optix_test.exe out\optix_kernel.ptx out\optix_kernel.optixir $Reps --nocache 2>&1 | ForEach-Object { Log $f $_ }
Log $f "# GPU snapshot after: $(Gpu-Snap)"
