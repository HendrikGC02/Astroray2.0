# SPDX-License-Identifier: Apache-2.0
# tools/build.ps1 reads the CUDA architecture of the build from `nvidia-smi --query-gpu=compute_cap
# --format=csv,noheader` (ConvertFrom-ArNvidiaSmi, loaded from the real script): the first GPU's capability
# as 10*major+minor, and nothing when the output holds no capability, so build.ps1 fails and names the variable
# rather than guessing 120. Canned text only; no GPU or nvidia-smi is needed. Run by tests/tools/run_fast.ps1
# (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$bad = 0
function Check([string]$what, $got, $want) {
  if ($got -ne $want) { Write-Host "FAIL fast_cuda_arch: $what gave '$got', wanted '$want'"; $script:bad++ }
}
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/build.ps1'), [ref]$null, [ref]$null)
$fn = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'ConvertFrom-ArNvidiaSmi' }, $true)
if (-not $fn) { Write-Host 'FAIL fast_cuda_arch: tools/build.ps1 has no ConvertFrom-ArNvidiaSmi'; exit 1 }
. ([scriptblock]::Create($fn.Extent.Text))

Check '12.0' (ConvertFrom-ArNvidiaSmi @('12.0')) 120
Check '8.6' (ConvertFrom-ArNvidiaSmi @('8.6')) 86
Check '8.9 with CRLF residue' (ConvertFrom-ArNvidiaSmi @("8.9`r")) 89
Check '7.5' (ConvertFrom-ArNvidiaSmi @('7.5')) 75
Check 'empty output' (ConvertFrom-ArNvidiaSmi @()) $null
Check 'blank lines only' (ConvertFrom-ArNvidiaSmi @('', '  ')) $null
Check 'an error text' (ConvertFrom-ArNvidiaSmi @('NVIDIA-SMI has failed because it could not communicate with the NVIDIA driver.')) $null
# Two GPUs: the first one, never the better one.
Check 'two GPUs, 8.6 first' (ConvertFrom-ArNvidiaSmi @('8.6', '12.0')) 86
Check 'two GPUs, 12.0 first' (ConvertFrom-ArNvidiaSmi @('12.0', '8.6')) 120
exit $(if ($bad) { 1 } else { 0 })
