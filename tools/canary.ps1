# SPDX-License-Identifier: Apache-2.0
# tools/canary.ps1 NNN [-NoBaseline] [-NoMatrix]
# Ref: docs/decisions/0009-build-test-process.md d26 step 3, d28. The same library code `land prepare` runs.
# In the task worktree: reads tasks/NNN-*.md and tasks/NNN.canary.patch, checks the patch, runs every item without
# and with the patch, then each hunk alone (which named case each hunk turns red). Prints at most 20 lines, the
# full log is out/canary/NNN.log, exit 1 on a refusal. The tree is restored (patch reverse-applied) before it ends.
# -NoBaseline: skip the item runs without the patch. -NoMatrix: skip the per-hunk runs.
param(
  [Parameter(Mandatory, Position = 0)][ValidatePattern('^\d{3}$')][string]$Number,
  [switch]$NoBaseline,
  [switch]$NoMatrix
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib/canary.ps1')
$root = (Resolve-Path ((& git rev-parse --show-toplevel).Trim())).Path
Set-Location $root

function Show-Lines($lines) {
  $lines = @($lines)
  if ($lines.Count -le 20) { $lines | ForEach-Object { Write-Host $_ }; return }
  $lines | Select-Object -First 18 | ForEach-Object { Write-Host $_ }
  Write-Host "... $($lines.Count - 19) more lines in out/canary/$Number.log"
  Write-Host $lines[-1]
}
try {
  $r = Invoke-Canary -Number $Number -Root $root -NoBaseline:$NoBaseline -NoMatrix:$NoMatrix
} catch {
  Write-Host "REFUSE canary: $($_.Exception.Message)"
  exit 1
}
if ($r.Dirty) { Write-Host "FAIL canary: git apply -R failed; the tree is dirty (see out/canary/$Number.log)"; exit 1 }
$out = @($r.Lines) + @($r.Problems | ForEach-Object { "REFUSE canary: $_" })
if (-not $r.Problems.Count) { $out += "PASS canary $Number" }
Show-Lines $out
exit $(if ($r.Problems.Count) { 1 } else { 0 })
