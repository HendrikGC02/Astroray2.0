# Paths/s per kernel variant on 2^20 paths: 5 warm-up + 20 timed launches per kernel, burn-in first; the whole bench is repeated $Runs times.
param([int]$Runs = 3)
. $PSScriptRoot\env.ps1; . $PSScriptRoot\common.ps1
Set-Location $PSScriptRoot
$f = 'runtime_raw.txt'; Remove-Item "$PSScriptRoot\out\results\$f" -ErrorAction SilentlyContinue
$all = @()
for ($r = 1; $r -le $Runs; ++$r) {
  Log $f "# run $r GPU snapshot before (util%,sm MHz,W,C): $(Gpu-Snap)"
  $lines = & .\out\bench.exe 2>&1
  foreach ($l in $lines) { Log $f "run$r,$l"; if ($l -like 'RESULT,*') { $p = $l.Split(','); $all += [pscustomobject]@{ run=$r; variant=$p[1]; kernel=$p[2]; records=$p[3]; min=[double]$p[4]; median=[double]$p[5] } } }
  Log $f "# run $r GPU snapshot after: $(Gpu-Snap)"
}
$sum = "$PSScriptRoot\out\results\runtime_summary.csv"
Set-Content $sum 'variant,kernel,records,min_ms_over_runs,median_ms_range_over_runs,median_ms_median_over_runs,Mpaths_per_s_at_median_of_medians'
$all | Group-Object variant, kernel, records | ForEach-Object {
  $g = $_.Group; $meds = $g.median | Sort-Object; $mid = $meds[[int][math]::Floor(($meds.Count - 1) / 2)]
  Add-Content $sum ("{0},{1},{2},{3:F4},{4:F4}-{5:F4},{6:F4},{7:F1}" -f $g[0].variant, $g[0].kernel, $g[0].records, ($g.min | Measure-Object -Minimum).Minimum, $meds[0], $meds[-1], $mid, (1048576 / ($mid * 1e-3) / 1e6))
}
Get-Content $sum | Out-Host
