# SPDX-License-Identifier: Apache-2.0
# Fast test of tools/gate_table.py: two fake gate.json files, median and max columns, group wall.
# Usage: pwsh -NoProfile -File tests/tools/fast_gate_table.ps1   (exit 0 = pass)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $root 'tools/lib/python.ps1')
$py = Get-ArPython
$tmp = Join-Path $root 'out/tmp/157/table-test'
New-Item -ItemType Directory -Force $tmp | Out-Null
function New-Run($path, $pass, $total, $steps) {
  [ordered]@{ tier = 'gate'; pass = $pass; seconds = $total; waited = 0; budget = 180; sha = 'x'
              steps = @($steps | ForEach-Object { [ordered]@{ name = $_[0]; seconds = $_[1]; pass = $true } }) } |
    ConvertTo-Json -Depth 4 | Set-Content -Path $path
}
New-Run "$tmp/a.json" $true 100.0 @(@('alpha', 10.0), @('analytic cpu', 4.0), @('analytic cuda', 7.0))
New-Run "$tmp/b.json" $true 120.0 @(@('alpha', 20.0), @('analytic cpu', 9.0), @('analytic cuda', 5.0))
$out = @(& $py -I (Join-Path $root 'tools/gate_table.py') --check (Join-Path $root 'tools/check.ps1') "$tmp/a.json" "$tmp/b.json")
$fail = 0
function Expect($pattern, $what) {
  if (-not ($out | Where-Object { $_ -match $pattern })) { Write-Host "FAIL gate_table: $what"; $script:fail++ }
}
Expect '^alpha\s+10\.0\s+20\.0\s+15\.0\s+20\.0$' 'median 15.0 and max 20.0 of 10 and 20'
Expect '^TIER seconds\s+100\.0\s+120\.0\s+110\.0\s+120\.0$' 'tier row median 110.0 max 120.0'
Expect '^group g1 wall\s+7\.0\s+9\.0\s+8\.0\s+9\.0$' 'group wall is the slowest member per run'
if ($fail -gt 0) { $out | ForEach-Object { Write-Host $_ }; exit 1 }
Write-Host 'PASS gate_table'
exit 0
