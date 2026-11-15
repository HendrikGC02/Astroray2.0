# SPDX-License-Identifier: Apache-2.0
# Step collector and the 20-line summary with its JSON (0009 d9). Dot-source this file.
$script:ArSteps = @()
$script:ArTier = ''
$script:ArClock = $null
$script:ArRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Start-ArSummary {
  # Call after the locks are held so the tier time excludes lock wait.
  param([Parameter(Mandatory)][string]$Tier)
  $script:ArTier = $Tier
  $script:ArSteps = @()
  $script:ArClock = [Diagnostics.Stopwatch]::StartNew()
  $dir = Join-Path $script:ArRoot 'out/check'
  New-Item -ItemType Directory -Force $dir | Out-Null
  Set-Content -Path (Join-Path $dir "$Tier.log") -Value "$Tier $(Get-Date -Format o)"
}

function Add-ArStep {
  param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][double]$Seconds, [bool]$Pass = $true)
  $script:ArSteps += [pscustomobject]@{ name = $Name; seconds = [math]::Round($Seconds, 2); pass = $Pass }
}

function Complete-ArSummary {
  # Budget is $null while the tier has no budget row.
  param([bool]$Pass = $true, [double]$Waited = 0, $Budget = $null, [string]$Reason = '')
  $secs = $script:ArClock.Elapsed.TotalSeconds
  $dir = Join-Path $script:ArRoot 'out/check'
  $sha = (& git -C $script:ArRoot rev-parse --short HEAD 2>$null)
  $over = ($null -ne $Budget) -and ($secs -gt $Budget)
  $ok = $Pass -and -not $over
  $bt = if ($null -ne $Budget) { "$Budget s" } else { 'n/a' }
  $lines = @()
  foreach ($s in $script:ArSteps) {
    $lines += ("{0,-4} {1} {2:N1} s" -f ($(if ($s.pass) { 'ok' } else { 'FAIL' })), $s.name, $s.seconds)
  }
  if ($lines.Count -gt 19) { $lines = @($lines[0..17]) + @("... more steps in the log") }
  $word = if ($ok) { 'PASS' } else { 'FAIL' }
  $tail = ("{0} {1} {2:N1} s (budget {3}, waited {4:N1} s)" -f $word, $script:ArTier, $secs, $bt, $Waited)
  if ($Reason) { $tail += " $Reason" }
  $lines += $tail
  $lines | ForEach-Object { Write-Host $_ }
  Add-Content -Path (Join-Path $dir "$($script:ArTier).log") -Value $lines
  $json = [ordered]@{
    tier = $script:ArTier; pass = $ok; seconds = [math]::Round($secs, 2); waited = [math]::Round($Waited, 2)
    budget = $Budget; sha = "$sha"; steps = @($script:ArSteps)
  }
  $json | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $dir "$($script:ArTier).json")
  return $ok
}
