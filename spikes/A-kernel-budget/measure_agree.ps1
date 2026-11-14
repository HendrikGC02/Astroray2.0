# CPU/GPU agreement on 1e6 inputs: GPU float (-fmad=true and -fmad=false) vs CPU float vs CPU double, plus CPU throughput.
. $PSScriptRoot\env.ps1; . $PSScriptRoot\common.ps1
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force out\agree | Out-Null
$f = 'agreement.txt'; Remove-Item "$PSScriptRoot\out\results\$f" -ErrorAction SilentlyContinue
function Run($l) { Log $f "## $l" }
cl /nologo /O2 /EHsc /std:c++17 /MD -Iinclude src\cmp.cpp /Fo:out\agree\cmp.obj /Fe:out\agree\cmp.exe *> $null
cl /nologo /O2 /EHsc /std:c++17 /MD -Iinclude src\cpu_main.cpp /Fo:out\agree\cpu_f32.obj /Fe:out\agree\cpu_f32.exe *> $null
cl /nologo /O2 /EHsc /std:c++17 /MD /DKL_REAL_DOUBLE -Iinclude src\cpu_main.cpp /Fo:out\agree\cpu_f64.obj /Fe:out\agree\cpu_f64.exe *> $null
cl /nologo /O2 /EHsc /std:c++17 /MD /arch:AVX2 /fp:contract -Iinclude src\cpu_main.cpp /Fo:out\agree\cpu_f32_fma.obj /Fe:out\agree\cpu_f32_fma.exe *> $null
nvcc -gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -Xcompiler /MD -Iinclude src\agree_gpu.cu -o out\agree\gpu_fma.exe *> $null
nvcc -gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -fmad=false -Xcompiler /MD -Iinclude src\agree_gpu.cu -o out\agree\gpu_nofma.exe *> $null
Run "builds: cpu_f32 = cl /O2; cpu_f64 = cl /O2 /DKL_REAL_DOUBLE; cpu_f32_fma = cl /O2 /arch:AVX2 /fp:contract; gpu_fma = nvcc -O3 (default -fmad=true); gpu_nofma = nvcc -O3 -fmad=false"
foreach ($e in 'cpu_f32', 'cpu_f64', 'cpu_f32_fma') {
  Run "run $e (3 timing passes; dumps from the last)"
  for ($i = 0; $i -lt 3; ++$i) { & ".\out\agree\$e.exe" "out\agree\$e.bin" 1000000 --time | ForEach-Object { Log $f "$e,$_" } }
}
foreach ($e in 'gpu_fma', 'gpu_nofma') { Run "run $e"; & ".\out\agree\$e.exe" "out\agree\$e.bin" 1000000 | ForEach-Object { Log $f "$e,$_" } }
$pairs = @(@('gpu_fma','cpu_f32'), @('gpu_nofma','cpu_f32'), @('gpu_fma','cpu_f32_fma'), @('cpu_f32','cpu_f64'), @('cpu_f32_fma','cpu_f64'), @('gpu_fma','cpu_f64'), @('gpu_nofma','cpu_f64'))
foreach ($p in $pairs) {
  Run "cmp $($p[0]) vs $($p[1])  (reference = second)"
  & .\out\agree\cmp.exe "out\agree\$($p[0]).bin" "out\agree\$($p[1]).bin" | ForEach-Object { Log $f $_ }
}
