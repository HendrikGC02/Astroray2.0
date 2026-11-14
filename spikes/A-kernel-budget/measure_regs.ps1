# Registers / stack / spills per kernel variant, from ptxas -v logs (written by the ninja build) and cuobjdump -res-usage.
# Needs a finished build (ninja -j8 kernels). Writes out/results/regs.csv, regs_raw.txt
. $PSScriptRoot\env.ps1; . $PSScriptRoot\common.ps1; . $PSScriptRoot\variants.ps1
Set-Location $PSScriptRoot
$csv = "$PSScriptRoot\out\results\regs.csv"; $raw = "$PSScriptRoot\out\results\regs_raw.txt"
Set-Content $csv 'unit,regs,stack_bytes,spill_store_bytes,spill_load_bytes,local_bytes,shared_bytes,const0_bytes,sass_instrs,est_resident_warps_of_48'
Set-Content $raw "# raw ptxas -v (from out/obj/<unit>.obj.log, produced by: nvcc -gencode arch=compute_120,code=sm_120 -O3 -std=c++17 -Xptxas -v -c ...) and cuobjdump -res-usage <obj>"
foreach ($u in Get-Units) {
  $log = Get-Content "$PSScriptRoot\$($u.obj).log"
  $idx = ($log | Select-String 'Compiling entry function').LineNumber[0]
  $block = $log[($idx - 1)..($idx + 3)]
  $stackLine = ($block | Select-String 'bytes stack frame').Line
  $usedLine = ($block | Select-String 'Used \d+ registers').Line
  $regs = [int]([regex]::Match($usedLine, 'Used (\d+) registers').Groups[1].Value)
  $m = [regex]::Match($stackLine, '(\d+) bytes stack frame, (\d+) bytes spill stores, (\d+) bytes spill loads')
  $res = cuobjdump -res-usage "$PSScriptRoot\$($u.obj)" 2>&1 | Out-String
  $kres = ($res -split "Function ")[1]; function G($p) { [regex]::Match($kres, $p).Groups[1].Value }
  $sass = (cuobjdump -sass "$PSScriptRoot\$($u.obj)" 2>&1 | Select-String -Pattern '^\s+/\*[0-9a-f]{4,}\*/' | Measure-Object).Count
  $alloc = [math]::Ceiling($regs * 32 / 256) * 256
  $warps = [math]::Min(48, [math]::Floor([math]::Floor(65536 / $alloc) / 8) * 8)
  Add-Content $csv ("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9}" -f $u.name, (G 'REG:(\d+)'), $m.Groups[1].Value, $m.Groups[2].Value, $m.Groups[3].Value, (G 'LOCAL:(\d+)'), (G 'SHARED:(\d+)'), (G 'CONSTANT\[0\]:(\d+)'), $sass, $warps)
  if ($u.name -match '^(base_fused|base_bsdf|base_nee|lb_fused|lb4_fused|lb4_bsdf|noinl_fused|noinl_ptr_fused|w8_fused|w8_lb_fused|w8_noinl_fused|base_param|fvm_fvm|w8_fvm_fvm)$') {
    Add-Content $raw "`n=== $($u.name) ($($u.src) $($u.flags)) ==="
    Add-Content $raw ($log | Select-String 'registers|stack frame|Compile time' | ForEach-Object { $_.Line })
    Add-Content $raw "--- cuobjdump -res-usage $($u.obj) ---"
    Add-Content $raw (($res -split "`r?`n") | Where-Object { $_ -match 'Function|REG:' })
  }
}
Get-Content $csv | Out-Host
