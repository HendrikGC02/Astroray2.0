# Compile wall-clock: per TU (cold, 3 runs, serial), full ninja build -j8 vs -j1, header touch, .cu touch, nvcc option sweep, CPU build.
param([int]$Reps = 3)
. $PSScriptRoot\env.ps1; . $PSScriptRoot\common.ps1; . $PSScriptRoot\variants.ps1
Set-Location $PSScriptRoot
$out = 'compile_times.txt'; Remove-Item "$PSScriptRoot\out\results\$out" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force out\tmp | Out-Null
Log $out "# GPU snapshot before: $(Gpu-Snap)"
Log $out "## per-TU compile time (s), serial nvcc, $Reps runs each. cmd: nvcc -gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -Xcompiler /MD -Iinclude -Isrc <variant flags> -Xptxas -v -c <src> -o <obj>"
Log $out "unit,min_s,median_s,runs"
$units = Get-Units; $tot = 0
foreach ($u in $units) {
  $t = @(); for ($i = 0; $i -lt $Reps; ++$i) { $t += Time-Cmd { Invoke-Expression "nvcc -gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -Xcompiler /MD -Iinclude -Isrc $($u.flags) -Xptxas -v -c $($u.src) -o out\tmp\x.obj" } }
  $s = Min-Med $t; $tot += $s.min
  Log $out "$($u.name),$($s.min),$($s.median),$($t -join '/')"
}
Log $out "sum of per-TU minima: $([math]::Round($tot,2)) s over $($units.Count) TUs"
Log $out "## full build (bench.exe: $($units.Count) nvcc TUs + 1 cl + link), clean each time. ninja -j8 vs ninja -j1"
foreach ($j in 8, 1) {
  $t = @(); for ($i = 0; $i -lt $Reps; ++$i) { ninja -t clean *> $null; $t += Time-Cmd { ninja "-j$j" out/bench.exe } }
  $s = Min-Med $t; Log $out "full build -j$j,$($s.min),$($s.median),$($t -join '/')"
}
Log $out "## incremental (build is current before each)"
ninja out/bench.exe *> $null
$t = @(); for ($i = 0; $i -lt $Reps; ++$i) { (Get-Item include\lobes.h).LastWriteTime = Get-Date; $t += Time-Cmd { ninja -j8 out/bench.exe } }
$s = Min-Med $t; Log $out "touch include/lobes.h (all TUs rebuild), ninja -j8,$($s.min),$($s.median),$($t -join '/')"
$t = @(); for ($i = 0; $i -lt $Reps; ++$i) { (Get-Item include\lobes.h).LastWriteTime = Get-Date; $t += Time-Cmd { ninja -j1 out/bench.exe } }
$s = Min-Med $t; Log $out "touch include/lobes.h, ninja -j1,$($s.min),$($s.median),$($t -join '/')"
$t = @(); for ($i = 0; $i -lt $Reps; ++$i) { (Get-Item src\k_nee.cu).LastWriteTime = Get-Date; $t += Time-Cmd { ninja -j8 out/bench.exe } }
$s = Min-Med $t; Log $out "touch src/k_nee.cu (10 variant TUs + link), ninja -j8,$($s.min),$($s.median),$($t -join '/')"
$t = @(); for ($i = 0; $i -lt $Reps; ++$i) { (Get-Item src\k_param.cu).LastWriteTime = Get-Date; $t += Time-Cmd { ninja -j8 out/bench.exe } }
$s = Min-Med $t; Log $out "touch src/k_param.cu (2 TUs + link), ninja -j8,$($s.min),$($s.median),$($t -join '/')"
$t = @(); for ($i = 0; $i -lt $Reps; ++$i) { (Get-Item src\bench.cpp).LastWriteTime = Get-Date; $t += Time-Cmd { ninja -j8 out/bench.exe } }
$s = Min-Med $t; Log $out "touch src/bench.cpp (cl + link only),$($s.min),$($s.median),$($t -join '/')"
ninja out/bench.exe *> $null
Log $out "## nvcc option sweep on one TU ($Reps runs, min / median s; regs/stack/spills from ptxas -v of the last run)"
$sweep = @(
  @{l='default (gencode sm_120 SASS only)'; o=''},
  @{l='-arch=sm_120 (SASS + PTX)'; o='ARCH'},
  @{l='--split-compile=0'; o='--split-compile=0'},
  @{l='--Ofast-compile=max'; o='--Ofast-compile=max'},
  @{l='-Xptxas --register-usage-level=1'; o='-Xptxas --register-usage-level=1'},
  @{l='-Xptxas --register-usage-level=10'; o='-Xptxas --register-usage-level=10'},
  @{l='-Xptxas -O1'; o='-Xptxas -O1'},
  @{l='-lineinfo'; o='-lineinfo'}
)
foreach ($pick in @('base_fused','w8_noinl_fused')) {
  $u = $units | Where-Object { $_.name -eq $pick }
  Log $out "### TU $pick"
  foreach ($s in $sweep) {
    $arch = if ($s.o -eq 'ARCH') { '-arch=sm_120' } else { '-gencode arch=compute_120,code=sm_120' }
    $extra = if ($s.o -eq 'ARCH') { '' } else { $s.o }
    $t = @(); for ($i = 0; $i -lt $Reps; ++$i) { $t += Time-Cmd { Invoke-Expression "nvcc $arch $extra -O3 -std=c++17 -Xcompiler /MD -Iinclude -Isrc $($u.flags) -Xptxas -v -c $($u.src) -o out\tmp\x.obj" } }
    $lg = (Invoke-Expression "nvcc $arch $extra -O3 -std=c++17 -Xcompiler /MD -Iinclude -Isrc $($u.flags) -Xptxas -v -c $($u.src) -o out\tmp\x.obj 2>&1") | Out-String
    $r = [regex]::Match($lg, "Used (\d+) registers"); $st = [regex]::Match($lg, '(\d+) bytes stack frame, (\d+) bytes spill stores, (\d+) bytes spill loads')
    $m = Min-Med $t
    if ($lg -match 'error|nvcc fatal') { Log $out "$($s.l): FAILED: $(($lg -split "`n" | Select-String 'error|fatal' | Select -First 1).Line.Trim())" }
    else { Log $out "$($s.l): min $($m.min) median $($m.median) | regs $($r.Groups[1].Value) stack $($st.Groups[1].Value) spill_st $($st.Groups[2].Value) spill_ld $($st.Groups[3].Value)" }
  }
}
Log $out "## CPU build (cl /O2 /EHsc /std:c++17 /MD, cpu_main.cpp, $Reps runs)"
New-Item -ItemType Directory -Force out\agree | Out-Null
foreach ($cfg in @(@{l='float'; f=''; o='cpu_f32'}, @{l='double'; f='/DKL_REAL_DOUBLE'; o='cpu_f64'}, @{l='float, /arch:AVX2 /fp:contract (FMA)'; f='/arch:AVX2 /fp:contract'; o='cpu_f32_fma'})) {
  $t = @(); for ($i = 0; $i -lt $Reps; ++$i) { $t += Time-Cmd { Invoke-Expression "cl /nologo /O2 /EHsc /std:c++17 /MD $($cfg.f) -Iinclude src\cpu_main.cpp /Fo:out\agree\$($cfg.o).obj /Fe:out\agree\$($cfg.o).exe" } }
  $m = Min-Med $t; Log $out "cpu $($cfg.l): min $($m.min) median $($m.median) ($($t -join '/'))"
}
Log $out "# GPU snapshot after: $(Gpu-Snap)"
Remove-Item out\tmp -Recurse -Force
