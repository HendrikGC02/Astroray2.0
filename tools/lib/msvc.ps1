# SPDX-License-Identifier: Apache-2.0
# The pinned MSVC environment (0009 d2), found once for build.ps1 and for kernelgate's timed compiles
# (tools/kernelgate.py runs this file in a pwsh child), so both time the compiler the same way. Dot-source this file.
# Get-ArMsvcEnv returns the `set` dump of vcvarsall (NAME=VALUE lines) and throws when vswhere, the Visual
# Studio install or vcvarsall is missing; Import-ArMsvcEnv puts that dump into this process.
function Get-ArMsvcEnv {
  $isCI = ($env:CI -eq 'true')
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found' }
  $vsArgs = @('-latest', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
  $vs = & $vswhere @vsArgs -products Microsoft.VisualStudio.Product.BuildTools | Select-Object -First 1
  if (-not $vs) { $vs = & $vswhere @vsArgs -products '*' | Select-Object -First 1 }
  if (-not $vs) { throw 'no Visual Studio with the C++ x64 tools' }
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
  $verArg = if ($isCI) { '' } else { '-vcvars_ver=14.44' }
  $envDump = & cmd.exe /c "`"$vcvars`" x64 $verArg >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { throw 'vcvarsall failed (toolset 14.44 installed?)' }
  return $envDump
}

function Import-ArMsvcEnv {
  foreach ($line in (Get-ArMsvcEnv)) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
  }
}
