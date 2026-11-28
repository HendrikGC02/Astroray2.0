# SPDX-License-Identifier: Apache-2.0
# Fast test of tools/land_history.py: its self-test on canned history and ledger lines (a refused merge, a
# missing phase key, refused submits in the ledger) and on empty or malformed history, which must fail loudly.
# Usage: pwsh -NoProfile -File tests/tools/fast_land_history.ps1   (exit 0 = pass)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. (Join-Path $root 'tools/lib/python.ps1')
$py = Get-ArPython
$out = @(& $py -I (Join-Path $root 'tools/land_history.py') --self-test 2>&1 | ForEach-Object { "$_" })
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
if ($code -ne 0 -or -not ($out | Where-Object { $_ -match '^PASS land_history self-test' })) {
  Write-Host 'FAIL land_history: self-test'
  exit 1
}
exit 0
