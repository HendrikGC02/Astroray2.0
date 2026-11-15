# SPDX-License-Identifier: Apache-2.0
# Process control by recorded PID only (0009 d8). Dot-source this file.
function Start-ArProcess {
  param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$File,
    [string[]]$ArgList = @(),
    [string]$LogPath
  )
  $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
  $pidDir = Join-Path $root 'out/pids'
  New-Item -ItemType Directory -Force $pidDir | Out-Null
  $sp = @{ FilePath = $File; PassThru = $true; NoNewWindow = $true }
  if ($ArgList.Count -gt 0) { $sp.ArgumentList = @($ArgList | ForEach-Object { '"' + $_ + '"' }) }
  if ($LogPath) { $sp.RedirectStandardOutput = $LogPath; $sp.RedirectStandardError = "$LogPath.err" }
  $p = Start-Process @sp
  Set-Content -Path (Join-Path $pidDir "$Name.pid") -Value $p.Id
  return $p
}

function Stop-ArProcess {
  param([Parameter(Mandatory)][int]$Id)
  Stop-Process -Id $Id -Force -ErrorAction SilentlyContinue
}
