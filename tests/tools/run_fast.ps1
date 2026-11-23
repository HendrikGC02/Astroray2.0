# SPDX-License-Identifier: Apache-2.0
# Runs every tests/tools/fast_*.ps1 in its own pwsh, prints one PASS/FAIL line each, exits non-zero on any
# failure. CPU only and quick: check.ps1's gate step `tools tests` (tag cpu, so hosted ci runs it) calls it.
$ErrorActionPreference = 'Stop'
$bad = 0
foreach ($f in Get-ChildItem $PSScriptRoot -Filter 'fast_*.ps1' | Sort-Object Name) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $out = @(& pwsh -NoProfile -File $f.FullName 2>&1 | ForEach-Object { "$_" })
  if ($LASTEXITCODE -eq 0) { Write-Host ("PASS {0} ({1:N1} s)" -f $f.BaseName, $sw.Elapsed.TotalSeconds) }
  else { Write-Host "FAIL $($f.BaseName)"; $out | Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }; $bad++ }
}
exit $(if ($bad) { 1 } else { 0 })
