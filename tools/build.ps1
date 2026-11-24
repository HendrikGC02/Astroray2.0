# SPDX-License-Identifier: Apache-2.0
# tools/build.ps1 [-Target t[,t]] [-Preset dev|cpu] [-CheckOnly] [-NoMachineLock] [-Jobs n]
# Enters the pinned MSVC environment, configures when needed, builds, then proves
# with `ninja -n` that no work is left (0009 d5, d9).
# Pins (0009 d2): MSVC toolset 14.44.x from VS 2022 Build Tools 17.14
# (vcvars_ver=14.44; hosted CI: cl >= 19.44, no -vcvars_ver), CUDA 12.8,
# OptiX 9.1 headers, CMake >= 3.31 (4.2.1 installed), Ninja 1.13.x, pwsh 7.
# Takes the machine lock (not with -NoMachineLock, for CPU-only callers) and the
# tree lock on out/build (0009 d8), and a share of the gate's fast window (lib/lock.ps1); the summary reports
# the waits.
# Output: at most 20 lines, last line "PASS build <s> s (budget n/a, waited <w> s)"
# or "FAIL build ..."; full log in out/check/build.log, JSON in build.json;
# non-zero exit on failure.
param(
  [string[]]$Target = @('all'),
  [ValidateSet('dev', 'cpu')][string]$Preset = 'dev',
  [switch]$CheckOnly,
  [switch]$NoMachineLock,
  [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$buildDir = Join-Path $root 'out/build'
$logDir = Join-Path $root 'out/check'
$log = Join-Path $logDir 'build.log'
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/summary.ps1')

# Locks first; the tier clock starts after them so waits are excluded.
$waited = 0.0
$machineWait = 0.0
if (-not $NoMachineLock) { $machineWait = Enter-ArMachineLock }
# After the machine lock (a gate holds it before its fast window, so a build that held a share and waited for
# the lock would deadlock the gate); before the tree lock. The gate's own fast tier is nested and does not wait.
$shareWait = Enter-ArBuildShare
$treeWait = Enter-ArTreeLock $buildDir
$waited = $machineWait + $shareWait + $treeWait
Start-ArSummary 'build'
$clock = [Diagnostics.Stopwatch]::StartNew()

function Say([string]$m) { Add-Content -Path $log -Value $m }
function Fail([string]$why) {
  $null = Complete-ArSummary -Pass $false -Waited $waited -Reason $why
  Exit-ArTreeLock
  Exit-ArBuildShare
  Exit-ArMachineLock
  exit 1
}
# Run a native command, append its output to the log, return its exit code.
function Run([string]$exe, [string[]]$argv) {
  Add-Content -Path $log -Value "> $exe $($argv -join ' ')"
  & $exe @argv 2>&1 | Out-File -Append -Encoding ascii -FilePath $log
  return $LASTEXITCODE
}

try {
  $isCI = ($env:CI -eq 'true')
  $cmakeBin = 'C:\Program Files\CMake\bin'
  if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -and (Test-Path $cmakeBin)) {
    $env:PATH = "$cmakeBin;$env:PATH"
  }

  # --- MSVC environment ---
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found' }
  $vsArgs = @('-latest', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
  $vs = & $vswhere @vsArgs -products Microsoft.VisualStudio.Product.BuildTools | Select-Object -First 1
  if (-not $vs) { $vs = & $vswhere @vsArgs -products '*' | Select-Object -First 1 }
  if (-not $vs) { Fail 'no Visual Studio with the C++ x64 tools' }
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
  $verArg = if ($isCI) { '' } else { '-vcvars_ver=14.44' }
  $envDump = & cmd.exe /c "`"$vcvars`" x64 $verArg >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { Fail "vcvarsall failed (toolset 14.44 installed?)" }
  foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
  }
  if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { Fail 'ninja not on PATH' }

  # --- configure ---
  if (-not $CheckOnly) {
    # Source registration (cmake/sources.cmake): same patterns, plain GLOB there.
    $globs = @('tests/cpu/test_*.cpp', 'tests/common/*.cpp', 'src/host/*.cpp', 'src/device/cpu/*.cpp', 'src/device/cuda/*.cpp', 'src/capi/*.cpp', 'src/kernel_gpu/*.cu', 'tests/gpu/kernels/*.cu', 'src/kernel_cpu/*.cpp', 'tests/gpu/test_*.cpp', 'tests/gpu/main.cpp', 'blender/astroray/*')
    $files = foreach ($g in $globs) {
      $dir = Split-Path $g -Parent
      $rec = $g.StartsWith('src/host') -or $g.StartsWith('blender/astroray')
      if (Test-Path $dir) {
        Get-ChildItem -Path $dir -Filter (Split-Path $g -Leaf) -File -Recurse:$rec |
          ForEach-Object { $_.FullName.Substring($root.Length + 1).Replace('\', '/') }
      }
    }
    $lines = @()
    foreach ($f in ($files | Sort-Object { $_ } -CaseSensitive)) {
      $lines += $f
      if ($f -like 'tests/*') {
        $head = Get-Content $f -TotalCount 5
        if ($head -match '// ar-f64') { $lines += "f64 $f" }
      }
    }
    $listText = ($lines -join "`n") + "`n"
    $listFile = Join-Path $buildDir 'sources.txt'
    $old = if (Test-Path $listFile) { [IO.File]::ReadAllText($listFile) } else { $null }
    $needCfg = (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) -or ($old -ne $listText)
    if ($needCfg) {
      $t = $clock.Elapsed.TotalSeconds
      if ((Run 'cmake' @('--preset', $Preset)) -ne 0) { Fail "configure failed (see $log)" }
      [IO.File]::WriteAllText($listFile, $listText)
      Add-ArStep "configure $Preset" ($clock.Elapsed.TotalSeconds - $t)
    }

    # --- build ---
    $t = $clock.Elapsed.TotalSeconds
    $jobArg = if ($Jobs -gt 0) { @('-j', [string]$Jobs) } else { @() }
    if ((Run 'cmake' (@('--build', 'out/build', '--target') + $Target + $jobArg)) -ne 0) { Fail "compile failed (see $log)" }
    Add-ArStep 'build' ($clock.Elapsed.TotalSeconds - $t)
  }

  # --- nothing left to do? ---
  $dry = & ninja -C out/build -n @Target 2>&1
  Add-Content -Path $log -Value ($dry -join "`n")
  if ($LASTEXITCODE -ne 0 -or (($dry | Select-Object -Last 1) -ne 'ninja: no work to do.')) {
    Fail ("ninja -n reports work left: " + ($dry | Where-Object { $_ -notlike 'ninja: Entering*' } | Select-Object -First 1))
  }
} catch {
  Fail ($_.Exception.Message)
}
$null = Complete-ArSummary -Pass $true -Waited $waited
Exit-ArTreeLock
Exit-ArBuildShare
Exit-ArMachineLock
exit 0
