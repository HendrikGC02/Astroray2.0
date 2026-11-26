# SPDX-License-Identifier: Apache-2.0
# tools/build.ps1 [-Target t[,t]] [-Preset dev|cpu] [-CheckOnly] [-NoMachineLock] [-Jobs n]
# Enters the pinned MSVC environment, configures when needed, builds, then proves
# with `ninja -n` that no work is left (0009 d5, d9; Test-ArNoWorkLeft settles build_info's stamp first, so an
# index refresh by another process is not work).
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
. (Join-Path $PSScriptRoot 'lib/msvc.ps1')

# Locks first; the tier clock starts after them so waits are excluded.
$waited = 0.0
$machineWait = 0.0
if (-not $NoMachineLock) { $machineWait = Enter-ArMachineLock -Job build -Short }
# After the machine lock (a gate holds it before its fast window, so a build that held a share and waited for
# the lock would deadlock the gate); before the tree lock. The gate's own fast tier is nested and does not wait.
$shareWait = Enter-ArBuildShare
$treeWait = Enter-ArTreeLock $buildDir
$waited = $machineWait + $shareWait + $treeWait
Start-ArSummary 'build'
$clock = [Diagnostics.Stopwatch]::StartNew()

# Whether `ninja -n` reports no work for the targets (0009 d5). An index refresh by any other process (git status,
# git diff, an editor's git integration) makes build_info.cpp's generator stale without a source change, and
# `ninja -n` cannot predict that the generator leaves build_info.cpp alone. So the generator's stamp is built for
# real first: it touches only the stamp when the identity is unchanged, and rewrites build_info.cpp, so that the
# check then reports work, when the commit or the dirty flag moved. Every other target is judged as before. A
# refresh between that step and the check is tried again, at most $Tries times in all.
function Test-ArNoWorkLeft {
  param([string]$BuildDir, [string[]]$Target, [string]$Stamp = 'gen/build_info.stamp', [int]$Tries = 3)
  $all = @()
  for ($i = 0; $i -lt $Tries; $i++) {
    # Dry run first: the settle step may only run the identity generator. A stale manifest (`Re-running CMake`) or
    # any other step is work left, and running the stamp for real would absorb it.
    $pre = @(& ninja -C $BuildDir -n $Stamp 2>&1 | ForEach-Object { "$_" })
    $all += "> ninja -C $BuildDir -n $Stamp"; $all += $pre
    $other = @($pre | Where-Object { $_ -notlike 'ninja: Entering*' -and $_ -ne 'ninja: no work to do.' -and $_ -notmatch '^\[\d+/\d+\] build_info\.cpp$' })
    if ($LASTEXITCODE -ne 0 -or $other.Count -gt 0) {
      return [pscustomobject]@{ Ok = $false; First = $(if ($other.Count) { $other[0] } else { $pre | Select-Object -Last 1 }); Output = $all }
    }
    $set = @(& ninja -C $BuildDir $Stamp 2>&1 | ForEach-Object { "$_" })
    $all += "> ninja -C $BuildDir $Stamp"; $all += $set
    if ($LASTEXITCODE -ne 0) {
      return [pscustomobject]@{ Ok = $false; First = ($set | Where-Object { $_ -notlike 'ninja: Entering*' } | Select-Object -First 1); Output = $all }
    }
    $dry = @(& ninja -C $BuildDir -n @Target 2>&1 | ForEach-Object { "$_" })
    $all += "> ninja -C $BuildDir -n $($Target -join ' ')"; $all += $dry
    $ok = ($LASTEXITCODE -eq 0 -and $dry.Count -gt 0 -and $dry[-1] -eq 'ninja: no work to do.')
    if ($ok -or $i -eq $Tries - 1) {
      return [pscustomobject]@{ Ok = $ok; First = ($dry | Where-Object { $_ -notlike 'ninja: Entering*' } | Select-Object -First 1); Output = $all }
    }
  }
}

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
  Import-ArMsvcEnv
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
  $nw = Test-ArNoWorkLeft $buildDir $Target
  Add-Content -Path $log -Value ($nw.Output -join "`n")
  if (-not $nw.Ok) { Fail ("ninja -n reports work left: " + $nw.First) }
} catch {
  Fail ($_.Exception.Message)
}
$null = Complete-ArSummary -Pass $true -Waited $waited
Exit-ArTreeLock
Exit-ArBuildShare
Exit-ArMachineLock
exit 0
