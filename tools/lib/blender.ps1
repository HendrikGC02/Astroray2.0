# SPDX-License-Identifier: Apache-2.0
# Isolated Blender launcher (0009 d17a). Dot-source this file.
. (Join-Path $PSScriptRoot 'lock.ps1')
. (Join-Path $PSScriptRoot 'proc.ps1')
. (Join-Path $PSScriptRoot 'python.ps1')

function Invoke-ArBlender {
  # Returns @{ ExitCode; Log; Waited }. ExitCode -1 means timeout.
  param(
    [Parameter(Mandatory)][string]$Script,
    [string[]]$Args = @(),
    [string[]]$BlenderArgs = @(),   # Blender options placed before --python (e.g. --log cycles --log-level 4)
    [int]$TimeoutS = 300,
    [switch]$Gui   # a windowed Blender (no --background); the script drives everything and must exit on its own
  )
  $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
  $exe = Join-Path (Get-ArBlenderRoot) 'blender.exe'
  if (-not (Test-Path $exe)) {
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
    $argv = $BlenderArgs + @('--python-exit-code', '1', '--python', $scriptPath, '--') + $Args
    if (-not $Gui) { $argv = @('--background', '--factory-startup') + $argv }
    else {
      # A fresh profile opens the Quick Setup popup over the window. --factory-startup ignores saved
      # preferences, so a windowed run seeds the (isolated, otherwise default) profile with a userpref
      # that hides the splash and starts from it.
      $seed = Start-ArProcess -Name 'blender-seed' -File $exe -LogPath (Join-Path $profileDir 'seed.log') -ArgList @(
        '--background', '--factory-startup', '--python-expr',
        'import bpy; bpy.context.preferences.view.show_splash = False; bpy.ops.wm.save_userpref()')
      if (-not $seed.WaitForExit(120000)) { Stop-ArProcess -Id $seed.Id; throw 'seeding the Blender profile timed out' }
      $seed.WaitForExit()
      if (-not (Test-Path (Join-Path $profileDir 'config/userpref.blend'))) { throw 'seeding the Blender profile wrote no userpref.blend' }
    }
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
  $exe = Join-Path (Get-ArBlenderRoot) 'blender.exe'
  if (-not (Test-Path $exe)) {
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

# The Blender suites `check full` runs and `land prepare` runs a subset of for a Blender diff (one list, 0009 d26).
# -Changed: the paths a diff lists; the headless suites always, a windowed gui_* only when the diff touches the
# viewport code it drives or that script itself. Without -Changed: every suite.
function Get-ArFullBlenderSuites {
  param([string[]]$Changed = $null, [string]$Root = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)))
  $echo = '^(Ran |OK$|FAILED|FAIL:|ERROR:)'
  $suites = @(
    [pscustomobject]@{ Name = 'api_surface'; Script = 'tools/api_surface.py'; Gui = $false; TimeoutS = 120; Echo = '^(api_surface:|MISSING|UNCHECKED)' }
    [pscustomobject]@{ Name = 'test_images'; Script = 'tests/blender/test_images.py'; Gui = $false; TimeoutS = 400; Echo = $echo }
    [pscustomobject]@{ Name = 'test_node_values'; Script = 'tests/blender/test_node_values.py'; Gui = $false; TimeoutS = 400; Echo = $echo }
    [pscustomobject]@{ Name = 'headless/test_node_coverage'; Script = 'tests/blender/headless/test_node_coverage.py'; Gui = $false; TimeoutS = 400; Echo = $echo }
    [pscustomobject]@{ Name = 'headless/test_lowering'; Script = 'tests/blender/headless/test_lowering.py'; Gui = $false; TimeoutS = 400; Echo = $echo }
  )
  $viewport = $null -ne $Changed -and @($Changed | Where-Object { $_ -match '^blender/astroray/(viewport|present|engine|heatmap_draw)\.py$' }).Count -gt 0
  foreach ($gf in (Get-ChildItem (Join-Path $Root 'tests/blender') -Filter 'gui_*.py' | Sort-Object Name)) {
    $own = $null -ne $Changed -and $Changed -contains "tests/blender/$($gf.Name)"
    if ($null -eq $Changed -or $viewport -or $own) {
      $suites += [pscustomobject]@{ Name = $gf.BaseName; Script = "tests/blender/$($gf.Name)"; Gui = $true; TimeoutS = 400; Echo = $echo }
    }
  }
  return $suites
}
