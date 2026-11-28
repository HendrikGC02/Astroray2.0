# SPDX-License-Identifier: Apache-2.0
# tools/test.ps1 -Exe astroray_tests_cpu|astroray_tests_gpu -Case "suite/case" [-Tier fast|gate|full|all]
# tools/test.ps1 -Py tests/abi/test_x.py|tests/abi/gpu_test_x.py|tests/blender/x.py -Case Class.test_name
# Runs one test case under the right lock, so nobody calls a test executable, python or Blender directly.
# Builds first (tools/build.ps1: the CPU exe with -NoMachineLock, the GPU exe, `all` for -Py). The machine
# lock is held for the GPU exe, gpu_test_*.py and Blender (0009 d8) and for any build of device code;
# the tree lock on out/build around the run, as `check fast` holds it. Cases run through lib/testrun.ps1.
# Output: at most 20 lines: the doctest or unittest summary, the first failing lines, then
# "PASS|FAIL test <target> <case> <s> s"; zero matching cases is a FAIL. Full log out/check/test.log.
param(
  [ValidateSet('astroray_tests_cpu', 'astroray_tests_gpu')][string]$Exe = '',
  [string]$Py = '',
  [Parameter(Mandatory)][string]$Case,
  [ValidateSet('fast', 'gate', 'full', 'all')][string]$Tier = 'all'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/testrun.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/machine.ps1')
$log = Join-Path $root 'out/check/test.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
Set-Content -Path $log -Value "test $(Get-Date -Format s) exe='$Exe' py='$Py' case='$Case' tier=$Tier"

if ([bool]$Exe -eq [bool]$Py) { Write-Host 'FAIL test: give exactly one of -Exe or -Py'; exit 2 }
if ($Py -and $PSBoundParameters.ContainsKey('Tier')) { Write-Host 'FAIL test: -Tier applies to -Exe only'; exit 2 }
if ($Exe) {
  $kind = 'exe'; $target = $Exe
  $machine = $Exe -eq 'astroray_tests_gpu'
  $build = @('-Target', $Exe) + $(if ($machine) { @() } else { @('-NoMachineLock') })
} else {
  $target = $Py -replace '\\', '/'
  if ($target -notmatch '^tests/(abi|blender)/[^/]+\.py$') { Write-Host "FAIL test: -Py takes tests/abi/<file>.py or tests/blender/<file>.py, not '$Py'"; exit 2 }
  if (-not (Test-Path $target)) { Write-Host "FAIL test: $target does not exist"; exit 2 }
  $kind = if ($target -like 'tests/abi/*') { 'py' } else { 'blender' }
  $machine = $kind -eq 'blender' -or (Split-Path $target -Leaf) -like 'gpu_test_*'
  $build = @('-Target', 'all')
}

$waited = 0.0
$pass = $false
$lines = @()
$secs = 0.0
try {
  # A build of device code takes the machine lock too. Taken here, so its waiting messages reach the
  # console (the build's own output is captured); released after the build when the run needs no lock.
  if ($machine -or $build -notcontains '-NoMachineLock') { $waited += Enter-ArMachineLock -Job test -Short:([bool]$Case) }
  Set-ArLockStep "build $target"
  $b = @(& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') @build 2>&1 | ForEach-Object { "$_" })
  Add-Content -Path $log -Value $b
  if ($LASTEXITCODE -ne 0) {
    $b | Select-Object -Last 5 | ForEach-Object { Write-Host $_ }
    Write-Host "FAIL test $target $Case 0.0 s (build failed; out/check/build.log)"
    exit 1
  }
  $bj = Join-Path $root 'out/check/build.json'
  if (Test-Path $bj) { $waited += [double](Get-Content $bj -Raw | ConvertFrom-Json).waited }
  # The standing a tier would give the case (ASTRORAY_OFF_REFERENCE, lib/machine.ps1): a hand-run timing case behaves as
  # in the tier. The CLI computes the id; the exe targets do not build it.
  if ($Exe) {
    $cb = @(& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') -Target astroray_cli $(if (-not $machine) { '-NoMachineLock' }) 2>&1 | ForEach-Object { "$_" })
    Add-Content -Path $log -Value $cb
    if ($LASTEXITCODE -ne 0) { $cb | Select-Object -Last 5 | ForEach-Object { Write-Host $_ }; Write-Host "FAIL test $target $Case 0.0 s (astroray_cli build failed: it computes the machine id)"; exit 1 }
  }
  try { $null = Set-ArMachineStanding } catch { Write-Host "FAIL test $target $Case 0.0 s (machine id: $($_.Exception.Message))"; exit 1 }
  if (-not $machine) { Exit-ArMachineLock }
  $waited += Enter-ArTreeLock (Join-Path $root 'out/build')
  $sw = [Diagnostics.Stopwatch]::StartNew()
  Set-ArLockStep "run $target $Case"
  $res = @(Invoke-ArTestCase -Kind $kind -Target $target -Case $Case -Tier $Tier -Root $root)
  $secs = $sw.Elapsed.TotalSeconds
  Add-Content -Path $log -Value $res[0].Output
  $v = Get-ArTestVerdict $res
  $pass = $v.Pass
  $lines = @(Select-ArTestLines $res[0].Output 19)
  if (-not $pass) { $lines = @(@($lines | Select-Object -First 18) + "reason: $($v.Reason)") }
} catch {
  $lines = @("error: $($_.Exception.Message)")
} finally {
  Exit-ArTreeLock
  Exit-ArMachineLock
}
$lines | ForEach-Object { Write-Host $_ }
$verdict = "{0} test {1} {2} {3:F1} s (waited {4:F0} s)" -f $(if ($pass) { 'PASS' } else { 'FAIL' }), $target, $Case, $secs, $waited
Add-Content -Path $log -Value $verdict
Write-Host $verdict
exit $(if ($pass) { 0 } else { 1 })
