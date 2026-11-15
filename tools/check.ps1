# SPDX-License-Identifier: Apache-2.0
# tools/check.ps1 fast|gate|full|ci [-Declared "name:regs=+0,stack=+16;..."]
# fast: the 10 s loop (0009 d10): lint, incremental CPU test build, deterministic tests.
# No machine lock (d8); the tree lock on out/build is held while the tests run (build.ps1 takes
# it for the build itself). Limits come from tools/budgets.toml: B-08 (tier), B-12 (one test).
# Output: at most 20 lines, last line "PASS fast <s> s (budget 10 s, waited <w> s)" or "FAIL fast ...";
# JSON in out/check/<tier>.json, log in out/check/<tier>.log; non-zero exit on failure.
# gate (0009 d11, d18; machine lock): fast, then the gate steps declared below, against B-09
# (180 s in all, 120 s for the kernel part). Every gate step carries a tag: `cpu` (needs neither a
# GPU nor Blender) or `local`; a task that adds a gate step tags it, and tier ci runs the `cpu` ones.
# -Declared is the task's declared kernel delta, printed next to the measured one.
# full: gate plus the probe build; uncapped registers are warnings, never failures (0002 D10).
# ci (0009 d12, d24; hosted CI): fast, cpu build, deps, every `cpu`-tagged gate step; no budget
# here (B-11 is measured by task 091); logs the cl version.
param([Parameter(Mandatory, Position = 0)][string]$Tier, [string]$Declared = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/summary.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
if ($Tier -notin @('fast', 'gate', 'full', 'ci')) {
  Write-Host "FAIL check: unknown tier '$Tier' (fast, gate, full, ci)"
  exit 2
}

# Locks first; the tier clock starts after them so waits are excluded (0009 d8).
$waited = 0.0
if ($Tier -in @('gate', 'full')) { $waited = Enter-ArMachineLock }
Start-ArSummary $Tier
$budget = $null
$isCi = ($Tier -eq 'ci')
$log = Join-Path $root "out/check/$Tier.log"
# B-09 (0009 d13): `check gate` <= 3 min, of which the kernel part (device builds, kernel gate,
# compile timing, GPU gate tests; 0002 D10) <= 120 s.
$gateBudget = 180.0
$kernelBudget = 120.0
$kernelSteps = @('build dev', 'kernelgate', 'compile timing', 'gpu gate tests')

function Fail([string]$why) {
  $null = Complete-ArSummary -Pass $false -Waited $waited -Budget $budget -Reason $why
  Exit-ArTreeLock
  Exit-ArMachineLock
  exit 1
}
# Run a step, return output lines; its time goes into the summary.
function Step([string]$name, [scriptblock]$body) {
  $t = [Diagnostics.Stopwatch]::StartNew()
  $global:LASTEXITCODE = 0
  $out = & $body 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  Add-ArStep $name $t.Elapsed.TotalSeconds ($code -eq 0)
  Add-Content -Path $log -Value $out
  return [pscustomobject]@{ Code = $code; Out = @($out) }
}
function Show-Tail($r, [int]$n = 8) {
  ($r.Out | Select-Object -Last $n) | ForEach-Object { Write-Host $_ }
}
# Identity (0009 d5): the binary must be this commit's, on embedded kernels.
function Test-Identity($r, [string]$what) {
  $b = $r.Out | Select-String -Pattern '^build: sha=(\S+) dirty=[01] kernels=(.*)$' | Select-Object -First 1
  if (-not $b) { Fail "$what printed no build line" }
  $head = (& git rev-parse HEAD).Trim()
  if ($b.Matches[0].Groups[1].Value -ne $head) {
    Fail "$what sha $($b.Matches[0].Groups[1].Value) is not HEAD $head"
  }
  if ($b.Matches[0].Groups[2].Value -ne 'embedded') {
    Fail "$what kernel source is '$($b.Matches[0].Groups[2].Value)', not embedded (ASTRORAY_KERNEL_DIR set?)"
  }
}

try {
  $py = Get-ArPython
  $exe = Join-Path $root 'out/bin/astroray_tests_cpu.exe'
  $gpuExe = Join-Path $root 'out/bin/astroray_tests_gpu.exe'
  $dargs = if ($Declared) { @('--declared', $Declared) } else { @() }

  # The gate steps (name, tag, kernel part is summed by $kernelSteps, Lock = hold the tree lock,
  # Identity = the exe must print this commit's build line). Each body fails by its exit code.
  $gateSteps = @(
    @{ Name = 'build dev'; Tag = 'local'; Run = { & pwsh -NoProfile -Command "& tools/build.ps1 -Target all,astroray_kernels_gpu_nofma; exit `$LASTEXITCODE" } },
    @{ Name = 'kernelgate self-test'; Tag = 'cpu'; Run = { & $py -I tools/kernelgate.py --self-test } },
    @{ Name = 'kernelgate'; Tag = 'local'; Lock = $true; Echo = '^kernelgate: \d+ row'; Run = { & $py -I tools/kernelgate.py @dargs } },
    @{ Name = 'compile timing'; Tag = 'local'; Lock = $true; Run = { & $py -I tools/kernelgate.py --time } },
    @{ Name = 'abi tests'; Tag = 'cpu'; Run = {
        if (Test-Path 'tests/abi') { & $py -I -m unittest discover -s tests/abi -p 'test_*.py' } } },
    @{ Name = 'dependents'; Tag = 'cpu'; Run = {
        # 0001 V5 first part: the engine DLL and the CLI import system DLLs only. dumpbin sits next to
        # the linker CMake recorded; build.ps1 sets the MSVC environment only in its own process.
        $lk = Select-String -Path (Join-Path $root 'out/build/CMakeCache.txt') -Pattern '^CMAKE_LINKER:FILEPATH=(.+)$' | Select-Object -First 1
        if (-not $lk) { 'no CMAKE_LINKER in out/build/CMakeCache.txt'; $global:LASTEXITCODE = 1; return }
        $dumpbin = Join-Path (Split-Path $lk.Matches[0].Groups[1].Value) 'dumpbin.exe'
        $bad = 0
        foreach ($f in 'astroray_core.dll', 'astroray_cli.exe') {
          $o = & $dumpbin /dependents (Join-Path $root "out/bin/$f")
          $names = @($o | ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1] } })
          if ($LASTEXITCODE -ne 0 -or $names.Count -eq 0) { "$f`: dumpbin listed no dependencies"; $bad++; continue }
          "$f`: $($names -join ' ')"
          foreach ($n in $names) {
            # The CLI imports the engine DLL beside it; everything else must be a system DLL.
            $ok = ($n -match '^(api|ext)-ms-win-') -or (Test-Path (Join-Path $env:SystemRoot "System32\$n")) -or
                  ($n -match '^astroray_' -and (Test-Path (Join-Path $root "out/bin/$n")))
            if ($n -match '^(vcruntime|msvcp|tbb|nvcuda|cudart)' -or -not $ok) { "$f imports $n, which is not a system DLL"; $bad++ }
          }
        }
        $global:LASTEXITCODE = $bad } },
    @{ Name = 'cpu gate tests'; Tag = 'cpu'; Lock = $true; Identity = $exe; Run = { & $exe --tier=gate } },
    @{ Name = 'gpu gate tests'; Tag = 'local'; Lock = $true; Identity = $gpuExe; Run = { & $gpuExe --tier=gate } }
  )
  function Invoke-GateStep($s) {
    if ($s.Lock) { $script:waited += Enter-ArTreeLock (Join-Path $root 'out/build') }
    try { $r = Step $s.Name $s.Run } finally { if ($s.Lock) { Exit-ArTreeLock } }
    if ($r.Code -ne 0) {
      $fl = @($r.Out | Where-Object { $_ -match '^(FAIL|kernelgate:)' })
      if ($fl.Count -gt 0) { ($fl | Select-Object -First 6) | ForEach-Object { Write-Host $_ } } else { Show-Tail $r }
      $why = if ($fl.Count -gt 0) { "$($s.Name): $($fl[0])" } else { "$($s.Name) failed (out/check/$Tier.log)" }
      Fail ($why.Substring(0, [math]::Min($why.Length, 140)))
    }
    if ($s.Echo) { $r.Out | Where-Object { $_ -match $s.Echo } | ForEach-Object { Write-Host $_ } }
    if ($s.Identity) { Test-Identity $r $s.Name }
  }

  # Budgets this tier enforces, from the machine copy of 0009 d13.
  $q = "import tomllib,json;d=tomllib.load(open('tools/budgets.toml','rb'))['budget'];" +
       "print(json.dumps({b['id']:b['limit'] for b in d if 'limit' in b}))"
  $lim = (& $py -I -c $q) | ConvertFrom-Json
  $budget = if ($isCi -or $Tier -eq 'full') { $null } elseif ($Tier -eq 'gate') { $gateBudget } else { [double]$lim.'B-08' }
  $perTest = [double]$lim.'B-12'

  if ($Tier -eq 'gate') {
    $r = Step 'fast' { & pwsh -NoProfile -File tools/check.ps1 fast }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'fast failed (out/check/fast.log)' }
    foreach ($s in $gateSteps) { Invoke-GateStep $s }
  } elseif ($Tier -eq 'full') {
    $r = Step 'gate' { & pwsh -NoProfile -File tools/check.ps1 gate @dargs }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'gate failed (out/check/gate.log)' }
    # The rows gated full (0002 D10, 0009 d18), all rules, after the gate's own pass.
    $script:waited += Enter-ArTreeLock (Join-Path $root 'out/build')
    try { $r = Step 'kernelgate full' { & $py -I tools/kernelgate.py --full @dargs } } finally { Exit-ArTreeLock }
    if ($r.Code -ne 0) {
      ($r.Out | Where-Object { $_ -match '^(FAIL|kernelgate:)' } | Select-Object -First 6) | ForEach-Object { Write-Host $_ }
      Fail ('kernelgate full: ' + ($r.Out | Where-Object { $_ -like 'FAIL*' } | Select-Object -First 1))
    }
    $r = Step 'build probe' { & pwsh -NoProfile -File tools/build.ps1 -Target astroray_kernels_gpu_probe }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'probe build failed (out/check/build.log)' }
    $r = Step 'probe registers' { & $py -I tools/kernelgate.py --probe }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'probe registers failed' }
    # Warnings only (0002 D10, ruling 17): shown, never a failure.
    $r.Out | Where-Object { $_ -like 'WARNING*' } | ForEach-Object { Write-Host $_ }
  } else {
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
      Show-Tail $r 5
      Fail 'build failed (out/check/build.log)'
    }

    $waited += Enter-ArTreeLock (Join-Path $root 'out/build')
    if (-not (Test-Path $exe)) { Fail "missing $exe" }
    $r = Step 'tests' { & $exe --tier=fast --duration=true }
    Exit-ArTreeLock
    if ($r.Code -ne 0) {
      Show-Tail $r
      Fail "tests failed (out/check/$Tier.log)"
    }
    Test-Identity $r 'binary'
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
      # The lint self-test proves every lint section still catches its seeded cases;
      # fast runs only the repository lint, so a section can rot without it.
      $r = Step 'lint self-test' { & $py -I tools/lint.py --self-test }
      if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'lint self-test failed (out/check/ci.log)' }
      $r = Step 'build cpu' { & pwsh -NoProfile -File tools/build.ps1 -Preset cpu -NoMachineLock }
      if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'cpu build failed (out/check/build.log)' }
      $r = Step 'fetch deps' { & pwsh -NoProfile -File tools/fetch.ps1 deps }
      if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'fetch deps failed' }
      # Every CPU gate step a later task tags `cpu` reaches hosted CI through this loop.
      foreach ($s in $gateSteps | Where-Object { $_.Tag -eq 'cpu' }) { Invoke-GateStep $s }
      # Headless sync suite (0007 d16, 0009 d12) against the pinned bpy wheel; ci.yml installs it from
      # .github/ci-requirements.txt, and a local CI=true run installs it here when it is missing.
      $r = Step 'headless sync tests' {
        & $py -I -c 'import bpy' 2>$null
        if ($LASTEXITCODE -ne 0) {
          $line = Select-String -Path .github/ci-requirements.txt -Pattern '^bpy==' | Select-Object -First 1
          if (-not $line) { 'bpy is not pinned in .github/ci-requirements.txt'; $global:LASTEXITCODE = 1; return }
          $req = Join-Path $root 'out/check/bpy-requirements.txt'
          Set-Content -Path $req -Value $line.Line
          & $py -I -m pip install --require-hashes --no-deps -r $req
          if ($LASTEXITCODE -ne 0) { return }
        }
        & $py -I tests/blender/headless/run_all.py
      }
      if ($r.Code -ne 0) { Show-Tail $r 10; Fail 'headless sync tests failed (out/check/ci.log)' }
      # build.ps1 sets the MSVC environment only in its own process, so read the compiler version
      # CMake recorded when it configured; it is the exact cl that built the tests.
      $cm = Get-ChildItem out/build/CMakeFiles -Recurse -Filter CMakeCXXCompiler.cmake | Select-Object -First 1
      $m = if ($cm) { Select-String -Path $cm.FullName -Pattern 'set\(CMAKE_CXX_COMPILER_VERSION "([^"]+)"' } else { $null }
      if (-not $m) { throw 'cl version not found in out/build/CMakeFiles' }
      $script:clVer = $m.Matches[0].Groups[1].Value
      Write-Host "cl $script:clVer"
      Add-Content -Path $log -Value "cl $script:clVer"
    }
  }
} catch {
  Fail ($_.Exception.Message)
}
$why = ''
if ($Tier -eq 'gate') {
  $kp = ($script:ArSteps | Where-Object { $_.name -in $kernelSteps } | Measure-Object -Property seconds -Sum).Sum
  $why = "kernel part {0:N1} s of {1} s" -f $kp, $kernelBudget
  if ($kp -gt $kernelBudget) { Fail "B-09: kernel part $([math]::Round($kp, 1)) s over $kernelBudget s" }
  if ($script:ArClock.Elapsed.TotalSeconds -gt $budget) { Fail "B-09: tier over $budget s ($why)" }
} elseif ($Tier -eq 'fast' -and $script:ArClock.Elapsed.TotalSeconds -gt $budget) {
  $why = "B-08: tier over $budget s"
}
$ok = Complete-ArSummary -Pass $true -Waited $waited -Budget $budget -Reason $why
if ($isCi) {
  $jp = Join-Path $root 'out/check/ci.json'
  $j = Get-Content $jp -Raw | ConvertFrom-Json
  $j | Add-Member -NotePropertyName cl -NotePropertyValue "$script:clVer"
  $j | ConvertTo-Json -Depth 4 | Set-Content -Path $jp
}
Exit-ArMachineLock
exit ($(if ($ok) { 0 } else { 1 }))
