# SPDX-License-Identifier: Apache-2.0
# tools/check.ps1 fast
# The 10 s loop (0009 d10): lint, incremental CPU test build, deterministic tests.
# No machine lock (d8); the tree lock on out/build is held while the tests run (build.ps1 takes
# it for the build itself). Limits come from tools/budgets.toml: B-08 (tier), B-12 (one test).
# Output: at most 20 lines, last line "PASS fast <s> s (budget 10 s, waited <w> s)" or "FAIL fast ...";
# JSON in out/check/fast.json; non-zero exit on failure. Other tiers belong to later tasks.
# Tier ci (0009 d12, d24; hosted CI): fast, cpu build, deps, CPU gate tests, tests/abi unittest
# (not test_gpu_*.py); no budget here (B-11 is measured by task 091); logs the cl version.
param([Parameter(Mandatory, Position = 0)][string]$Tier)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/summary.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
if ($Tier -notin @('fast', 'ci')) {
  Write-Host "FAIL check: unknown or not yet implemented tier '$Tier' (only: fast, ci)"
  exit 2
}

Start-ArSummary $Tier
$waited = 0.0
$budget = $null
$isCi = ($Tier -eq 'ci')
$log = Join-Path $root 'out/check/$Tier.log'

function Fail([string]$why) {
  $null = Complete-ArSummary -Pass $false -Waited $waited -Budget $budget -Reason $why
  Exit-ArTreeLock
  exit 1
}
# Run a step, return output lines; its time goes into the summary.
function Step([string]$name, [scriptblock]$body) {
  $t = [Diagnostics.Stopwatch]::StartNew()
  $out = & $body 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  Add-ArStep $name $t.Elapsed.TotalSeconds ($code -eq 0)
  Add-Content -Path $log -Value $out
  return [pscustomobject]@{ Code = $code; Out = @($out) }
}

try {
  $py = Get-ArPython
  # Budgets this tier enforces, from the machine copy of 0009 d13.
  $q = "import tomllib,json;d=tomllib.load(open('tools/budgets.toml','rb'))['budget'];" +
       "print(json.dumps({b['id']:b['limit'] for b in d if 'limit' in b}))"
  $lim = (& $py -I -c $q) | ConvertFrom-Json
  $budget = if ($isCi) { $null } else { [double]$lim.'B-08' }
  $perTest = [double]$lim.'B-12'

  $r = Step 'lint' { & $py -I tools/lint.py }
  if ($r.Code -ne 0) {
    ($r.Out | Select-Object -First 6) | ForEach-Object { Write-Host $_ }
    $ids = ($r.Out | Select-String -Pattern 'B-\d\d' -AllMatches | ForEach-Object { $_.Matches.Value } | Sort-Object -Unique) -join ','
    Fail ("lint: " + $r.Out.Count + " findings " + $ids + " " + $r.Out[0])
  }

  $r = Step 'build' { & pwsh -NoProfile -File tools/build.ps1 -Target astroray_tests_cpu -Preset $(if ($isCi) { 'cpu' } else { 'dev' }) -NoMachineLock -Jobs 4 }
  $bj = Join-Path $root 'out/check/build.json'
  if (Test-Path $bj) { $waited += [double](Get-Content $bj -Raw | ConvertFrom-Json).waited }
  if ($r.Code -ne 0) {
    ($r.Out | Select-Object -Last 5) | ForEach-Object { Write-Host $_ }
    Fail 'build failed (out/check/build.log)'
  }

  $waited += Enter-ArTreeLock (Join-Path $root 'out/build')
  $exe = Join-Path $root 'out/bin/astroray_tests_cpu.exe'
  if (-not (Test-Path $exe)) { Fail "missing $exe" }
  $r = Step 'tests' { & $exe --tier=fast --duration=true }
  Exit-ArTreeLock
  if ($r.Code -ne 0) {
    ($r.Out | Select-Object -Last 8) | ForEach-Object { Write-Host $_ }
    Fail 'tests failed (out/check/fast.log)'
  }
  $slow = @()
  foreach ($ln in $r.Out) {
    if ($ln -match '^\s*([0-9]+\.[0-9]+) s: (.+)$' -and [double]$Matches[1] -gt $perTest) {
      $slow += "$($Matches[2]) took $($Matches[1]) s"
    }
  }
  if ($slow.Count -gt 0) {
    $slow | Select-Object -First 5 | ForEach-Object { Write-Host "B-12: $_ (limit $perTest s)" }
    Fail "B-12: test over $perTest s"
  }

  if ($isCi) {
    $r = Step 'build cpu' { & pwsh -NoProfile -File tools/build.ps1 -Preset cpu -NoMachineLock }
    if ($r.Code -ne 0) {
      ($r.Out | Select-Object -Last 5) | ForEach-Object { Write-Host $_ }
      Fail 'cpu build failed (out/check/build.log)'
    }
    $r = Step 'fetch deps' { & pwsh -NoProfile -File tools/fetch.ps1 deps }
    if ($r.Code -ne 0) {
      ($r.Out | Select-Object -Last 5) | ForEach-Object { Write-Host $_ }
      Fail 'fetch deps failed'
    }
    $r = Step 'gate tests' { & $exe --tier=gate }
    if ($r.Code -ne 0) {
      ($r.Out | Select-Object -Last 8) | ForEach-Object { Write-Host $_ }
      Fail 'gate tests failed (out/check/ci.log)'
    }
    if (Test-Path 'tests/abi') {
      $r = Step 'abi tests' { & $py -I -m unittest discover -s tests/abi -p 'test_*.py' }
      if ($r.Code -ne 0) {
        ($r.Out | Select-Object -Last 8) | ForEach-Object { Write-Host $_ }
        Fail 'abi tests failed (out/check/ci.log)'
      }
    }
    # build.ps1 sets the MSVC environment only in its own process, so read the compiler version
    # CMake recorded when it configured; it is the exact cl that built the tests.
    $cm = Get-ChildItem out/build/CMakeFiles -Recurse -Filter CMakeCXXCompiler.cmake | Select-Object -First 1
    $m = if ($cm) { Select-String -Path $cm.FullName -Pattern 'set\(CMAKE_CXX_COMPILER_VERSION "([^"]+)"' } else { $null }
    if (-not $m) { throw 'cl version not found in out/build/CMakeFiles' }
    $script:clVer = $m.Matches[0].Groups[1].Value
    Write-Host "cl $script:clVer"
    Add-Content -Path $log -Value "cl $script:clVer"
  }
} catch {
  Fail ($_.Exception.Message)
}
$why = if (-not $isCi -and $script:ArClock.Elapsed.TotalSeconds -gt $budget) { "B-08: tier over $budget s" } else { '' }
$ok = Complete-ArSummary -Pass $true -Waited $waited -Budget $budget -Reason $why
if ($isCi) {
  $jp = Join-Path $root 'out/check/ci.json'
  $j = Get-Content $jp -Raw | ConvertFrom-Json
  $j | Add-Member -NotePropertyName cl -NotePropertyValue "$script:clVer"
  $j | ConvertTo-Json -Depth 4 | Set-Content -Path $jp
}
exit ($(if ($ok) { 0 } else { 1 }))
