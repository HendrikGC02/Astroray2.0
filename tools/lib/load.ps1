# SPDX-License-Identifier: Apache-2.0
# Foreign-load sampling and the single re-run of a timing check (0009 d13, owner decision 42). Dot-source this file.
# Load is any compiler, linker, build, Blender or GPU compute process outside this run's own process tree
# (its ancestors and its descendants; a sibling under a shared parent is foreign): other worktrees build without the machine lock, and a wall-clock
# budget read beside them measures them. Ref: tools/perf.ps1 (the GPU-user list); 0009 d18 (the tripwire's
# 1.25x is far outside the measured noise, so a red under load is the load).
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
  $null = $Watch.Ps.EndInvoke($Watch.Handle)
  $Watch.Ps.Dispose()
  foreach ($l in Get-ArForeignLoad $Watch.Names) { $Watch.State.Seen[$l] = $true }
  return @($Watch.State.Seen.Keys | Sort-Object)
}

# Runs `& $Run 1`, judges it with `& $Over result` (an object with Over [bool] and Text, both for a pass and a
# fail; a result that failed for another reason is Over = $false). A first result that is over and had foreign
# load in its window runs `& $Run 2` once; the second result decides. Without load the first decides. Prints
# `RERUN <check>: load <processes>` and both results when it re-runs. Returns Result, Over (the deciding one),
# Rerun and Load.
function Invoke-ArTimedCheck {
  param([Parameter(Mandatory)][string]$Check, [Parameter(Mandatory)][scriptblock]$Run,
    [Parameter(Mandatory)][scriptblock]$Over, [string[]]$Names = $script:ArLoadNames, [int]$IntervalMs = 2000)
  $w = Start-ArLoadWatch $Names $IntervalMs
  $first = & $Run 1
  $load = @(Stop-ArLoadWatch $w)
  $o1 = & $Over $first
  if (-not $o1.Over -or $load.Count -eq 0) {
    return [pscustomobject]@{ Result = $first; Over = [bool]$o1.Over; Rerun = $false; Load = $load }
  }
  Write-Host "RERUN ${Check}: load $($load -join ', ')"
  Write-Host "  first:  $($o1.Text)"
  $w = Start-ArLoadWatch $Names $IntervalMs
  $second = & $Run 2
  $null = Stop-ArLoadWatch $w
  $o2 = & $Over $second
  Write-Host "  second: $($o2.Text) (decides)"
  return [pscustomobject]@{ Result = $second; Over = [bool]$o2.Over; Rerun = $true; Load = $load }
}
