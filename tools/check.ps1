# SPDX-License-Identifier: Apache-2.0
# tools/check.ps1 fast|gate|full|ci [-Declared "name:regs=+0,stack=+16;..."]
# fast: the 10 s loop (0009 d10): lint, incremental CPU test build (outside the timed window), deterministic tests.
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
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
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
  # A GPU test executable under a time limit: past it the process (by its own PID, 0009 d8) is
  # stopped and the step fails naming the last case the executable started (its "case start"
  # lines), so a hang costs minutes. Output goes through a file so the tail survives the stop.
  # Text, not a function: grouped steps run in jobs, which rebuild it from $vars.
  $timedText = @'
param([string]$file, [string]$arg, [int]$limitS, [string]$capture)
$p = Start-Process -FilePath $file -ArgumentList $arg -NoNewWindow -PassThru -RedirectStandardOutput $capture -RedirectStandardError "$capture.err"
$null = $p.Handle
$done = $p.WaitForExit($limitS * 1000)
if (-not $done) { Stop-Process -Id $p.Id -Force; $null = $p.WaitForExit(30000) }
Get-Content $capture
Get-Content "$capture.err"
if ($done) { $global:LASTEXITCODE = $p.ExitCode; return }
$last = Select-String -Path $capture -Pattern '^case start ' | Select-Object -Last 1
"FAIL timeout: $(Split-Path -Leaf $file) $arg stopped after $limitS s; last case started: $(if ($last) { $last.Line } else { 'none' })"
$global:LASTEXITCODE = 1
'@
  $runTimed = [scriptblock]::Create($timedText)

  # Group: steps with the same Group, adjacent in this table, run concurrently (CPU device against GPU
  # device; see Invoke-GateGroup). 'aov agreement', 'integrator cuda' (tripwire) and 'material compile'
  # are timing-sensitive or use both devices and stay alone.
  # The gate steps (name, tag, kernel part is summed by $kernelSteps, Lock = hold the tree lock,
  # Identity = the exe must print this commit's build line). Each body fails by its exit code.
  $gateSteps = @(
    @{ Name = 'build dev'; Tag = 'local'; Run = { & pwsh -NoProfile -Command "& tools/build.ps1 -Target all,astroray_kernels_gpu_nofma; exit `$LASTEXITCODE" } },
    # 0008 V9 compile part (050): the negative-compile cases are outside ALL, so only this step runs them.
    @{ Name = 'negative compile'; Tag = 'cpu'; Run = { & pwsh -NoProfile -File tools/build.ps1 -Target astroray_negative_compile -Preset $(if ($isCi) { 'cpu' } else { 'dev' }) -NoMachineLock } },
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
    # 0009 d19 (070): recordings are generated here, never committed; later steps that render a scene
    # (analytic harness, A1) come after this one. Up-to-date recordings are skipped by source hash.
    @{ Name = 'scenes'; Tag = 'cpu'; Echo = '^scenes:'; Run = { & $py -I tools/scenes.py } },
    # 0009 d11, d16 (072): analytic scenes on CPU and GPU at 32x32 through the CLI, after 'scenes'.
    @{ Name = 'analytic cpu'; Group = 'g1'; Tag = 'cpu'; Lock = $true; Echo = '^(PASS|FAIL|crossings) analytic'; Run = {
        & $py -I tools/analytic.py --gate --device cpu } },
    @{ Name = 'analytic cuda'; Group = 'g1'; Tag = 'local'; Lock = $true; Echo = '^(PASS|FAIL|crossings) analytic'; Run = {
        & $py -I tools/analytic.py --gate --device cuda } },
    # 0009 d17(iv) (077): deterministic AOVs of scene D2, CPU against GPU; D1 runs in the analytic steps.
    @{ Name = 'aov agreement'; Tag = 'local'; Lock = $true; Echo = '^(PASS|FAIL|aov) '; Run = {
        & $py -I tools/aov_agree.py --scene d2_aov_geometry } },
    # ABI tests that need a CUDA device (115): gpu_test_*.py, outside the cpu step's test_*.py, so hosted
    # CI never runs them. After 'scenes' because they render its recordings; they overlap 'integrator cpu' (GPU against CPU).
    @{ Name = 'gpu abi tests'; Tag = 'local'; Lock = $true; Group = 'g2'; Echo = '^cli render cuda'; Run = {
        if (Test-Path 'tests/abi') { & $py -I -m unittest discover -s tests/abi -p 'gpu_test_*.py' } } },
    # 076: Russian roulette furnace, shadow lifetime, closed box, radiance scale (0004 D2, D4/D5, D28;
    # 0003 check 15) and the B-16 work counters against tests/budgets/counters.toml; the tripwire on cuda.
    @{ Name = 'integrator cpu'; Group = 'g2'; Tag = 'cpu'; Lock = $true; Echo = '^(PASS|FAIL) integrator'; Run = {
        & $py -I tools/integrator_checks.py --gate --device cpu } },
    @{ Name = 'integrator cuda'; Tag = 'local'; Lock = $true; Echo = '^(PASS|FAIL) integrator'; Run = {
        & $py -I tools/integrator_checks.py --gate --device cuda } },
    @{ Name = 'cpu gate tests'; Group = 'g3'; Tag = 'cpu'; Lock = $true; Identity = $exe; Run = { & $exe --tier=gate } },
    # Time limit 270 s: after 132's fix the tier took 12.6-24.1 s over 100 runs under the machine lock;
    # 10x the slowest run, rounded up to a multiple of 30 s.
    @{ Name = 'gpu gate tests'; Group = 'g3'; Tag = 'local'; Lock = $true; Identity = $gpuExe;
       Run = { & $runTimed $gpuExe '--tier=gate' 270 (Join-Path $root 'out/check/gpu-gate-tests.out') } },
    # B-24 compile row (0009 d13, 0005 V7): the test prints the median compile time of a fixed
    # 100-node graph; the limit is the "ms per 100 nodes" figure of the row in budgets.toml.
    @{ Name = 'material compile'; Tag = 'cpu'; Lock = $true; Identity = $exe; Echo = '^B-24 compile:';
       Run = { & $exe --tier=gate '--test-case=compiler_outputs/B-24*' };
       Check = { param($r)
         $q = "import tomllib,re;t=next(b['text'] for b in tomllib.load(open('tools/budgets.toml','rb'))['budget'] if b['id']=='B-24');" +
              "m=re.search(r'([0-9.]+) ms per 100 nodes',t);print(m.group(1) if m else '')"
         $limit = (& $py -I -c $q | Select-Object -First 1)
         if (-not $limit) { Fail 'material compile: B-24 in tools/budgets.toml names no "ms per 100 nodes" limit' }
         $m = $r.Out | Select-String -Pattern '^B-24 compile: ([0-9.]+) ms per 100 nodes' | Select-Object -First 1
         if (-not $m) { Fail 'material compile: the test printed no B-24 compile line' }
         $ms = [double]$m.Matches[0].Groups[1].Value
         if ($ms -gt [double]$limit) { Fail "material compile: $ms ms per 100 nodes is above B-24's $limit ms" }
         Write-Host "B-24 compile: $ms ms per 100 nodes (limit $limit ms)" } },
    # 0009 d11, d17a (084): one Blender start installs the staged extension zip, runs the 083 material tests,
    # renders A1's .blend through F12 on the CPU device and checks it against A1's truth and the builder's
    # recording (exporter equivalence). The smoke builds the zip itself; Invoke-ArBlender's own output goes to the log.
    @{ Name = 'blender smoke'; Tag = 'local'; Echo = '^(RESULT smoke|Ran |OK$|FAILED)'; Run = {
        $r = Invoke-ArBlender -Script 'tests/blender/smoke_a1.py' -Args @('--with-materials') -TimeoutS 400 6>$null
        foreach ($f in $r.Log, "$($r.Log).err") { if (Test-Path $f) { Get-Content $f } }
        $global:LASTEXITCODE = $r.ExitCode } }
  )
  # Judge one finished step: its failure names itself, then Identity and Check/Echo as before.
  function Complete-GateStep($s, $r) {
    if ($r.Code -ne 0) {
      $fl = @($r.Out | Where-Object { $_ -match '^(FAIL|kernelgate:)' })
      if ($fl.Count -gt 0) { ($fl | Select-Object -First 6) | ForEach-Object { Write-Host $_ } } else { Show-Tail $r }
      $why = if ($fl.Count -gt 0) { "$($s.Name): $($fl[0])" } else { "$($s.Name) failed (out/check/$Tier.log)" }
      Fail ($why.Substring(0, [math]::Min($why.Length, 140)))
    }
    if ($s.Identity) { Test-Identity $r $s.Name }
    if ($s.Check) { & $s.Check $r } elseif ($s.Echo) { $r.Out | Where-Object { $_ -match $s.Echo } | ForEach-Object { Write-Host $_ } }
  }
  function Invoke-GateStep($s) {
    if ($s.Lock) { $w = Enter-ArTreeLock (Join-Path $root 'out/build'); $script:waited += $w; Add-ArExcluded $w }
    try { $r = Step $s.Name $s.Run } finally { if ($s.Lock) { Exit-ArTreeLock } }
    Complete-GateStep $s $r
  }
  # Steps sharing a Group touch different devices and no output, so they run as concurrent jobs under
  # the one machine lock (0009 d11). Each job is its own process and returns its output and time;
  # results are judged in table order, so a failure names its own step. Timing-sensitive steps (the
  # CUDA tripwire, the B-24 compile time) carry no Group and run alone.
  function Invoke-GateGroup($members) {
    if ($members.Count -eq 1) { Invoke-GateStep $members[0]; return }
    if (@($members | Where-Object { $_.Lock }).Count -gt 0) {
      $w = Enter-ArTreeLock (Join-Path $root 'out/build'); $script:waited += $w; Add-ArExcluded $w
    }
    try {
      $pre = { param($body, $v)
        # As in the parent (Stop): a PowerShell error in a grouped step fails it, as it would alone.
        $ErrorActionPreference = 'Stop'
        Set-Location $v.root
        $py = $v.py; $dargs = $v.dargs; $exe = $v.exe; $gpuExe = $v.gpuExe; $root = $v.root; $isCi = $v.isCi
        $runTimed = [scriptblock]::Create($v.timedText)
        $t = [Diagnostics.Stopwatch]::StartNew()
        $global:LASTEXITCODE = 0
        $out = & ([scriptblock]::Create($body)) 2>&1 | ForEach-Object { "$_" }
        [pscustomobject]@{ Code = $LASTEXITCODE; Out = @($out); Seconds = $t.Elapsed.TotalSeconds } }
      $vars = @{ root = "$root"; py = $py; dargs = $dargs; exe = $exe; gpuExe = $gpuExe; isCi = $isCi; timedText = $timedText }
      $jobs = @($members | ForEach-Object { Start-Job -ScriptBlock $pre -ArgumentList $_.Run.ToString(), $vars })
      $null = $jobs | Wait-Job
      $res = @(for ($i = 0; $i -lt $members.Count; $i++) {
        $o = @(Receive-Job $jobs[$i] -ErrorAction Continue 2>&1)
        $r = $o | Where-Object { $_ -is [pscustomobject] -and $null -ne $_.Seconds } | Select-Object -Last 1
        if (-not $r) { $r = [pscustomobject]@{ Code = 1; Out = @($o | ForEach-Object { "$_" }); Seconds = 0.0 } }
        Remove-Job $jobs[$i] -Force
        $r })
    } finally { if (@($members | Where-Object { $_.Lock }).Count -gt 0) { Exit-ArTreeLock } }
    for ($i = 0; $i -lt $members.Count; $i++) {
      Add-ArStep $members[$i].Name $res[$i].Seconds ($res[$i].Code -eq 0)
      Add-Content -Path $log -Value $res[$i].Out
    }
    for ($i = 0; $i -lt $members.Count; $i++) { Complete-GateStep $members[$i] $res[$i] }
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
    $i = 0
    while ($i -lt $gateSteps.Count) {
      $g = $gateSteps[$i].Group
      $n = 1
      if ($g) { while ($i + $n -lt $gateSteps.Count -and $gateSteps[$i + $n].Group -eq $g) { $n++ } }
      Invoke-GateGroup @($gateSteps[$i..($i + $n - 1)])
      $i += $n
    }
  } elseif ($Tier -eq 'full') {
    $r = Step 'gate' { & pwsh -NoProfile -File tools/check.ps1 gate @dargs }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'gate failed (out/check/gate.log)' }
    # API surface inside Blender, where the bundled openvdb exists (0007 d46); ci checks the rest on the wheel.
    $r = Step 'blender api_surface' {
      $b = Invoke-ArBlender -Script 'tools/api_surface.py' -TimeoutS 120 6>$null
      foreach ($f in $b.Log, "$($b.Log).err") { if (Test-Path $f) { Get-Content $f } }
      $global:LASTEXITCODE = $b.ExitCode }
    $r.Out | Where-Object { $_ -match '^(api_surface:|MISSING|UNCHECKED)' } | ForEach-Object { Write-Host $_ }
    if ($r.Code -ne 0) { Fail 'blender api_surface failed (out/check/full.log)' }
    # Image sync and node values through Blender, full only (about 40 s does not fit B-09).
    # Not $t: Step's own stopwatch of that name is visible inside the block (dynamic scope).
    foreach ($bt in 'test_images', 'test_node_values') {
      $r = Step "blender $bt" {
        $b = Invoke-ArBlender -Script "tests/blender/$bt.py" -TimeoutS 400 6>$null
        foreach ($f in $b.Log, "$($b.Log).err") { if (Test-Path $f) { Get-Content $f } }
        $global:LASTEXITCODE = $b.ExitCode }
      $r.Out | Where-Object { $_ -match '^(Ran |OK$|FAILED|FAIL:|ERROR:)' } | ForEach-Object { Write-Host $_ }
      if ($r.Code -ne 0) { Fail "blender $bt failed (out/check/full.log)" }
    }
    # Windowed-Blender scripts: every tests/blender/gui_*.py, one run and one line each. Each
    # script drives itself by timers and ends the process; a non-zero exit fails the tier.
    foreach ($gf in (Get-ChildItem 'tests/blender' -Filter 'gui_*.py' | Sort-Object Name)) {
      $gn = $gf.BaseName
      $r = Step "blender $gn" {
        $b = Invoke-ArBlender -Gui -Script "tests/blender/$gn.py" -TimeoutS 400 6>$null
        foreach ($f in $b.Log, "$($b.Log).err") { if (Test-Path $f) { Get-Content $f } }
        $global:LASTEXITCODE = $b.ExitCode }
      $r.Out | Where-Object { $_ -match '^(Ran |OK$|FAILED|FAIL:|ERROR:)' } | ForEach-Object { Write-Host $_ }
      if ($r.Code -ne 0) { Fail "blender $gn failed (out/check/full.log)" }
    }
    # The probe build first, so kernelgate full reads fresh capped probe cubins (kernels_probe_capped).
    $r = Step 'build probe' { & pwsh -NoProfile -File tools/build.ps1 -Target astroray_kernels_gpu_probe }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'probe build failed (out/check/build.log)' }
    # The rows gated full (0002 D10, 0009 d18), all rules, after the gate's own pass.
    $w = Enter-ArTreeLock (Join-Path $root 'out/build'); $script:waited += $w; Add-ArExcluded $w
    try { $r = Step 'kernelgate full' { & $py -I tools/kernelgate.py --full @dargs } } finally { Exit-ArTreeLock }
    if ($r.Code -ne 0) {
      ($r.Out | Where-Object { $_ -match '^(FAIL|kernelgate:)' } | Select-Object -First 6) | ForEach-Object { Write-Host $_ }
      Fail ('kernelgate full: ' + ($r.Out | Where-Object { $_ -like 'FAIL*' } | Select-Object -First 1))
    }
    # The GPU suite's full tier (0009 B-17 launch duration, 099): a whole tier, a few seconds today.
    # Time limit 60 s: after 132's fix the tier took 3.1 s (one run); 10x the slowest run, rounded up to a
    # multiple of 30 s, as for the gate tier.
    $r = Step 'gpu full tests' { & $runTimed $gpuExe '--tier=full' 60 (Join-Path $root 'out/check/gpu-full-tests.out') }
    if ($r.Code -ne 0) { Show-Tail $r 8; Fail 'gpu full tests failed (out/check/full.log)' }
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

    if ($isCi) {
      # Configure resolves the fetched dependency cache (cmake/deps.cmake), so a fresh
      # runner fetches before the first build.
      $r = Step 'fetch deps' { & pwsh -NoProfile -File tools/fetch.ps1 deps }
      if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'fetch deps failed' }
    }
    $r = Step 'build' { & pwsh -NoProfile -File tools/build.ps1 -Target astroray_tests_cpu -Preset $(if ($isCi) { 'cpu' } else { 'dev' }) -NoMachineLock -Jobs 4 }
    $bj = Join-Path $root 'out/check/build.json'
    if (Test-Path $bj) { $waited += [double](Get-Content $bj -Raw | ConvertFrom-Json).waited }
    # B-08 times lint and tests. The build step (its lock wait included) leaves the tier clock: a rebuild
    # after a header edit or a rebase is not this change's test time; build time has rows B-02 and B-03.
    Add-ArExcluded ([double]$script:ArSteps[-1].seconds)
    if ($r.Code -ne 0) {
      Show-Tail $r 5
      Fail 'build failed (out/check/build.log)'
    }

    $w = Enter-ArTreeLock (Join-Path $root 'out/build'); $waited += $w; Add-ArExcluded $w
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
      # API surface (0007 d46): every bpy name the addon reads exists in the pinned wheel; the bundled openvdb
      # is not in the wheel and is checked inside Blender by the full tier.
      $r = Step 'api surface' { & $py -I tools/api_surface.py }
      Write-Host ($r.Out | Where-Object { $_ -match '^api_surface:' })
      if ($r.Code -ne 0) { Show-Tail $r 10; Fail 'api surface failed (out/check/ci.log)' }
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
  if ((Get-ArSeconds) -gt $budget) { Fail "B-09: tier over $budget s ($why)" }
} elseif ($Tier -eq 'fast' -and (Get-ArSeconds) -gt $budget) {
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
