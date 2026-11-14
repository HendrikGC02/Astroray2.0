# Does __launch_bounds__ still change registers under relocatable device code (-rdc), as used by the old engine?
. $PSScriptRoot\env.ps1; . $PSScriptRoot\common.ps1; . $PSScriptRoot\variants.ps1
Set-Location $PSScriptRoot
New-Item -ItemType Directory -Force out\rdc | Out-Null
$f = 'rdc_probe.txt'; Remove-Item "$PSScriptRoot\out\results\$f" -ErrorAction SilentlyContinue
foreach ($n in 'base', 'lb', 'noinl', 'noinl_lb') {
  $v = $variants | Where-Object { $_.n -eq $n }
  $obj = "out\rdc\${n}_fused.obj"
  $lg = nvcc -rdc=true -gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -Xcompiler /MD -Iinclude -Isrc (Flags $v).Split(' ') -Xptxas -v -c src\k_fused.cu -o $obj 2>&1 | Out-String
  $r = [regex]::Match($lg, 'Used (\d+) registers'); $st = [regex]::Match($lg, '(\d+) bytes stack frame, (\d+) bytes spill stores, (\d+) bytes spill loads')
  Log $f "-rdc=true $n : ptxas -v regs $($r.Groups[1].Value) stack $($st.Groups[1].Value) spill_st $($st.Groups[2].Value) spill_ld $($st.Groups[3].Value) (compile-time counts; device-linked counts not measured)"
}
