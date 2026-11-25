# SPDX-License-Identifier: Apache-2.0
# Fast test of tools/region_bias.py: its self-test on synthetic planes with known correlation and bias.
# Usage: pwsh -NoProfile -File tests/tools/fast_region_bias.ps1   (exit 0 = pass)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $root 'tools/lib/python.ps1')
$py = Get-ArPython
$out = @(& $py -I (Join-Path $root 'tools/region_bias.py') --self-test 2>&1 | ForEach-Object { "$_" })
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
if ($code -ne 0 -or -not ($out | Where-Object { $_ -match '^PASS region_bias self-test' })) {
  Write-Host 'FAIL region_bias: self-test'
  exit 1
}
