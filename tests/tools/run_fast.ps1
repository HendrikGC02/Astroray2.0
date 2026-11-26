# SPDX-License-Identifier: Apache-2.0
# Runs every tests/tools/fast_*.ps1 in its own pwsh, prints one PASS/FAIL line each, exits non-zero on any
# failure. CPU only and quick: check.ps1's gate step `tools tests` (tag cpu, so hosted ci runs it) calls it.
# -Set gate (default) skips the fixtures named in $slow, the one list of what left the gate (task 234: 22.6 s of
# the step's 37.5 s); -Set slow runs only those, in `check full`. A fixture is listed by its base name.
param([ValidateSet('gate', 'slow')][string]$Set = 'gate')
$ErrorActionPreference = 'Stop'
$slow = @('fast_landing', 'fast_gate_table', 'fast_timing_rerun')
$bad = 0
foreach ($f in Get-ChildItem $PSScriptRoot -Filter 'fast_*.ps1' | Where-Object { ($_.BaseName -in $slow) -eq ($Set -eq 'slow') } | Sort-Object Name) {
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $out = @(& pwsh -NoProfile -File $f.FullName 2>&1 | ForEach-Object { "$_" })
  if ($LASTEXITCODE -eq 0) { Write-Host ("PASS {0} ({1:N1} s)" -f $f.BaseName, $sw.Elapsed.TotalSeconds) }
  else {
    Write-Host "FAIL $($f.BaseName)"
    # The fixture's own FAIL lines, unindented: check.ps1 echoes lines that start with FAIL, so a hosted log names the check.
    $out | Where-Object { $_ -match '^FAIL ' } | Select-Object -First 6 | ForEach-Object { Write-Host $_ }
    $out | Select-Object -Last 8 | ForEach-Object { Write-Host "  $_" }; $bad++
  }
}
exit $(if ($bad) { 1 } else { 0 })
