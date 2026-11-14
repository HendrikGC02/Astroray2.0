# Reproduces every number in RESULTS.md. Runs everything serially (one GPU job at a time). Output: out/results/*.txt|csv
# Usage: powershell -File run_all.ps1            (about 20-25 minutes)
$ErrorActionPreference = 'Continue'
Set-Location $PSScriptRoot
. $PSScriptRoot\env.ps1; . $PSScriptRoot\common.ps1
Remove-Item "$PSScriptRoot\out\results" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$PSScriptRoot\out\results" | Out-Null
$v = 'versions.txt'
Log $v "date: $(Get-Date -Format s)"
Log $v "nvcc: $((nvcc --version | Select-String 'release') -join '')"
Log $v "ptxas: $((ptxas --version | Select-String 'release') -join '')"
Log $v "cl: $((cl 2>&1 | Select-Object -First 1) -join '')"
Log $v "ninja $(ninja --version), $((cmake --version | Select-Object -First 1))"
Log $v "gpu: $((nvidia-smi --query-gpu=name,driver_version,compute_cap,clocks.max.sm --format=csv,noheader) -join '')"
Log $v "cuda path: $env:CUDA_PATH ; nvrtc dll: $((Get-ChildItem "$env:CUDA_PATH\bin\nvrtc64*.dll" | ForEach-Object Name) -join ', ')"
Log $v "optix sdk: $OPTIX"
Set-Content "$env:TEMP\a_probe.cu" '__global__ void k(float*a){a[threadIdx.x]=sinf(a[0]);}'
$probe = nvcc -arch=sm_120 -Xptxas -v -c "$env:TEMP\a_probe.cu" -o "$env:TEMP\a_probe.obj" 2>&1 | Out-String
Log $v "nvcc -arch=sm_120 with this MSVC: exit=$LASTEXITCODE; $((($probe -split "`n") | Select-String 'Compiling entry function' | Select-Object -First 1).Line.Trim())"
Log $v "gpu snapshot: $(Gpu-Snap)"
Log $v "other GPU clients (nvidia-smi compute-apps): $((nvidia-smi --query-compute-apps=pid,process_name --format=csv,noheader) -join '; ')"
.\gen_ninja.ps1
ninja -t clean *> $null
Log $v "initial full build -j8: $(Time-Cmd { ninja -j8 out/bench.exe }) s"
foreach ($s in "measure_regs.ps1") { powershell -NoProfile -ExecutionPolicy Bypass -File ".\$s" *> $null }
powershell -NoProfile -ExecutionPolicy Bypass -File .\measure_runtime.ps1 -Runs 3
powershell -NoProfile -ExecutionPolicy Bypass -File .\measure_agree.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\measure_rtc_optix.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\rdc_probe.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\measure_compile.ps1
Write-Host "done; see out/results"
