# SPDX-License-Identifier: Apache-2.0
# Canary verification shared by tools/canary.ps1 (implementers) and tools/land.ps1 (prepare). Dot-source this file.
# Ref: docs/decisions/0009-build-test-process.md d26 step 3, d28 (task and canary format).
# Get-CanaryItems below is the real parse of the `Canary:` line; tools/lint.py taskfile() only checks its shape
# and must stay in step with it.
. (Join-Path $PSScriptRoot 'blender.ps1')

$script:CanaryLogPath = $null
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

# Outcome of one case in verbosity-2 unittest output. The header line `test_x (mod.Class.test_x)` carries the
# name; its outcome `... ok|FAIL|ERROR` is on the same line, or, after a docstring line or text the test
# printed, on a later line before the next test header.
function Get-UnittestOutcome([string]$text, [string]$name) {
  $ran = $false; $failed = $false; $pending = $false
  foreach ($ln in ($text -split "`r?`n")) {
    if ($ln -match '^\w+ \([\w.]+\)') { $pending = $ln.Contains($name) }
    if ($pending -and $ln -match '\.\.\. (ok|FAIL|ERROR)\s*$') {
      $ran = $true; $pending = $false
      if ($Matches[1] -ne 'ok') { $failed = $true }
    }
  }
  return [pscustomobject]@{ Ran = $ran; Failed = $failed }
}

# The error for a Blender exit code, or '': --python-exit-code 1 is how a failing test script exits, -1 is
# this tool's timeout, any other code is a crash.
function Get-BlenderExitError([int]$code) {
  if ($code -eq -1) { return 'Blender timed out' }
  if ($code -ne 0 -and $code -ne 1) { return "Blender exited $code (crash?)" }
  return ''
}

# Results of one file group from its output; a non-empty $err marks every case of the group errored.
function Get-CanaryGroupResults($group, [string]$txt, [string]$err) {
  return @($group | ForEach-Object {
    if ($err) { New-CanaryResult $_.Name $false $false $err }
    else { $o = Get-UnittestOutcome $txt $_.Name; New-CanaryResult $_.Name $o.Ran $o.Failed }
  })
}

# Whether a passed baseline recorded earlier still holds: only a clean tree at the same tree hash and the
# same items. A dirty tree's HEAD hash says nothing about the files that ran.
function Test-CanaryBaselineRecord($prior, [string]$tree, [string]$key, [bool]$clean) {
  return [bool]($clean -and $prior -and $prior.tree -eq $tree -and $prior.key -eq $key)
}

# Items of one tests/abi or tests/blender file (not gui_*) run in one start; each case is read from its line.
function Invoke-CanaryFileGroup($group, [string]$rel, [string]$Root) {
  $kargs = @($group | ForEach-Object { '-k'; $_.Name })
  $err = ''
  if ($rel -like 'tests/abi/*') {
    $out = & (Get-ArPython) -I -m unittest discover -s (Split-Path $rel) -p (Split-Path $rel -Leaf) -v @kargs 2>&1 | ForEach-Object { "$_" }
    $txt = $out -join "`n"
    Write-CanaryLog $out
    if ($txt -match '_FailedTest|ModuleNotFoundError|ImportError|SyntaxError') { $err = 'test module failed to import' }
  } else {
    $r = Invoke-ArBlender -Script $rel -Args $kargs 6>$null
    $txt = ''
    foreach ($f in $r.Log, "$($r.Log).err") { if (Test-Path $f) { $txt += (Get-Content $f -Raw) } }
    Write-CanaryLog $txt
    $err = Get-BlenderExitError $r.ExitCode
  }
  return Get-CanaryGroupResults $group $txt $err
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
    # The .cmd fallback lets the land fixture stand in for a test binary.
    $p = Join-Path $Root "out/bin/$($it.Target).exe"
    if (-not (Test-Path $p)) { $p = Join-Path $Root "out/bin/$($it.Target).cmd" }
    if (-not (Test-Path $p)) { return New-CanaryResult $it.Name $false $false "missing out/bin/$($it.Target).exe after the build" }
    $out = & $p --tier=all ('-tc=' + ($it.Name -replace ',', '\,')) 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    Write-CanaryLog $out
    $txt = $out -join "`n"
    $m = [regex]::Match($txt, 'test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed')
    if (-not $m.Success) { return New-CanaryResult $it.Name $false $false "$($it.Target) printed no doctest summary (crash?)" }
    $nFail = [int]$m.Groups[3].Value
    if ($code -ne 0 -and $nFail -eq 0) { return New-CanaryResult $it.Name $true $false "$($it.Target) exited $code with no failed case" }
    return New-CanaryResult $it.Name ([int]$m.Groups[1].Value -gt 0) ($nFail -gt 0)
  }
  $rel = $it.Target -replace '\\', '/'
  if ($rel -like 'tests/blender/*' -and (Split-Path $rel -Leaf) -like 'gui_*') {
    if (-not (Get-Command Invoke-ArBlender).Parameters.ContainsKey('Gui')) { return New-CanaryResult $it.Name $false $false "$rel needs Invoke-ArBlender -Gui (task 085)" }
    # Those scripts read a single -k: one windowed start per case.
    $r = Invoke-ArBlender -Script $rel -Args @('-k', $it.Name) -Gui 6>$null
    $log = ''
    foreach ($f in $r.Log, "$($r.Log).err") { if (Test-Path $f) { $log += Get-Content $f -Raw } }
    Write-CanaryLog $log
    $be = Get-BlenderExitError $r.ExitCode
    if ($be) { return New-CanaryResult $it.Name $false $false $be }
    return New-CanaryResult $it.Name $log.Contains($it.Name) ($r.ExitCode -ne 0)
  }
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

function Invoke-CanaryBuild($targets, [bool]$cpuOnly, [string]$Root) {
  foreach ($t in $targets) {
    $b = @('-NoProfile', '-File', (Join-Path $Root 'tools/build.ps1'), '-Target', $t)
    if ($cpuOnly) { $b += '-NoMachineLock' }
    $o = & pwsh @b 2>&1 | ForEach-Object { "$_" }
    Write-CanaryLog $o
    if ($LASTEXITCODE -ne 0) { throw "canary build of '$t' failed: $(($o | Select-Object -Last 2) -join ' ')" }
  }
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
  $res = [pscustomobject]@{ Problems = @(); Lines = @(); Red = @(); Matrix = @(); Dirty = $false }
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
    if (-not $cpuOnly) { $null = Enter-ArMachineLock }
    # Baseline: every item must run and pass without the patch, or "red" means nothing.
    Invoke-CanaryBuild $targets $cpuOnly $Root
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
      Invoke-CanaryBuild $targets $cpuOnly $Root
      $w = & $eval (Invoke-CanaryItems $items $Root) 'whole'
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
            Invoke-CanaryBuild $targets $cpuOnly $Root
            $e = & $eval (Invoke-CanaryItems $items $Root) 'hunk'
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
