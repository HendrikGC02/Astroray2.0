# SPDX-License-Identifier: Apache-2.0
# tools/check.ps1 fast|gate|full|ci|profile [-Declared "name:regs=+0,stack=+16;..."]
# fast: the 10 s loop (0009 d10): lint, incremental CPU test build (outside the timed window), deterministic tests.
# No machine lock (d8); the tree lock on out/build is held while the tests run (build.ps1 takes
# it for the build itself). Limits come from tools/budgets.toml: B-08 (tier), B-12 (one test).
# Output: at most 20 lines, last line "PASS fast <s> s (budget 10 s, waited <w> s)" or "FAIL fast ...";
# JSON in out/check/<tier>.json, log in out/check/<tier>.log; non-zero exit on failure.
# gate (0009 d11, d18; machine lock): fast, then the gate steps declared below, against B-09
# (180 s in all, 120 s for the kernel part). Every gate step carries a tag: `cpu` (needs neither a
# GPU nor Blender) or `local`; a task that adds a gate step tags it.
# -Declared is the task's declared kernel delta, printed next to the measured one.
# Machines (owner decision 47, 0009 d13): a timing row over its limit is judged for the machine it ran on (lib/machine.ps1
# Resolve-ArTimingRow): the reference machine fails it, a profiled machine fails it against its own derived limit, any
# other machine prints `off-reference <machine id> <row>: ...` and passes. Hosted ci has no machine id.
# profile (lead, quiet machine, not the reference): five fast runs after a warm-up, the median of their tier times into
# bench/machines.toml as this machine's B-08; -Reference (lead, on the reference machine) writes the same measurement
# as [reference_median] with its date and sha. The measuring runs (-Profiling on the fast tier) do not judge B-08.
# full: gate plus the probe build; uncapped registers are warnings, never failures (0002 D10).
# ci (0009 d12, d24; hosted CI): fast, cpu build, deps, the headless bpy suite and the API-surface check; the gate
# steps stay local (owner decision 42: every landing gate runs them on this PC). No budget here (B-11); logs the cl version.
# -Landing (gate only; land.ps1 passes it): the gate holds the fast window for the whole tier, so builds of other
# worktrees wait while a landing is gated (0009 d8). A gate an implementer runs holds it for the fast step and
# the compile timing step only.
# Gate time (B-09, ruling 94) leaves out the gate's build steps (`build dev`, the nofma build and the build inside
# the fast step); their seconds are printed beside the tier total.
param([Parameter(Mandatory, Position = 0)][string]$Tier, [string]$Declared = '', [switch]$Landing, [switch]$Reference, [switch]$Profiling)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/summary.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
. (Join-Path $PSScriptRoot 'lib/testrun.ps1')
. (Join-Path $PSScriptRoot 'lib/load.ps1')
. (Join-Path $PSScriptRoot 'lib/machine.ps1')
if ($Tier -notin @('fast', 'gate', 'full', 'ci', 'profile')) {
  Write-Host "FAIL check: unknown tier '$Tier' (fast, gate, full, ci, profile)"
  exit 2
}
if ($Landing -and $Tier -ne 'gate') {
  Write-Host "FAIL check: -Landing belongs to the gate tier, not '$Tier'"
  exit 2
}
if ($Reference -and $Tier -ne 'profile') {
  Write-Host "FAIL check: -Reference belongs to the profile tier, not '$Tier'"
  exit 2
}
# -Profiling waives B-08 for the measuring runs of the profile tier only, which marks the child's environment.
if ($Profiling -and -not ($Tier -eq 'fast' -and $env:ASTRORAY_PROFILE_RUN -eq '1')) {
  Write-Host "FAIL check: -Profiling is the profile tier's own switch; check fast does not waive B-08 on request"
  exit 2
}

# Locks first; the tier clock starts after them so waits are excluded (0009 d8).
$waited = 0.0
if ($Tier -in @('gate', 'full', 'profile')) { $waited = Enter-ArMachineLock -Job $(if ($Tier -eq 'profile') { 'check-gate' } else { "check-$Tier" }) }   # a profile holds the lock for minutes, like a gate
Start-ArSummary $Tier
$budget = $null
$isCi = ($Tier -eq 'ci')
$log = Join-Path $root "out/check/$Tier.log"
# B-09 (0009 d13): `check gate` <= 3 min, of which the kernel part (device builds, kernel gate,
# compile timing, GPU gate tests; 0002 D10) <= 120 s.
$gateBudget = 180.0
$kernelBudget = 120.0
$kernelSteps = @('build dev', 'kernelgate', 'compile timing', 'gpu gate tests')
# Seconds of the gate's own build steps: out of the tier clock (ruling 94), printed beside the total. The kernel
# part keeps counting `build dev`, which is a device build (0002 D10). `compile timing` is such a step too (ruling 100):
# it compiles the changed kernels against B-05's per-TU limit, so it is judged there, not on the tier clock.
$script:buildSeconds = 0.0
function Add-GateBuild([double]$Seconds) { $script:buildSeconds += $Seconds; Add-ArExcluded $Seconds }

function Fail([string]$why) {
  $null = Complete-ArSummary -Pass $false -Waited $waited -Budget $budget -Reason $why
  Exit-ArTreeLock
  Exit-ArFastWindow
  Exit-ArMachineLock
  exit 1
}
# Run a step, return output lines; its time goes into the summary.
function Step([string]$name, [scriptblock]$body) {
  Set-ArLockStep $name
  $t = [Diagnostics.Stopwatch]::StartNew()
  $global:LASTEXITCODE = 0
  $out = & $body 2>&1 | ForEach-Object { "$_" }
  $code = $LASTEXITCODE
  Add-ArStep $name $t.Elapsed.TotalSeconds ($code -eq 0)
  Add-Content -Path $log -Value $out
  $res = [pscustomobject]@{ Code = $code; Out = @($out) }
  # Every step of the full tier (the nested gate, Blender suites, kernelgate full, the GPU and CPU full tests, B-23): the
  # off-reference lines a step printed are shown, never only logged. The gate tier shows them in Complete-GateStep.
  if ($Tier -eq 'full') { Show-OffReference $res }
  return $res
}
# A Blender start inside the tier: its machine-lock wait (0 s when nested under the tier's own hold) is not
# work (0009 d8), so it joins `waited` and leaves the tier clock. Prints the Blender log for the step.
function Invoke-TierBlender([string]$Script, [string[]]$BArgs = @(), [int]$TimeoutS = 300, [switch]$Gui) {
  $b = Invoke-ArBlender -Script $Script -Args $BArgs -TimeoutS $TimeoutS -Gui:$Gui 6>$null
  $script:waited += $b.Waited
  Add-ArExcluded $b.Waited
  foreach ($f in $b.Log, "$($b.Log).err") { if (Test-Path $f) { Get-Content $f } }
  $global:LASTEXITCODE = $b.ExitCode
}
# One timing row against its limit on this machine (lib/machine.ps1). Prints the off-reference or profile line (and
# the reason when the machine id cannot be read); the result's Verdict is PASS, FAIL or OFF and Limit the limit in force.
function Resolve-TierRow([string]$Row, [double]$Measured, [double]$Limit, [string]$Unit = 's') {
  $v = Resolve-ArTimingRow -Row $Row -Measured $Measured -Limit $Limit -Unit $Unit -NoMachine:$isCi
  if ($v.Line) { Write-ArOffReference $v.Line }
  return $v
}
# The off-reference and profile lines a step printed (tests with a timing CHECK of their own, B-05, B-16): visible in the
# tier's output, never only in a log (a rule that does not gate must not be silent).
function Show-OffReference($r) {
  @($r.Out | Where-Object { $_ -match '^(off-reference|profile) ' } | Select-Object -Unique) | ForEach-Object { Write-Host $_ }
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
  if ($Tier -eq 'profile') {
    # The machine's own median of B-08 (bench/machines.toml, owner decision 47 c), measured by the lead on a quiet
    # machine: five fast runs after a warm-up. -Reference does the same on the reference machine and writes
    # [reference_median]. The runs are -Profiling: they do not judge B-08 against the profile they replace.
    $id = Get-ArMachineId
    $isRef = ($id -eq (Get-ArReferenceMachineId))
    if ($Reference -and -not $isRef) { Fail "profile -Reference: $id is not the reference machine" }
    if (-not $Reference -and $isRef) { Fail "profile: $id is the reference machine; measure it with -Reference" }
    $load = @(Get-ArForeignLoad $script:ArLoadNames)
    if ($load.Count) { Fail "profile: foreign load ($($load -join ', ')); a profile needs a quiet machine" }
    $secs = @()
    $env:ASTRORAY_PROFILE_RUN = '1'   # the marker `check fast -Profiling` requires
    for ($i = 0; $i -lt 6; $i++) {
      $r = Step "fast run $($i + 1)" { & pwsh -NoProfile -File tools/check.ps1 fast -Profiling }
      $fj = Join-Path $root 'out/check/fast.json'
      $fr = if (Test-Path $fj) { Get-Content $fj -Raw | ConvertFrom-Json } else { $null }
      if ($r.Code -ne 0 -or -not $fr -or -not $fr.pass) { Remove-Item Env:ASTRORAY_PROFILE_RUN -ErrorAction SilentlyContinue; Show-Tail $r 5; Fail "profile: fast run $($i + 1) failed (out/check/profile.log)" }
      $secs += [double]$fr.seconds
    }
    Remove-Item Env:ASTRORAY_PROFILE_RUN -ErrorAction SilentlyContinue
    Write-Host "profile $id B-08: six runs $($secs -join ', ') s; the first is dropped"
    $secs = @($secs | Select-Object -Skip 1)   # the first run warms the caches
    $med = @($secs | Sort-Object)[[int][math]::Floor($secs.Count / 2)]
    if ($Reference) {
      $sha = (& git rev-parse --short HEAD).Trim()
      $note = "B-08, seconds: median of five check fast runs after a dropped warm-up, on $id, $(Get-Date -Format 'yyyy-MM-dd'), sha $sha (check.ps1 profile -Reference; runs $($secs -join ', '))"
      & $py -I tools/machine_id.py record-reference --row 'B-08' --value $med --note $note
    } else {
      & $py -I tools/machine_id.py record --id $id --row 'B-08' --value $med
    }
    if ($LASTEXITCODE -ne 0) { Fail 'profile: bench/machines.toml was not written' }
    Write-Host "profile $id B-08: median $med s of $($secs.Count) runs written to bench/machines.toml"
    $ok = Complete-ArSummary -Pass $true -Waited $waited -Budget $null
    Exit-ArMachineLock
    exit 0
  }
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

  # The two long CPU steps run as n shard processes side by side (they use under a tenth of the 16
  # threads alone). Each shard prints through a file, as in $timedText, and the outputs are joined
  # in shard order; the exit code is the first non-zero one. Text, for the same reason as $timedText.
  $shardText = @'
param([string]$file, [string[]]$argLines, [string]$capture)
# A unique name per run, so shards a failed run left behind cannot hold the next run's files.
$capture = "$capture-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$ps = @()
$code = 0
try {
  # An empty stdin file: python started with no console (inside a grouped job) and an inherited stdin never exits.
  $null = New-Item -ItemType File -Force "$capture.in"
  for ($i = 0; $i -lt $argLines.Count; $i++) {
    $p = Start-Process -FilePath $file -ArgumentList $argLines[$i] -NoNewWindow -PassThru -RedirectStandardInput "$capture.in" -RedirectStandardOutput "$capture.$i" -RedirectStandardError "$capture.$i.err"
    $null = $p.Handle
    $ps += $p
  }
  for ($i = 0; $i -lt $ps.Count; $i++) {
    $ps[$i].WaitForExit()
    if ($ps[$i].ExitCode -ne 0 -and $code -eq 0) { $code = $ps[$i].ExitCode }
    Get-Content "$capture.$i"
    Get-Content "$capture.$i.err"
  }
} catch {
  "FAIL shards: $_"
  # What the finished shards printed, before the files go.
  for ($i = 0; $i -lt $ps.Count; $i++) { foreach ($f in "$capture.$i", "$capture.$i.err") { if (Test-Path $f) { Get-Content $f } } }
  $code = 1
} finally {
  # By the PIDs this helper started (0009 d8): a throw leaves no shard running; a Ctrl-C of the parent can
  # leave them for the seconds they need to finish.
  foreach ($p in $ps) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
  Remove-Item "$capture.*" -Force -ErrorAction SilentlyContinue
}
$global:LASTEXITCODE = $code
'@
  $runShards = [scriptblock]::Create($shardText)
  # One tier of astroray_tests_cpu as $n shard processes (the gate's `cpu gate tests` step and the fast tier's tests
  # step): cut the tier's cases by the last run's seconds, run the shards, and prove together they ran every case
  # once. Text, for the same reason as $timedText; it reads $exe, $py, $root and $runShards from the caller.
  $cpuTierText = @'
param([string]$tier, [int]$n, [string]$name, [string]$prev, [string]$capture, [string]$extra = '')
$ls = "$capture.list"
$ex = @(if ($extra) { $extra })
& $exe --tier=$tier @ex --list-test-cases | Set-Content $ls
$all = [int]([regex]::Match((Get-Content $ls -Raw), 'filters: (\d+)').Groups[1].Value)
# Ranges balance the last run's per-case seconds and keep same-name cases (the f32 and f64 instances of
# one AR_RT case) in one shard; $extra is a doctest ordering both calls share; tools/gate_table.py --cut fails loudly when it cannot cut.
$cut = @(& $py -I tools/gate_table.py --cut $n --list $ls --prev $prev)
if ($LASTEXITCODE -ne 0) { $cut; $global:LASTEXITCODE = 1; return }
$o = @(& $runShards $exe @($cut | ForEach-Object { $f, $l = $_ -split ' '; "--tier=$tier $extra --duration=true --first=$f --last=$l" }) $capture)
$code = $LASTEXITCODE
$o
# One --duration line per case a shard ran (doctest's summary counts every case passing the filters,
# not the --first/--last range): together the shards run the whole tier.
$ran = @($o | Where-Object { $_ -match '^\s*[0-9]+\.[0-9]+ s: ' }).Count
# Every shard prints its build line first: a shard that died before printing is counted out here.
# (A dead shard also lowers the case count below; naming it first says which process to look at.)
$started = @($o | Where-Object { $_ -match '^build: ' }).Count
if ($started -ne $n) { "FAIL $name shards: $started of $n shards printed a build line"; $code = 1 }
if ($ran -ne $all) { "FAIL $name shards: $ran of $all cases ran"; $code = 1 }
else { "shards: $name $n shards, $ran of $all cases" }
# No shard sees a whole statistical family: sum the executed counts of every family over the shards
# (each process prints `family count <name> <n> <m>`) and fail on any overrun of its m.
$fo = "$capture-families.out"
$o | Set-Content $fo
$fam = @(& $py -I tools/gate_table.py --families $fo)
if ($LASTEXITCODE -ne 0) { $code = 1 }
$fam
$global:LASTEXITCODE = $code
'@
  $runCpuTier = [scriptblock]::Create($cpuTierText)
  # Shard counts: measured in task 212 (docs/BACKLOG.md Gate row). Every case and scene runs exactly once:
  # the bodies below sum the shards' own counts against the whole and fail on a difference.
  $shardsCpuGate = 3
  # Analytic: 3, so the longest CPU scene (A9, 6.6 s with its 1024^2 CPU bake) does not set a shard's wall.
  $shardsAnalytic = 3
  # The fast tier's tests run in 3 shards the same way; hosted ci runs one process: 2-4 cores and timing-sensitive
  # session cases (task 212).
  $shardsFast = if ($isCi) { 1 } else { 3 }

  # Group: steps with the same Group, adjacent in this table, run concurrently (CPU device against GPU
  # device; see Invoke-GateGroup). 'aov agreement', 'integrator cuda' (tripwire) and 'material compile'
  # are timing-sensitive or use both devices and stay alone.
  # The gate steps (name, tag, kernel part is summed by $kernelSteps, Lock = hold the tree lock,
  # Keep = save the output (see Complete-GateStep), Build = a build: its seconds leave the tier clock, Window = timed against a budget beside other worktrees'
  # builds, so the fast window is held exclusively for it,
  # Identity = the exe must print this commit's build line). Each body fails by its exit code.
  $gateSteps = @(
    @{ Name = 'build dev'; Tag = 'local'; Build = $true; Run = { & pwsh -NoProfile -Command "& tools/build.ps1 -Target all,astroray_kernels_gpu_nofma; exit `$LASTEXITCODE" } },
    # 0008 V9 compile part (050): the negative-compile cases are outside ALL, so only this step runs them.
    @{ Name = 'negative compile'; Tag = 'cpu'; Run = { & pwsh -NoProfile -File tools/build.ps1 -Target astroray_negative_compile -Preset dev -NoMachineLock } },
    # The tools' own CPU tests (tests/tools/fast_*.ps1): without a step here no tier would run them.
    @{ Name = 'tools tests'; Tag = 'cpu'; Run = { & pwsh -NoProfile -File tests/tools/run_fast.ps1 } },
    @{ Name = 'kernelgate self-test'; Tag = 'cpu'; Run = { & $py -I tools/kernelgate.py --self-test } },
    @{ Name = 'perf self-test'; Tag = 'cpu'; Run = { & $py -I tools/perf.py --self-test; if ($LASTEXITCODE -eq 0) { & $py -I tools/perf_shares.py --self-test } } },
    @{ Name = 'report self-test'; Tag = 'cpu'; Run = { & $py -I tools/report.py --selftest } },
    @{ Name = 'kernelgate'; Tag = 'local'; Lock = $true; Echo = '^kernelgate: \d+ row'; Run = { & $py -I tools/kernelgate.py @dargs } },
    # B-05 reads the compiler's own time: beside another worktree's build it read 7.29 s against 7.0 s when a
    # quiet machine reads 5.2-5.8 s.
    @{ Name = 'compile timing'; Tag = 'local'; Lock = $true; Window = $true; Build = $true; Run = { & $py -I tools/kernelgate.py --time } },
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
    @{ Name = 'analytic cpu'; Group = 'g1'; Tag = 'cpu'; Lock = $true; Echo = '^(PASS|FAIL|crossings) analytic'; Keep = $true; Run = {
        $n = $shardsAnalytic
        $o = @(& $runShards $py @(1..$n | ForEach-Object { "-I tools/analytic.py --gate --device cpu --shard $_/$n" }) (Join-Path $root 'out/check/analytic-cpu'))
        $code = $LASTEXITCODE
        $o
        # Each shard prints "members analytic cpu shard i/n: k of T"; the k must add up to T.
        $k = 0; $tot = -1
        foreach ($l in $o) { if ($l -match '^members analytic cpu shard \d+/\d+: (\d+) of (\d+)$') { $k += [int]$Matches[1]; $tot = [int]$Matches[2] } }
        if ($k -ne $tot -or $tot -lt 0) { "FAIL analytic cpu shards: members $k of $tot"; $code = 1 }
        else { "shards: analytic cpu $n shards, $k of $tot members" }
        $global:LASTEXITCODE = $code } },
    @{ Name = 'analytic cuda'; Group = 'g1'; Tag = 'local'; Lock = $true; Echo = '^(PASS|FAIL|crossings) analytic'; Keep = $true; Run = {
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
    @{ Name = 'integrator cpu'; Group = 'g2'; Tag = 'cpu'; Lock = $true; Echo = '^(PASS|FAIL) integrator'; Keep = $true; Run = {
        & $py -I tools/integrator_checks.py --gate --device cpu } },
    @{ Name = 'integrator cuda'; Tag = 'local'; Lock = $true; Echo = '^(PASS|FAIL) integrator'; Keep = $true; Run = {
        & $py -I tools/integrator_checks.py --gate --device cuda }
       Timed = @{ Check = 'B-16 tripwire'; Case = 'tripwire'; Rerun = { & $py -I tools/integrator_checks.py --check tripwire --device cuda } } },
    @{ Name = 'cpu gate tests'; Group = 'g3'; Tag = 'cpu'; Lock = $true; Identity = $exe; Keep = $true; Run = {
        & $runCpuTier 'gate' $shardsCpuGate 'cpu gate tests' (Join-Path $root 'out/check/gate-steps/cpu-gate-tests.out') (Join-Path $root 'out/check/cpu-gate-tests') } },
    # Time limit 270 s: after 132's fix the tier took 12.6-24.1 s over 100 runs under the machine lock;
    # 10x the slowest run, rounded up to a multiple of 30 s.
    @{ Name = 'gpu gate tests'; Group = 'g3'; Tag = 'local'; Lock = $true; Identity = $gpuExe; Keep = $true;
       Run = { & $runTimed $gpuExe '--tier=gate --duration=true' 270 (Join-Path $root 'out/check/gpu-gate-tests.out') } },
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
         if ($ms -gt [double]$limit -and (Resolve-TierRow 'B-24' $ms ([double]$limit) 'ms per 100 nodes').Verdict -eq 'FAIL') {
           Fail "material compile: $ms ms per 100 nodes is above B-24's $limit ms"
         }
         Write-Host "B-24 compile: $ms ms per 100 nodes (limit $limit ms)" } },
    # 0009 d11, d17a (084): one Blender start installs the staged extension zip, runs the 083 material tests,
    # renders A1's .blend through F12 on the CPU device and checks it against A1's truth and the builder's
    # recording (exporter equivalence), then renders a textured, lamp-lit plane through F12 on CPU and on CUDA
    # (a luminance floor per device and the pair_image agreement). The step uses the GPU, so it stays
    # ungrouped: no GPU step may run beside it. The smoke builds the zip itself; Invoke-ArBlender's own output
    # goes to the log.
    @{ Name = 'blender smoke'; Tag = 'local'; Echo = '^(RESULT smoke|Ran |OK$|FAILED)'; Run = {
        Invoke-TierBlender 'tests/blender/smoke_a1.py' @('--with-materials') 400 } }
  )
  # Judge one finished step: its failure names itself, then Identity and Check/Echo as before.
  function Complete-GateStep($s, $r) {
    # Keep: the step's whole output, for `tools/gate_table.py --cases` (seconds per case and scene).
    if ($s.Keep) {
      $kd = Join-Path $root 'out/check/gate-steps'
      New-Item -ItemType Directory -Force $kd | Out-Null
      Set-Content -Path (Join-Path $kd (($s.Name -replace ' ', '-') + '.out')) -Value $r.Out
    }
    if ($r.Code -ne 0) {
      $fl = @($r.Out | Where-Object { $_ -match '^(FAIL|kernelgate:)' })
      if ($fl.Count -gt 0) { ($fl | Select-Object -First 6) | ForEach-Object { Write-Host $_ } } else { Show-Tail $r }
      $why = if ($fl.Count -gt 0) { "$($s.Name): $($fl[0])" } else { "$($s.Name) failed (out/check/$Tier.log)" }
      Fail ($why.Substring(0, [math]::Min($why.Length, 140)))
    }
    if ($s.Identity) { Test-Identity $r $s.Name }
    if ($s.Check) { & $s.Check $r } elseif ($s.Echo) { $r.Out | Where-Object { $_ -match $s.Echo } | ForEach-Object { Write-Host $_ } }
    Show-OffReference ([pscustomobject]@{ Out = @($r.Out | Where-Object { -not ($s.Echo -and $_ -match $s.Echo) -or $s.Check }) })
  }
  function Invoke-GateStep($s) {
    # The window before the tree lock: a build holds its share while it waits for the tree lock, so the other
    # order would deadlock. Under -Landing the gate owns the window already (nested: nothing to take).
    $win = $s.Window -and -not (Test-ArFastWinNested)
    if ($win) { $w = Enter-ArFastWindow; $script:waited += $w; Add-ArExcluded $w }
    try {
      if ($s.Lock) { $w = Enter-ArTreeLock (Join-Path $root 'out/build'); $script:waited += $w; Add-ArExcluded $w }
      try {
        if ($s.Timed) {
          # A timing check with a re-run (B-16): foreign load in the step's window and nothing but this check
          # failed re-runs that check alone once; the second result decides (0009 d13).
          $tm = Invoke-ArTimedCheck -Check $s.Timed.Check -Log $log -Run {
            param($n)
            if ($n -eq 1) { $x = Step $s.Name $s.Run; $script:GateFirstOut = $x.Out; $x } else { Step "$($s.Name) (rerun)" $s.Timed.Rerun }
          } -Over {
            param($x)
            $line = @($x.Out | Where-Object { $_ -match "^(PASS|FAIL) integrator $($s.Timed.Case) " }) | Select-Object -First 1
            @{ Over = (Test-ArOnlyTiming $x.Out $x.Code $s.Timed.Case); Text = "$line" }
          }
          $r = $tm.Result
          if ($tm.Rerun) {
            # Keep the first run's other checks in the echo and the kept output; the re-run's line replaces its own.
            $r = [pscustomobject]@{ Code = $r.Code; Out = @($script:GateFirstOut | Where-Object { $_ -notmatch "^(PASS|FAIL) integrator ($($s.Timed.Case) |\w+:)" }) + @($r.Out) }
          }
        } else { $r = Step $s.Name $s.Run }
      } finally { if ($s.Lock) { Exit-ArTreeLock } }
    } finally { if ($win) { Exit-ArFastWindow } }
    if ($s.Build) { Add-GateBuild $script:ArSteps[-1].seconds }
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
        $runShards = [scriptblock]::Create($v.shardText)
        $runCpuTier = [scriptblock]::Create($v.cpuTierText)
        $shardsCpuGate = $v.shardsCpuGate; $shardsAnalytic = $v.shardsAnalytic
        $t = [Diagnostics.Stopwatch]::StartNew()
        $global:LASTEXITCODE = 0
        $out = & ([scriptblock]::Create($body)) 2>&1 | ForEach-Object { "$_" }
        [pscustomobject]@{ Code = $LASTEXITCODE; Out = @($out); Seconds = $t.Elapsed.TotalSeconds } }
      $vars = @{ root = "$root"; py = $py; dargs = $dargs; exe = $exe; gpuExe = $gpuExe; isCi = $isCi; timedText = $timedText; shardText = $shardText; cpuTierText = $cpuTierText; shardsCpuGate = $shardsCpuGate; shardsAnalytic = $shardsAnalytic }
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
    # B-08 times this step: builds of other worktrees that run without the machine lock wait outside it
    # (0009 d8), and its wait for builds already running is not work. -Landing keeps the window to the end.
    $w = Enter-ArFastWindow; $waited += $w; Add-ArExcluded $w
    try { $r = Step 'fast' { & pwsh -NoProfile -File tools/check.ps1 fast } } finally { if (-not $Landing) { Exit-ArFastWindow } }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'fast failed (out/check/fast.log)' }
    Show-OffReference $r
    # The fast tier's own build is not gate time either (B-08 leaves it out of its clock the same way).
    $fj = Join-Path $root 'out/check/fast.json'
    if (-not (Test-Path $fj)) { Fail 'fast wrote no out/check/fast.json' }
    $fb = @((Get-Content $fj -Raw | ConvertFrom-Json).steps | Where-Object { $_.name -eq 'build' })
    if ($fb.Count -ne 1) { Fail "out/check/fast.json holds $($fb.Count) 'build' steps, expected 1" }
    Add-GateBuild ([double]$fb[0].seconds)
    $i = 0
    while ($i -lt $gateSteps.Count) {
      $g = $gateSteps[$i].Group
      $n = 1
      if ($g) { while ($i + $n -lt $gateSteps.Count -and $gateSteps[$i + $n].Group -eq $g) { $n++ } }
      Invoke-GateGroup @($gateSteps[$i..($i + $n - 1)])
      if ($gateSteps[$i].Name -eq 'build dev') {
        # The gate is a writer of the machine id cache (the CLI is built, the machine lock held): refresh it every run
        # (one line when it changed or the CLI refused), then the standing the gate's tests with a timing CHECK take.
        $ml = @(& $py -I tools/machine_id.py refresh --sha "$(& git rev-parse --short HEAD 2>$null)" 2>&1 | ForEach-Object { "$_" })
        $ml | ForEach-Object { Write-Host $_ }
        try { $null = Set-ArMachineStanding } catch { Fail "machine id: $($_.Exception.Message)" }
      }
      $i += $n
    }
  } elseif ($Tier -eq 'full') {
    $r = Step 'gate' { & pwsh -NoProfile -File tools/check.ps1 gate @dargs }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'gate failed (out/check/gate.log)' }
    try { $null = Set-ArMachineStanding } catch { Fail "machine id: $($_.Exception.Message)" }
    # The tools fixtures that left the gate's `tools tests` step (tests/tools/run_fast.ps1 $slow).
    $r = Step 'tools tests slow' { & pwsh -NoProfile -File tests/tools/run_fast.ps1 -Set slow }
    $r.Out | Where-Object { $_ -match '^(PASS|FAIL) ' } | ForEach-Object { Write-Host $_ }
    if ($r.Code -ne 0) { Show-Tail $r 8; Fail 'tools tests slow failed (out/check/full.log)' }
    # Land fixture cases a-j (canary refusals, hunk matrix, undo, busy refusals): minutes, so full only.
    $r = Step 'tools fixtures' { & pwsh -NoProfile -File tests/tools/land_fixture/run_cases.ps1 }
    if ($r.Code -ne 0) { Show-Tail $r 8; Fail 'tools fixtures failed (out/check/full.log)' }
    # The Blender suites (lib/blender.ps1 Get-ArFullBlenderSuites, the list `land prepare` shares): the API
    # surface inside Blender, where the bundled openvdb exists (0007 d46; ci checks the rest on the wheel), image
    # sync and node values (about 40 s does not fit B-09), and every windowed gui_* script, which drives itself by
    # timers and ends the process. Not $t: Step's own stopwatch of that name is visible inside the block.
    foreach ($su in (Get-ArFullBlenderSuites)) {
      $r = Step "blender $($su.Name)" { Invoke-TierBlender $su.Script @() $su.TimeoutS -Gui:([bool]$su.Gui) }
      $r.Out | Where-Object { $_ -match $su.Echo } | ForEach-Object { Write-Host $_ }
      if ($r.Code -ne 0) { Fail "blender $($su.Name) failed (out/check/full.log)" }
    }
    # 0004 D10 tree row: the light tree against the alias-table oracle per 8x8 tile (Welch, Holm, 0009 d17(v)) and the
    # 2x variance ratio, 8 seeds a side on each device (tools/welch_tiles.py). The gate's `scenes` step built the recording.
    $r = Step 'tree~alias tiles' {
      & $py -I tools/welch_tiles.py --scene d10_many_lights --device cpu
      if ($LASTEXITCODE -eq 0) { & $py -I tools/welch_tiles.py --scene d10_many_lights --device cuda }
    }
    $r.Out | Where-Object { $_ -match '^(PASS|FAIL) welch_tiles|^variance ratio' } | ForEach-Object { Write-Host $_ }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'tree~alias tiles failed (out/check/full.log)' }
    # Analytic renders moved out of the gate (owner decision 38; tools/analytic.py MOVED_TO_FULL): the
    # gate keeps them on the other device. Recordings are up to date: the gate's `scenes` step built them.
    foreach ($dev in 'cpu', 'cuda') {
      $r = Step "analytic $dev moved to full" { & $py -I tools/analytic.py --moved --device $dev }
      $r.Out | Where-Object { $_ -match '^(PASS|FAIL) analytic' } | ForEach-Object { Write-Host $_ }
      if ($r.Code -ne 0) { Show-Tail $r 5; Fail "analytic $dev moved to full failed (out/check/full.log)" }
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
    # 0008 V11 final row: denoised relMSE below raw at 64 spp on three scenes, CUDA, against cached 16k-spp
    # references (rendered once into the machine-wide cache; tools/denoise_check.py). The scenes need the
    # assets of assets/manifest.toml, fetched by hand (a check never downloads); b_tx is reported, not asserted.
    $r = Step 'denoise check' {
      $missing = @('cornell_box/cornell-box.zip', 'studio_small_09/studio_small_09_1k.exr') | Where-Object { -not (Test-Path (Join-Path $root "assets/_cache/$_")) }
      if ($missing) {
        "FAIL denoise_check: missing assets/_cache/$($missing -join ', assets/_cache/'): run pwsh tools/fetch.ps1 assets -Name cornell_box; pwsh tools/fetch.ps1 assets -Name studio_small_09"
        $global:LASTEXITCODE = 1
      } else {
        & $py -I tools/scenes.py cornell_area cornell_point shader_balls s3_textured
        if ($LASTEXITCODE -eq 0) { & $py -I tools/denoise_check.py --device cuda }
      }
    }
    $r.Out | Where-Object { $_ -match '^(PASS|FAIL) denoise_check|^cold start' } | ForEach-Object { Write-Host $_ }
    if ($r.Code -ne 0) { Show-Tail $r 8; Fail 'denoise check failed (out/check/full.log)' }
    # The CPU suite's full tier: cases tagged full there ran nowhere before this step; the gate's `cpu gate tests` runs --tier=gate only.
    # Time limit 150 s: the tier took 14.7 s (one run, task 234); 10x that, rounded up to a multiple of 30 s, as for the GPU tiers.
    $r = Step 'cpu full tests' { & $runTimed $exe '--tier=full --duration=true' 150 (Join-Path $root 'out/check/cpu-full-tests.out') }
    if ($r.Code -ne 0) { Show-Tail $r 8; Fail 'cpu full tests failed (out/check/full.log)' }
    # B-23 (0009 d13): the light-structure build at 10k lights against the row's limit; tools/b23_check.ps1 is the one place that compares.
    $r = Step 'light build B-23' { & pwsh -NoProfile -File tools/b23_check.ps1 -Exe $exe }
    $r.Out | Where-Object { $_ -match '^(B-23 build:|PASS B-23|FAIL B-23)' } | ForEach-Object { Write-Host $_ }
    if ($r.Code -ne 0) { Show-Tail $r 8; Fail 'light build B-23 failed (out/check/full.log)' }
    $r = Step 'probe registers'{ & $py -I tools/kernelgate.py --probe }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'probe registers failed' }
    # Warnings only (0002 D10, ruling 17): shown, never a failure.
    $r.Out | Where-Object { $_ -like 'WARNING*' } | ForEach-Object { Write-Host $_ }
  } else {
    # The tile Welch tool's self-test (tools/welch_tiles.py, 0009 d17(v)) starts beside lint and is collected after
    # it: its own work is 0.06 s, but importing numpy costs 0.4 s here, which run in series would grow B-08's clock.
    $wtOut = Join-Path $root 'out/check/welch-self-test.out'
    $wt = Start-Process -FilePath $py -ArgumentList '-I', (Join-Path $root 'tools/welch_tiles.py'), '--self-test' -WorkingDirectory $root `
      -NoNewWindow -PassThru -RedirectStandardOutput $wtOut -RedirectStandardError "$wtOut.err"
    $null = $wt.Handle
    $r = Step 'lint' { & $py -I tools/lint.py }
    $wr = Step 'welch_tiles self-test' {
      if (-not $wt.WaitForExit(60000)) { Stop-Process -Id $wt.Id -Force; 'FAIL welch_tiles self-test: no exit in 60 s'; $global:LASTEXITCODE = 1; return }
      Get-Content $wtOut; Get-Content "$wtOut.err"
      $global:LASTEXITCODE = $wt.ExitCode
    }
    if ($wr.Code -ne 0) { Show-Tail $wr 5; Fail 'welch_tiles self-test failed (out/check/fast.log)' }
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
    # The standing the tests with a timing CHECK of their own take (ASTRORAY_OFF_REFERENCE), from the machine id cache
    # a full dev build wrote; ci has no machine id and clears a stray value. A file read: nothing here is excluded from B-08.
    try { $null = Set-ArMachineStanding -NoMachine:$isCi } catch { Fail "machine id: $($_.Exception.Message)" }
    # Fast cases that share an exact name (plain TEST_CASEs in the ar-f64 files; AR_RT cases carry the ' f64' suffix
    # here as in the gate) sit hundreds of cases apart in file order, which leaves no allowed cut; by name they are adjacent.
    $prevFast = Join-Path $root 'out/check/fast-tests.out'
    # B-08 and B-12 are wall-clock: a first result over either with foreign load in its window runs the step once
    # more and the second decides (0009 d13); the first attempt's seconds leave the tier clock.
    $tm = Invoke-ArFastTests -PerTest $perTest -Budget $budget -Log $log -NoMachine:$isCi -WaiveB08:$Profiling -Step { param($n, $b) Step $n $b } -Tests {
      & $runCpuTier 'fast' $shardsFast 'fast tests' $prevFast (Join-Path $root 'out/check/fast-tests') '--order-by=name'
    }
    $r = $tm.Result
    # The next run's cut balances by these seconds, as the gate does with its kept step outputs.
    if ($r.Code -eq 0) { Set-Content -Path $prevFast -Value $r.Out }
    Exit-ArTreeLock
    if ($r.Code -ne 0) {
      Show-Tail $r
      Fail "tests failed (out/check/$Tier.log)"
    }
    Test-Identity $r 'binary'
    $b12 = Get-ArSlowTests $r.Out $perTest
    if ($b12.Slow.Count -gt 0) {
      $rows = @($b12.Slow | ForEach-Object { [pscustomobject]@{ Case = $_.Case; Seconds = $_.Seconds
            V = (Resolve-ArTimingRow -Row "B-12 $($_.Case)" -Measured $_.Seconds -Limit $perTest -NoMachine:$isCi) } })
      $rows | Select-Object -First 5 | ForEach-Object {
        if ($_.V.Verdict -eq 'FAIL') {
          Write-Host "B-12: $($_.Case) took $($_.Seconds) s (limit $perTest s)"
          if ($_.V.Line) { Write-ArOffReference $_.V.Line }
        } elseif ($_.V.Line) { Write-ArOffReference $_.V.Line }
      }
      if (@($rows | Where-Object { $_.V.Verdict -eq 'FAIL' }).Count -gt 0) { Fail "B-12: test over $perTest s" }
    }
    # Hosted ci shows only the summary, so the margin of the slowest case goes into it on every pass.
    Write-Host "B-12: slowest $($b12.Slowest.Case) $($b12.Slowest.Seconds) s of $perTest s"
    # 0008 V11's relMSE function, its exact cases and its box-filter case: stdlib only (the tool imports numpy
    # only for a capture), so hosted ci runs it as well.
    $r = Step 'denoise self-test' { & $py -I tools/denoise_check.py --self-test }
    if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'denoise self-test failed (out/check/fast.log)' }

    if ($isCi) {
      # The lint self-test proves every lint section still catches its seeded cases;
      # fast runs only the repository lint, so a section can rot without it.
      $r = Step 'lint self-test' { & $py -I tools/lint.py --self-test }
      if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'lint self-test failed (out/check/ci.log)' }
      $r = Step 'build cpu' { & pwsh -NoProfile -File tools/build.ps1 -Preset cpu -NoMachineLock }
      if ($r.Code -ne 0) { Show-Tail $r 5; Fail 'cpu build failed (out/check/build.log)' }
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
$waive = $false
# What the over-limit text says: the limit in force when a profile derived it, with the reference limit beside it.
function Get-OverText($v, [double]$Limit) {
  if ($v.Limit -ne $Limit) { return "over its derived limit $([math]::Round($v.Limit, 2)) s (reference limit $Limit s)" }
  return "over $Limit s"
}
if ($Tier -eq 'gate') {
  $kp = ($script:ArSteps | Where-Object { $_.name -in $kernelSteps } | Measure-Object -Property seconds -Sum).Sum
  $ct = ($script:ArSteps | Where-Object { $_.name -eq 'compile timing' } | Measure-Object -Property seconds -Sum).Sum
  $why = "kernel part {0:N1} s of {1} s, build steps {2:N1} s not counted (compile timing {3:N1} s)" -f $kp, $kernelBudget, $script:buildSeconds, $ct
  if ($kp -gt $kernelBudget) {
    $v = Resolve-TierRow 'B-09 kernel part' $kp $kernelBudget
    if ($v.Verdict -eq 'FAIL') { Fail "B-09: kernel part $([math]::Round($kp, 1)) s $(Get-OverText $v $kernelBudget)" }
    $waive = $true; $why += "; $($v.Line)"
  }
  if ((Get-ArSeconds) -gt $budget) {
    $v = Resolve-TierRow 'B-09' (Get-ArSeconds) $budget
    if ($v.Verdict -eq 'FAIL') { Fail "B-09: tier $(Get-OverText $v $budget) ($why)" }
    # Off-reference (or a profile that allows it): the budget stays as printed, the line says why it passes.
    $waive = $true; $why += "; $($v.Line)"
  }
} elseif ($Tier -eq 'fast') {
  $v = Resolve-ArFastB08 -Seconds (Get-ArSeconds) -Budget $budget -Profiling:$Profiling -StartedByProfile:($env:ASTRORAY_PROFILE_RUN -eq '1') -NoMachine:$isCi
  if ($v.Verdict -eq 'FAIL') {
    $why = "B-08: tier $(Get-OverText $v $budget)"
    if ($v.Line) { Write-ArOffReference $v.Line }
  } elseif ($v.Line) {
    # Off-reference, a profile that allows it, or a profile measuring run: the budget stays as printed, the line says why.
    if ($v.Verdict -ne 'WAIVED') { Write-ArOffReference $v.Line }
    $waive = $true; $why = $v.Line
  }
}
$ok = Complete-ArSummary -Pass $true -Waited $waited -Budget $budget -Reason $why -OverWaived:$waive
if ($isCi) {
  $jp = Join-Path $root 'out/check/ci.json'
  $j = Get-Content $jp -Raw | ConvertFrom-Json
  $j | Add-Member -NotePropertyName cl -NotePropertyValue "$script:clVer"
  $j | ConvertTo-Json -Depth 4 | Set-Content -Path $jp
}
Exit-ArFastWindow
Exit-ArMachineLock
exit ($(if ($ok) { 0 } else { 1 }))
