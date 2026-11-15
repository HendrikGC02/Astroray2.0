# SPDX-License-Identifier: Apache-2.0
# Isolated Blender launcher (0009 d17a). Dot-source this file.
. (Join-Path $PSScriptRoot 'lock.ps1')
. (Join-Path $PSScriptRoot 'proc.ps1')

function Invoke-ArBlender {
  # Returns @{ ExitCode; Log; Waited }. ExitCode -1 means timeout.
  param(
    [Parameter(Mandatory)][string]$Script,
    [string[]]$Args = @(),
    [int]$TimeoutS = 300
  )
  $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
  $exe = Join-Path $env:ASTRORAY_BLENDER 'blender.exe'
  if (-not $env:ASTRORAY_BLENDER -or -not (Test-Path $exe)) {
    throw "blender.exe not found at '$exe'; set ASTRORAY_BLENDER to the Blender 5.3 install folder"
  }
  $scriptPath = (Resolve-Path $Script).Path
  $runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + "-$PID"
  $profileDir = Join-Path $root "out/blender_home/$runId"
  foreach ($d in 'config', 'scripts', 'extensions', 'datafiles') { New-Item -ItemType Directory -Force (Join-Path $profileDir $d) | Out-Null }
  $log = Join-Path $profileDir 'blender.log'
  $waited = Enter-ArMachineLock
  $saved = @{}
  $vars = @{
    BLENDER_USER_CONFIG = 'config'; BLENDER_USER_SCRIPTS = 'scripts'
    BLENDER_USER_EXTENSIONS = 'extensions'; BLENDER_USER_DATAFILES = 'datafiles'
  }
  try {
    foreach ($k in $vars.Keys) {
      $saved[$k] = [Environment]::GetEnvironmentVariable($k)
      [Environment]::SetEnvironmentVariable($k, (Join-Path $profileDir $vars[$k]))
    }
    $argv = @('--background', '--factory-startup', '--python-exit-code', '1', '--python', $scriptPath, '--') + $Args
    $p = Start-ArProcess -Name 'blender' -File $exe -ArgList $argv -LogPath $log
    if (-not $p.WaitForExit($TimeoutS * 1000)) {
      Stop-ArProcess -Id $p.Id
      $code = -1
      Add-Content -Path $log -Value "TIMEOUT after $TimeoutS s"
    } else {
      $p.WaitForExit()
      $code = $p.ExitCode
    }
  } finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    Exit-ArMachineLock
  }
  if (Test-Path $log) { Get-Content $log | Write-Host }
  $global:LASTEXITCODE = $code
  return @{ ExitCode = $code; Log = $log; Waited = $waited }
}

function Invoke-ArBlenderCommand {
  # Runs `blender --command <args>` (no profile isolation needed; nothing is installed). Returns the exit code.
  param([Parameter(Mandatory)][string[]]$Args, [int]$TimeoutS = 600)
  $exe = Join-Path $env:ASTRORAY_BLENDER 'blender.exe'
  if (-not $env:ASTRORAY_BLENDER -or -not (Test-Path $exe)) {
    throw "blender.exe not found at '$exe'; set ASTRORAY_BLENDER to the Blender 5.3 install folder"
  }
  $null = Enter-ArMachineLock
  $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
  $home_ = Join-Path $root ('out/blender_home/cmd-' + $PID)
  $saved = @{}
  try {
    foreach ($k in 'config', 'scripts', 'extensions', 'datafiles') {
      $v = 'BLENDER_USER_' + $k.ToUpper()
      New-Item -ItemType Directory -Force (Join-Path $home_ $k) | Out-Null
      $saved[$v] = [Environment]::GetEnvironmentVariable($v)
      [Environment]::SetEnvironmentVariable($v, (Join-Path $home_ $k))
    }
    $p = Start-ArProcess -Name 'blender-cmd' -File $exe -ArgList (@('--command') + $Args)
    if (-not $p.WaitForExit($TimeoutS * 1000)) { Stop-ArProcess -Id $p.Id; return -1 }
    $p.WaitForExit()
    return $p.ExitCode
  } finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    Exit-ArMachineLock
  }
}
