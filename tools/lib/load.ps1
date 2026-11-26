# SPDX-License-Identifier: Apache-2.0
# Foreign-load sampling and the single re-run of a timing check (0009 d13, owner decision 42). Dot-source this file.
# Load is any compiler, linker, build, Blender or GPU compute process outside this run's own process tree
# (its ancestors and its descendants; a sibling under a shared parent is foreign): other worktrees build without
# the machine lock, and a wall-clock budget read beside them measures them. Ref: tools/perf.ps1 (the GPU-user
# list); 0009 d18 (the tripwire's 1.25x is far outside the measured noise, so a red under load is the load).
$script:ArLoadNames = @('cl', 'link', 'nvcc', 'ninja', 'blender', 'astroray_cli', 'astroray_tests_gpu', 'nsys')

# "name pid" of every foreign process of $Names now. Each call reads the process table once.
function Get-ArForeignLoad([string[]]$Names) {
  $cand = @(Get-Process -Name $Names -ErrorAction SilentlyContinue)
  if ($cand.Count -eq 0) { return @() }
  $parent = @{}
  foreach ($p in Get-CimInstance Win32_Process) { $parent[[int]$p.ProcessId] = [int]$p.ParentProcessId }
  $ancestors = @{}
  for ($i = $PID; $i -and -not $ancestors.ContainsKey($i); $i = $parent[$i]) { $ancestors[$i] = $true }
  $foreign = @()
  foreach ($c in $cand) {
    $own = $ancestors.ContainsKey([int]$c.Id)
    $seen = @{}
    # A parent that already exited ends the chain (its ppid is not re-parented): that process is foreign.
    for ($i = [int]$c.Id; $i -and -not $seen.ContainsKey($i); $i = $parent[$i]) {
      $seen[$i] = $true
      if ($i -eq $PID) { $own = $true; break }
    }
    if (-not $own) { $foreign += "$($c.ProcessName) $($c.Id)" }
  }
  return $foreign
}

# Samples at the start, every $IntervalMs on a background runspace, and (Stop-ArLoadWatch) at the end.
function Start-ArLoadWatch([string[]]$Names = $script:ArLoadNames, [int]$IntervalMs = 2000) {
  $st = [hashtable]::Synchronized(@{ Stop = $false; Seen = @{} })
  foreach ($l in Get-ArForeignLoad $Names) { $st.Seen[$l] = $true }
  $ps = [powershell]::Create()
  $null = $ps.AddScript({
      param($st, $fnText, $names, $ms)
      $f = [scriptblock]::Create($fnText)
      while ($true) {
        for ($t = 0; $t -lt $ms -and -not $st.Stop; $t += 50) { Start-Sleep -Milliseconds 50 }
        if ($st.Stop) { break }
        foreach ($l in (& $f $names)) { $st.Seen[$l] = $true }
      }
    }).AddArgument($st).AddArgument(${function:Get-ArForeignLoad}.ToString()).AddArgument($Names).AddArgument($IntervalMs)
  [pscustomobject]@{ State = $st; Ps = $ps; Handle = $ps.BeginInvoke(); Names = $Names }
}

function Stop-ArLoadWatch($Watch) {
  $Watch.State.Stop = $true
  # Dispose even when EndInvoke rethrows the sampler's error: a leaked runspace keeps sampling.
  try { $null = $Watch.Ps.EndInvoke($Watch.Handle) } finally { $Watch.Ps.Dispose() }
  foreach ($l in Get-ArForeignLoad $Watch.Names) { $Watch.State.Seen[$l] = $true }
  return @($Watch.State.Seen.Keys | Sort-Object)
}

# Runs `& $Run 1`, judges it with `& $Over result` (an object with Over [bool] and Text, both for a pass and a
# fail; a result that failed for another reason is Over = $false). A first result that is over and had foreign
# load in its window runs `& $Run 2` once; the second result decides. Without load the first decides. Prints
# `RERUN <check>: load <processes>` and both results when it re-runs, and one `load <check>: <processes>` line
# whenever load was seen and no re-run follows; both also go to $Log. Returns Result, Over (the deciding one),
# Rerun, Load (first window) and DecidingLoad (the window of the deciding run: a baseline must not be recorded
# beside it).
function Invoke-ArTimedCheck {
  param([Parameter(Mandatory)][string]$Check, [Parameter(Mandatory)][scriptblock]$Run,
    [Parameter(Mandatory)][scriptblock]$Over, [string[]]$Names = $script:ArLoadNames, [int]$IntervalMs = 2000,
    [string]$Log = '')
  function Say([string]$t) { Write-Host $t; if ($Log) { Add-Content -Path $Log -Value $t } }
  $w = Start-ArLoadWatch $Names $IntervalMs
  try { $first = & $Run 1 } finally { $load = @(Stop-ArLoadWatch $w) }
  $o1 = & $Over $first
  if (-not $o1.Over -or $load.Count -eq 0) {
    if ($load.Count) { Say "load ${Check}: $($load -join ', ')" }
    return [pscustomobject]@{ Result = $first; Over = [bool]$o1.Over; Rerun = $false; Load = $load; DecidingLoad = $load }
  }
  Say "RERUN ${Check}: load $($load -join ', ')"
  Say "  first:  $($o1.Text)"
  $w = Start-ArLoadWatch $Names $IntervalMs
  try { $second = & $Run 2 } finally { $load2 = @(Stop-ArLoadWatch $w) }
  $o2 = & $Over $second
  Say "  second: $($o2.Text) (decides)"
  if ($load2.Count) { Say "load ${Check} (re-run): $($load2 -join ', ')" }
  return [pscustomobject]@{ Result = $second; Over = [bool]$o2.Over; Rerun = $true; Load = $load; DecidingLoad = $load2 }
}

# B-16: did the gate step fail on the tripwire's timing alone? Only a failing line of check $Case that carries the
# limit ratio counts; "no baseline", a caught error, another failing check or a bare summary line stay red
# without a re-run. $Out is the step's output lines.
function Test-ArOnlyTiming([string[]]$Out, [int]$Code, [string]$Case) {
  $bad = @($Out | Where-Object { $_ -match '^FAIL integrator (\w+) \w+:' } | ForEach-Object { $Matches[1] })
  $line = @($Out | Where-Object { $_ -match "^FAIL integrator $Case \w+:" }) | Select-Object -First 1
  $timing = $line -and $line -match 'limit x\d' -and $line -notmatch 'no baseline'
  return [bool]($Code -ne 0 -and $bad.Count -gt 0 -and @($bad | Where-Object { $_ -ne $Case }).Count -eq 0 -and $timing)
}

# B-08 and B-12 of the fast tier's tests step. $Step is `{ param($name, $body) ... }` (check.ps1's Step: runs the
# body, records the step, returns Code and Out); $Tests is the body. The first attempt's seconds leave the tier
# clock, so the second result is judged on the tier's own time (summary.ps1 and testrun.ps1 are dot-sourced).
function Invoke-ArFastTests {
  param([Parameter(Mandatory)][scriptblock]$Step, [Parameter(Mandatory)][scriptblock]$Tests, [double]$PerTest,
    $Budget, [string[]]$Names = $script:ArLoadNames, [int]$IntervalMs = 2000, [string]$Log = '')
  Invoke-ArTimedCheck -Check 'B-08/B-12 fast tests' -Names $Names -IntervalMs $IntervalMs -Log $Log -Run {
    param($n)
    if ($n -eq 2) { Add-ArExcluded ([double]$script:ArSteps[-1].seconds) }
    & $Step $(if ($n -eq 1) { 'tests' } else { 'tests (rerun)' }) $Tests
  } -Over {
    param($x)
    if ($x.Code -ne 0) { return @{ Over = $false; Text = 'tests failed' } }
    $b = Get-ArSlowTests $x.Out $PerTest
    $secs = Get-ArSeconds
    $why = @()
    if ($b.Slow.Count -gt 0) { $why += "B-12 $($b.Slow[0].Case) $($b.Slow[0].Seconds) s over $PerTest s" }
    if ($null -ne $Budget -and $secs -gt $Budget) { $why += "B-08 tier $([math]::Round($secs, 1)) s over $Budget s" }
    @{ Over = ($why.Count -gt 0); Text = $(if ($why) { $why -join '; ' } else { "slowest $($b.Slowest.Seconds) s, tier $([math]::Round($secs, 1)) s" }) }
  }
}

# B-38: one bench run is `& $Bench` (history lines); `& $Compare lines` returns Code (1 = over B-38) and Out. The
# re-run starts with `& $Quiet` (perf.ps1's Assert-QuietGpu: the rule before every bench run) and replaces the lines.
function Invoke-ArPerfCheck {
  param([Parameter(Mandatory)][scriptblock]$Bench, [Parameter(Mandatory)][scriptblock]$Compare,
    [Parameter(Mandatory)][scriptblock]$Quiet, [string[]]$Names = $script:ArLoadNames, [int]$IntervalMs = 2000)
  Invoke-ArTimedCheck -Check 'B-38 perf' -Names $Names -IntervalMs $IntervalMs -Run {
    param($n)
    if ($n -eq 2) { & $Quiet }
    & $Bench
  } -Over {
    param($lines)
    $c = & $Compare $lines
    @{ Over = ($c.Code -eq 1); Text = (@($c.Out) -join ' | ') }
  }
}
