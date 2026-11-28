# SPDX-License-Identifier: Apache-2.0
# The machine rule of the timing rows (owner decision 47; 0009 d13 preamble, d18). Dot-source this file
# (lib/python.ps1 first). The id is computed once, by cli/bench.cpp, and read here through tools/machine_id.py
# (`astroray_cli machine-id`); the reference id and the machine profiles are bench/machines.toml. A row is judged
# by machine_id.py's `judge`, so the rule exists once for these tools and for the Python ones.
# Ref: docs/decisions/0000-owner-decisions.md decision 47 (a-f).
$script:ArMachineIdForTest = ''   # fixtures only: stands in for the CLI's answer
$script:ArMachineIdCache = ''

function Get-ArMachineScript { Join-Path (Split-Path -Parent $PSScriptRoot) 'machine_id.py' }

# The id of this machine. Throws (naming tools/build.ps1) when the CLI is not built.
function Get-ArMachineId {
  if ($script:ArMachineIdForTest) { return $script:ArMachineIdForTest }
  if (-not $script:ArMachineIdCache) {
    $out = @(& (Get-ArPython) -I (Get-ArMachineScript) id 2>&1 | ForEach-Object { "$_" })
    if ($LASTEXITCODE -ne 0 -or -not $out) { throw ($out -join ' ') }
    $script:ArMachineIdCache = $out[-1].Trim()
  }
  return $script:ArMachineIdCache
}

function Get-ArReferenceMachineId {
  $out = @(& (Get-ArPython) -I (Get-ArMachineScript) reference 2>&1 | ForEach-Object { "$_" })
  if ($LASTEXITCODE -ne 0 -or -not $out) { throw ($out -join ' ') }
  return $out[-1].Trim()
}

function Test-ArReferenceMachine { return ((Get-ArMachineId) -eq (Get-ArReferenceMachineId)) }

function Write-ArOffReference([string]$Line) { Write-Host $Line }

# One timing row against its reference limit: Verdict PASS, FAIL or OFF (over the limit on a machine without a
# profile: the line is printed by the caller with Write-ArOffReference and nothing fails), Limit (the profile's
# derived one where it applies) and Line (what to print; text of the error on a FAIL that is not a plain overrun).
# Within the limit nothing is read: no CLI, no file. -NoMachine (hosted ci, which has no machine id) judges against
# the limit alone.
function Resolve-ArTimingRow {
  param([Parameter(Mandatory)][string]$Row, [Parameter(Mandatory)][double]$Measured, [Parameter(Mandatory)][double]$Limit,
    [string]$Unit = 's', [switch]$NoMachine)
  $inv = [Globalization.CultureInfo]::InvariantCulture
  if ($Measured -le $Limit) { return [pscustomobject]@{ Verdict = 'PASS'; Limit = $Limit; Line = '' } }
  if ($NoMachine) { return [pscustomobject]@{ Verdict = 'FAIL'; Limit = $Limit; Line = '' } }
  # The id here, not inside judge: this process caches it, a judge process would run the CLI again.
  try { $id = Get-ArMachineId } catch { return [pscustomobject]@{ Verdict = 'FAIL'; Limit = $Limit; Line = "$($_.Exception.Message)" } }
  $a = @('-I', (Get-ArMachineScript), 'judge', '--row', $Row, '--measured', $Measured.ToString('R', $inv),
    '--limit', $Limit.ToString('R', $inv), '--unit', $Unit, '--id', $id)
  $out = @(& (Get-ArPython) @a 2>&1 | ForEach-Object { "$_" })
  $j = if ($LASTEXITCODE -eq 0 -and $out) { try { $out[-1] | ConvertFrom-Json } catch { $null } } else { $null }
  if (-not $j) { return [pscustomobject]@{ Verdict = 'FAIL'; Limit = $Limit; Line = ($out -join ' ') } }
  [pscustomobject]@{ Verdict = $j.verdict.ToUpperInvariant(); Limit = [double]$j.limit; Line = "$($j.line)" }
}

# Tells tests with a timing CHECK of their own (ASTRORAY_OFF_REFERENCE, machine_id.py timing_ok and
# tests/common/off_reference.h) which standing to take: set to this machine's id when it is not the reference one,
# removed otherwise, so a stray value cannot soften the reference machine's verdicts. Returns the id, or '' on hosted ci.
function Set-ArMachineStanding([switch]$NoMachine) {
  if ($NoMachine) { Remove-Item Env:ASTRORAY_OFF_REFERENCE -ErrorAction SilentlyContinue; return '' }
  $id = Get-ArMachineId
  if ($id -eq (Get-ArReferenceMachineId)) { Remove-Item Env:ASTRORAY_OFF_REFERENCE -ErrorAction SilentlyContinue }
  else { $env:ASTRORAY_OFF_REFERENCE = $id }
  return $id
}
