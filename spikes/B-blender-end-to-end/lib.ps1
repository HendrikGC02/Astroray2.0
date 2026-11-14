# Helpers: run an ISOLATED Blender (own config/scripts/extensions/datafiles under .\blender_home) as its own process.
# Kills only the PID it started.
. "$PSScriptRoot\env.ps1"
$global:HOME_B = "$PSScriptRoot\blender_home"
foreach ($d in 'config','scripts','extensions','datafiles') { New-Item -ItemType Directory -Force "$HOME_B\$d" | Out-Null }
New-Item -ItemType Directory -Force "$PSScriptRoot\out" | Out-Null

function Invoke-IsolatedBlender {
  param([string]$Script, [string[]]$ScriptArgs = @(), [switch]$Background, [string]$BlendFile = '',
        [int]$TimeoutSec = 300, [string]$LogName = 'blender.log', [string[]]$ExtraBlenderArgs = @(), [string]$WindowSize = '')
  $env:BLENDER_USER_CONFIG = "$HOME_B\config"; $env:BLENDER_USER_SCRIPTS = "$HOME_B\scripts"
  $env:BLENDER_USER_EXTENSIONS = "$HOME_B\extensions"; $env:BLENDER_USER_DATAFILES = "$HOME_B\datafiles"
  $log = "$SB\out\$LogName"; $err = "$SB\out\$LogName.err"
  $a = @()
  if ($Background) { $a += '-b' }
  $a += '--factory-startup'
  if ($WindowSize) { $a += @('--window-geometry') + ($WindowSize -split ' ') }
  if ($BlendFile) { $a += $BlendFile }
  $a += $ExtraBlenderArgs
  $a += @('--python', $Script)
  if ($ScriptArgs.Count) { $a += '--'; $a += $ScriptArgs }
  $env:SB_LAUNCH_EPOCH = ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() / 1000.0).ToString([Globalization.CultureInfo]::InvariantCulture)
  $p = Start-Process -FilePath $BLENDER -ArgumentList $a -PassThru -RedirectStandardOutput $log -RedirectStandardError $err -WindowStyle Normal
  Write-Host "started blender PID $($p.Id)"
  if (-not $p.WaitForExit($TimeoutSec * 1000)) {
    Write-Host "TIMEOUT: killing own PID $($p.Id)"; Stop-Process -Id $p.Id -Force; return -999
  }
  return $p.ExitCode
}
