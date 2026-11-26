# SPDX-License-Identifier: Apache-2.0
# Canary verification shared by tools/canary.ps1 (implementers) and tools/land.ps1 (prepare). Dot-source this file.
# Ref: docs/decisions/0009-build-test-process.md d26 step 3, d28 (task and canary format).
# Get-CanaryItems below is the real parse of the `Canary:` line; tools/lint.py taskfile() only checks its shape
# and must stay in step with it.
. (Join-Path $PSScriptRoot 'testrun.ps1')

$script:CanaryLogPath = $null
$script:CanaryScenesSeconds = 0.0   # time spent in tools/scenes.py after builds; land reports it as its own phase
function Write-CanaryLog($text) { if ($script:CanaryLogPath) { Add-Content -Path $script:CanaryLogPath -Value $text } }

# Items of a Canary: line. Returns objects {Kind exe|py|cmd; Target; Name}.
function Get-CanaryItems([string]$line) {
  $m = [regex]::Match($line, 'must turn red:(.*)$')
  if (-not $m.Success) { return @() }
  $items = @()
  $cur = $null
  # Alternatives in order: backticked command, quoted name, ignorable parenthesised text, bare word.
  $re = '`([^`]*)`|"([^"]*)"|\((?:[^()]|\([^()]*\))*\)|([^\s,;]+)'
  $flush = {
    if ($cur -and -not $cur.Used) { $script:__u = [pscustomobject]@{ Kind = $cur.Kind; Target = $cur.Target; Name = '' } } else { $script:__u = $null }
  }
  foreach ($t in [regex]::Matches($m.Groups[1].Value, $re)) {
    if ($t.Groups[1].Success) {
      & $flush; if ($script:__u) { $items += $script:__u }; $cur = $null
      $items += [pscustomobject]@{ Kind = 'cmd'; Target = $t.Groups[1].Value.Trim(); Name = '' }
    } elseif ($t.Groups[2].Success) {
      if (-not $cur) { throw "canary name '$($t.Groups[2].Value)' follows no test executable or file" }
      $items += [pscustomobject]@{ Kind = $cur.Kind; Target = $cur.Target; Name = $t.Groups[2].Value }
      $cur.Used = $true
    } elseif ($t.Groups[3].Success) {
      $w = $t.Groups[3].Value
      $kind = if ($w -match '^astroray_\w+$') { 'exe' } elseif ($w -match '^[\w./\\-]+\.py$') { 'py' } else { $null }
      if ($kind) {
        & $flush; if ($script:__u) { $items += $script:__u }
        $cur = @{ Kind = $kind; Target = $w; Used = $false }
      }
    }
  }
  & $flush; if ($script:__u) { $items += $script:__u }
  return $items
}

# One patch per hunk: the file header plus one @@ hunk each. Returns {Where 'file:line'; Text}.
function Split-CanaryPatch([string]$text) {
  $lines = @($text -split "`n")
  if ($lines.Count -and $lines[-1] -eq '') { $lines = $lines[0..($lines.Count - 2)] }
  $hunks = [Collections.Generic.List[object]]::new()
  $hdr = @(); $file = ''; $cur = $null
  $push = { if ($cur) { $hunks.Add([pscustomobject]@{ Where = "$($cur.File):$($cur.Line)"; Text = ((@($hdr) + $cur.Lines) -join "`n") + "`n" }) } }
  foreach ($ln in $lines) {
    if ($ln -like 'diff --git *') {
      & $push; $cur = $null; $hdr = @($ln); $file = ''
      if ($ln -match ' b/(.+)$') { $file = $Matches[1] }
    } elseif ($ln -like '@@ *') {
      & $push
      $cur = @{ File = $file; Line = [regex]::Match($ln, '^@@ -(\d+)').Groups[1].Value; Lines = @($ln) }
    } elseif ($cur) { $cur.Lines += $ln }
    else { $hdr += $ln }
  }
  & $push
  return @($hunks)
}

# Problems that make a patch unusable, each naming the fix. The cheap text checks come first.
function Get-CanaryPatchProblems([string]$Patch, [string]$Root, [string]$Number) {
  $bytes = [IO.File]::ReadAllBytes($Patch)
  $p = @()
  if ([Text.Encoding]::UTF8.GetString($bytes) -match '(?m)^diff --git a/tasks/') {
    $p += "tasks/$Number.canary.patch changes a file under tasks/ (it contains itself): break the code only, then git diff --output=tasks/$Number.canary.patch -- src cli include blender tools scenes"
  }
  if ([Array]::IndexOf($bytes, [byte]13) -ge 0) {
    $p += "tasks/$Number.canary.patch holds CR bytes: write it with git diff --output=tasks/$Number.canary.patch, never with >"
  }
  if ($p.Count) { return $p }
  $o = & git -C $Root apply --check $Patch 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0) { return @("tasks/$Number.canary.patch does not apply (stale after a rebase?): regenerate it from a broken tree. $($o -join ' ')") }
  return @()
}

function New-CanaryResult($label, $ran, $failed, $err = '') { [pscustomobject]@{ Label = $label; Ran = $ran; Failed = $failed; Error = $err } }

function ConvertTo-CanaryResult($r) { New-CanaryResult $r.Case $r.Ran $r.Failed $r.Error }

# Results of one file group from its output; a non-empty $err marks every case of the group errored.
function Get-CanaryGroupResults($group, [string]$txt, [string]$err) {
  return @(Get-ArCaseResults @($group | ForEach-Object { $_.Name }) $txt $err | ForEach-Object { ConvertTo-CanaryResult $_ })
}

# Whether a passed baseline recorded earlier still holds: only a clean tree at the same tree hash and the
# same items. A dirty tree's HEAD hash says nothing about the files that ran.
function Test-CanaryBaselineRecord($prior, [string]$tree, [string]$key, [bool]$clean) {
  return [bool]($clean -and $prior -and $prior.tree -eq $tree -and $prior.key -eq $key)
}

# Items of one tests/abi or tests/blender file run through lib/testrun.ps1 (one start for a non-gui file);
# each case is read from its line.
function Invoke-CanaryFileGroup($group, [string]$rel, [string]$Root) {
  $kind = if ($rel -like 'tests/abi/*') { 'py' } else { 'blender' }
  $res = @(Invoke-ArTestCase -Kind $kind -Target $rel -Case @($group | ForEach-Object { $_.Name }) -Root $Root)
  # A shared start logs its output once.
  foreach ($o in @($res | ForEach-Object { $_.Output } | Select-Object -Unique)) { Write-CanaryLog $o }
  return @($res | ForEach-Object { ConvertTo-CanaryResult $_ })
}

function Invoke-CanaryItem($it, [string]$Root) {
  # Returns {Label; Ran; Failed; Error}. Ran: the named test exists and ran. Failed: a real test failure.
  # Error: the item could not be judged (missing script, crash, timeout); it is never counted as red.
  if ($it.Kind -eq 'cmd') {
    $tok = @($it.Target -split '\s+')
    foreach ($t in $tok) {
      if ($t -match '\.(py|ps1)$' -and -not (Test-Path (Join-Path $Root $t))) { return New-CanaryResult $it.Target $false $false "script '$t' does not exist" }
    }
    if ($tok[0] -notmatch '\.py$' -and -not (Get-Command $tok[0] -ErrorAction SilentlyContinue)) { return New-CanaryResult $it.Target $false $false "command '$($tok[0])' not found" }
    if ($tok[0] -match '\.py$') { & (Get-ArPython) -I @tok 2>&1 | ForEach-Object { Write-CanaryLog "$_" } }
    else { & pwsh -NoProfile -Command $it.Target 2>&1 | ForEach-Object { Write-CanaryLog "$_" } }
    return New-CanaryResult $it.Target $true ($LASTEXITCODE -ne 0)
  }
  if ($it.Kind -eq 'exe') {
    $r = @(Invoke-ArTestCase -Kind exe -Target $it.Target -Case $it.Name -Root $Root)[0]
    Write-CanaryLog $r.Output
    return ConvertTo-CanaryResult $r
  }
  $rel = $it.Target -replace '\\', '/'
  if ($rel -like 'tests/abi/*' -or $rel -like 'tests/blender/*') { return (Invoke-CanaryFileGroup @($it) $rel $Root)[0] }
  return New-CanaryResult $it.Name $false $false "no runner for canary test file '$rel' (tests/abi and tests/blender only)"
}

# Runs every item; items of one abi or non-gui Blender file share one start. Results are in item order.
function Invoke-CanaryItems($items, [string]$Root) {
  $res = [object[]]::new(@($items).Count)
  $done = @{}
  for ($i = 0; $i -lt $res.Count; $i++) {
    if ($done[$i]) { continue }
    $it = $items[$i]
    $rel = $it.Target -replace '\\', '/'
    $grouped = $it.Kind -eq 'py' -and ($rel -like 'tests/abi/*' -or ($rel -like 'tests/blender/*' -and (Split-Path $rel -Leaf) -notlike 'gui_*'))
    $idx = if ($grouped) { @(0..($res.Count - 1) | Where-Object { $items[$_].Kind -eq 'py' -and ($items[$_].Target -replace '\\', '/') -eq $rel }) } else { @($i) }
    if ($grouped) { $r = @(Invoke-CanaryFileGroup @($idx | ForEach-Object { $items[$_] }) $rel $Root) } else { $r = @(Invoke-CanaryItem $it $Root) }
    for ($k = 0; $k -lt $idx.Count; $k++) { $res[$idx[$k]] = $r[$k]; $done[$idx[$k]] = $true }
  }
  return $res
}

function Invoke-CanaryBuild($targets, [bool]$cpuOnly, [string]$Root, [bool]$Patched = $false) {
  # Returns '' or, for a patched tree only, why tools/scenes.py failed. A build failure always throws.
  foreach ($t in $targets) {
    $b = @('-NoProfile', '-File', (Join-Path $Root 'tools/build.ps1'), '-Target', $t)
    if ($cpuOnly) { $b += '-NoMachineLock' }
    $o = & pwsh @b 2>&1 | ForEach-Object { "$_" }
    Write-CanaryLog $o
    if ($LASTEXITCODE -ne 0) { throw "canary build of '$t' failed: $(($o | Select-Object -Last 2) -join ' ')" }
  }
  # A build can change what the recorder writes, and items that render read out/scenes: regenerate after every
  # build (a warm machine-wide cache makes it cost well under a second). The fixture has no tools/scenes.py.
  # On the baseline a failure is fatal; under a patch it is what the breakage did (a canary may break the recorder).
  if (Test-Path (Join-Path $Root 'tools/scenes.py')) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $o = & (Get-ArPython) -I (Join-Path $Root 'tools/scenes.py') 2>&1 | ForEach-Object { "$_" }
    $script:CanaryScenesSeconds += $sw.Elapsed.TotalSeconds
    Write-CanaryLog $o
    if ($LASTEXITCODE -ne 0) {
      $why = "tools/scenes.py failed after the canary build: $(($o | Select-Object -Last 2) -join ' ')"
      if (-not $Patched) { throw $why }
      return $why
    }
  }
  return ''
}

# The results of a patched build: its items run, unless tools/scenes.py failed after the build; then every item of
# that build is recorded as run and red (the patch broke the recordings the items read), and the log says so.
function Invoke-CanaryPatchedItems($items, [string]$scenesErr, [string]$Root) {
  if (-not $scenesErr) { return @(Invoke-CanaryItems $items $Root) }
  Write-CanaryLog "$scenesErr; every item of this build counts red"
  return @($items | ForEach-Object { New-CanaryResult $(if ($_.Name) { $_.Name } else { $_.Target }) $true $true })
}

function Format-CanaryNames($labels, [int]$max = 60) {
  $s = (@($labels) | ForEach-Object { if ($_.Length -gt 40) { $_.Substring(0, 37) + '...' } else { $_ } }) -join '; '
  if ($s.Length -gt $max) { $s = $s.Substring(0, $max - 3) + '...' }
  return $s
}

# Verifies tasks/NNN.canary.patch against the Canary: line of tasks/NNN-*.md in the tree at -Root: baseline (every
# item runs and passes), the whole patch (every item red), then each hunk alone (matrix). The tree is restored.
# Returns {Problems; Lines (for display); Red; Matrix {Where; Red; Green}; Dirty}.
function Invoke-Canary {
  param([Parameter(Mandatory)][string]$Number, [Parameter(Mandatory)][string]$Root, [switch]$NoBaseline, [switch]$NoMatrix)
  $res = [pscustomobject]@{ Problems = @(); Lines = @(); Red = @(); Matrix = @(); Dirty = $false; Targets = @() }
  $dir = Join-Path $Root 'out/canary'
  New-Item -ItemType Directory -Force $dir | Out-Null
  $script:CanaryLogPath = Join-Path $dir "$Number.log"
  Set-Content -Path $script:CanaryLogPath -Value "canary $Number $(Get-Date -Format s)"
  $patch = Join-Path $Root "tasks/$Number.canary.patch"
  $taskFile = @(Get-ChildItem (Join-Path $Root 'tasks') -Filter "$Number-*.md")
  if ($taskFile.Count -ne 1) { $res.Problems += "expected one tasks/$Number-*.md, found $($taskFile.Count)"; return $res }
  if (-not (Test-Path $patch)) { $res.Problems += "missing tasks/$Number.canary.patch"; return $res }
  $line = [regex]::Match((Get-Content $taskFile[0].FullName -Raw), '(?m)^Canary:[ \t]*(.*)$').Groups[1].Value.Trim()
  if ($line -notmatch 'must turn red:') { $res.Problems += 'canary patch present but the Canary: line names no tests'; return $res }
  try { $items = @(Get-CanaryItems $line) } catch { $res.Problems += $_.Exception.Message; return $res }
  if ($items.Count -eq 0) { $res.Problems += 'canary names no test'; return $res }
  $unnamed = @($items | Where-Object { $_.Kind -ne 'cmd' -and -not $_.Name })
  if ($unnamed.Count -gt 0) { $res.Problems += "canary names no test: $($unnamed[0].Target)"; return $res }
  $pp = @(Get-CanaryPatchProblems $patch $Root $Number)
  if ($pp.Count) { $res.Problems += $pp; return $res }

  $cpuOnly = @($items | Where-Object { $_.Kind -ne 'exe' -or $_.Target -ne 'astroray_tests_cpu' }).Count -eq 0
  $targets = if (@($items | Where-Object { $_.Kind -ne 'exe' }).Count) { @('all') } else { @($items.Target | Sort-Object -Unique) }
  $res.Targets = $targets   # land rebuilds these after the canary: the last build here may hold a patched tree
  $key = ($items | ForEach-Object { "$($_.Kind)|$($_.Target)|$($_.Name)" }) -join "`n"
  $tree = (& git -C $Root rev-parse 'HEAD^{tree}').Trim()
  $clean = -not (& git -C $Root status --porcelain)
  $statusBefore = @(& git -C $Root status --porcelain)
  $record = Join-Path $dir "$Number.json"
  $prior = if ($clean -and (Test-Path $record)) { Get-Content $record -Raw | ConvertFrom-Json } else { $null }
  $hunks = @(Split-CanaryPatch (Get-Content $patch -Raw))
  $applied = $null
  $eval = { param($results, $mode)
    $red = @(); $green = @(); $probs = @()
    foreach ($r in $results) {
      if ($r.Error) { $probs += "canary item errored: $($r.Label): $($r.Error)" }
      elseif (-not $r.Ran) { $probs += "canary names no test: $($r.Label)" }
      elseif ($mode -eq 'base' -and $r.Failed) { $probs += "canary item fails without the patch: $($r.Label)" }
      elseif ($mode -eq 'whole' -and -not $r.Failed) { $probs += "canary inert: $($r.Label) still passes" }
      elseif ($r.Failed) { $red += $r.Label } else { $green += $r.Label }
    }
    [pscustomobject]@{ Red = $red; Green = $green; Problems = $probs } }
  try {
    if (-not $cpuOnly) { $null = Enter-ArMachineLock -Job canary }
    # Baseline: every item must run and pass without the patch, or "red" means nothing.
    $null = Invoke-CanaryBuild $targets $cpuOnly $Root
    if ($NoBaseline) { $res.Lines += 'baseline: skipped on request (-NoBaseline)' }
    elseif (Test-CanaryBaselineRecord $prior $tree $key $clean) { $res.Lines += "baseline: skipped, tree $($tree.Substring(0, 8)) passed at $($prior.time)" }
    else {
      $b = & $eval (Invoke-CanaryItems $items $Root) 'base'
      $res.Problems += $b.Problems
      if (-not $b.Problems.Count -and $clean) {
        @{ tree = $tree; key = $key; time = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') } | ConvertTo-Json | Set-Content -Path $record
      }
    }
    if ($res.Problems.Count -eq 0) {
      & git -C $Root apply $patch 2>&1 | ForEach-Object { Write-CanaryLog "$_" }
      if ($LASTEXITCODE -ne 0) { throw "git apply of tasks/$Number.canary.patch failed" }
      $applied = $patch
      $se = Invoke-CanaryBuild $targets $cpuOnly $Root $true
      if ($se) { $res.Lines += "whole patch: $se; its items count red" }
      $w = & $eval (Invoke-CanaryPatchedItems $items $se $Root) 'whole'
      $res.Problems += $w.Problems
      $res.Red = $w.Red
      & git -C $Root apply -R $patch 2>&1 | ForEach-Object { Write-CanaryLog "$_" }
      if ($LASTEXITCODE -ne 0) { throw 'git apply -R of the canary patch failed' }
      $applied = $null
      $res.Lines += if ($w.Red.Count -le 6) { @($w.Red | ForEach-Object { "red: $_" }) } else { @("red: all $($w.Red.Count) named cases (list in out/canary/$Number.log)") }
      foreach ($r in $w.Red) { Write-CanaryLog "red: $r" }
    }
    if ($res.Problems.Count -eq 0) {
      if ($NoMatrix) { $res.Lines += 'canary: matrix skipped on request (-NoMatrix)' }
      elseif ($hunks.Count -gt 8) { $res.Lines += "WARN canary: matrix skipped ($($hunks.Count) hunks > 8)" }
      elseif ($hunks.Count -le 1) {
        $res.Matrix += [pscustomobject]@{ Where = $(if ($hunks.Count) { $hunks[0].Where } else { 'patch' }); Red = $res.Red; Green = @() }
      } else {
        for ($h = 0; $h -lt $hunks.Count; $h++) {
          $hp = Join-Path $dir "$Number.hunk$($h + 1).patch"
          [IO.File]::WriteAllText($hp, $hunks[$h].Text, [Text.UTF8Encoding]::new($false))
          & git -C $Root apply $hp 2>&1 | ForEach-Object { Write-CanaryLog "$_" }
          if ($LASTEXITCODE -ne 0) { $res.Problems += "hunk $($hunks[$h].Where) does not apply alone (hunks must not depend on each other)"; continue }
          $applied = $hp
          try {
            $se = Invoke-CanaryBuild $targets $cpuOnly $Root $true
            if ($se) { $res.Lines += "hunk $($hunks[$h].Where): $se; its items count red" }
            $e = & $eval (Invoke-CanaryPatchedItems $items $se $Root) 'hunk'
          } catch { $res.Problems += "hunk $($hunks[$h].Where) alone: $($_.Exception.Message) (hunks must not depend on each other)"; $e = $null }
          finally {
            & git -C $Root apply -R $hp 2>&1 | ForEach-Object { Write-CanaryLog "$_" }
            if ($LASTEXITCODE -ne 0) { $res.Dirty = $true } else { $applied = $null }
          }
          if ($res.Dirty) { break }
          if ($e) { $res.Problems += $e.Problems; $res.Matrix += [pscustomobject]@{ Where = $hunks[$h].Where; Red = $e.Red; Green = $e.Green } }
        }
        if ($res.Problems.Count -eq 0) {
          foreach ($r in $res.Red) {
            if (-not @($res.Matrix | Where-Object { $_.Red -contains $r }).Count) {
              $res.Problems += "canary '$r' turns red only with several hunks together; hunks must not need each other (one breakage per test group)"
            }
          }
        }
      }
      foreach ($row in $res.Matrix) {
        $res.Lines += "hunk $($row.Where): red $(@($row.Red).Count): $(Format-CanaryNames $row.Red); green $(@($row.Green).Count)"
        if ($hunks.Count -gt 1 -and -not @($row.Red).Count) { $res.Lines += "WARN canary: inert hunk $($row.Where) turns nothing red" }
      }
    }
  } catch {
    $res.Problems += $_.Exception.Message
  } finally {
    Exit-ArMachineLock
    if ($applied) {
      & git -C $Root apply -R $applied 2>&1 | ForEach-Object { Write-CanaryLog "$_" }
      if ($LASTEXITCODE -ne 0) { $res.Dirty = $true }
    }
  }
  if (-not $res.Dirty -and (@(& git -C $Root status --porcelain) -join "`n") -ne ($statusBefore -join "`n")) {
    $res.Problems += 'tree not clean after the canary check'
  }
  foreach ($l in $res.Lines) { Write-CanaryLog $l }
  foreach ($p in $res.Problems) { Write-CanaryLog "problem: $p" }
  return $res
}
