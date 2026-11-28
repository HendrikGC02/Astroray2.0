# SPDX-License-Identifier: Apache-2.0
# tools/b23_check.ps1 [-Exe <astroray_tests_cpu.exe>]
# B-23 (0009 d13): runs the full-tier case `light_tree_kernel/B-23 tree build for 10k lights`, parses the line it
# prints (`B-23 build: <ms> ms per 10k lights`, the median of 7 builds) and compares it with the `limit` of
# the row in tools/budgets.toml. The one place that reads the limit; no C++ holds it.
# The caller holds the machine lock (check full does; a canary or a hand run on a quiet machine needs no more).
# Output: the B-23 line; exit 0 at or under the limit, 1 above it, the case failed or no line. On a machine that is not
# the reference one an overrun prints `off-reference <id> B-23: ...` and exits 0 (owner decision 47 c, lib/machine.ps1).
# Ref: 0009 d13 row B-23, d22.
param([string]$Exe = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/machine.ps1')
$py = Get-ArPython
if (-not $Exe) { $Exe = Join-Path $root 'out/bin/astroray_tests_cpu.exe' }
$q = "import tomllib;b=next(b for b in tomllib.load(open('tools/budgets.toml','rb'))['budget'] if b['id']=='B-23');" +
     "print(b.get('limit',''))"
$limit = (& $py -I -c $q | Select-Object -First 1)
if (-not $limit) { Write-Host 'FAIL B-23: tools/budgets.toml gives the row no limit'; exit 1 }
$out = @(& $Exe --tier=full '--test-case=light_tree_kernel/B-23 tree build for 10k lights' 2>&1 | ForEach-Object { "$_" })
if ($LASTEXITCODE -ne 0) { $out | Select-Object -Last 8 | ForEach-Object { Write-Host $_ }; Write-Host 'FAIL B-23: the case failed'; exit 1 }
$m = $out | Select-String -Pattern '^B-23 build: ([0-9.]+) ms per 10k lights' | Select-Object -First 1
if (-not $m) { Write-Host 'FAIL B-23: the case printed no B-23 build line'; exit 1 }
Write-Host $m.Line
$ms = [double]$m.Matches[0].Groups[1].Value
if ($ms -gt [double]$limit) {
  $v = Resolve-ArTimingRow -Row 'B-23' -Measured $ms -Limit ([double]$limit) -Unit 'ms per 10k lights'
  if ($v.Line) { Write-Host $v.Line }
  if ($v.Verdict -eq 'FAIL') { Write-Host "FAIL B-23: $ms ms per 10k lights is above the limit of $limit ms"; exit 1 }
}
Write-Host "PASS B-23: $ms ms per 10k lights (limit $limit ms)"
