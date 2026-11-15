# SPDX-License-Identifier: Apache-2.0
# tools/land.ps1 prepare|merge NNN [-DryRun]   (lead only)
# Ref: docs/decisions/0009-build-test-process.md d25 (gate status), d26 (landing), d28 (task and canary format).
# prepare (in the task worktree): branch and tree checks, rebase on origin/main, canary verification,
#   reviewer inputs in out/land/NNN/, deletion commit, push, gate, status `astroray/gate`.
# merge: PR head == gated sha, `ci` and `astroray/gate` success, lead comment `REVIEW PASS <sha>`
#   (or `REVIEW SKIP <sha> (O31)` for a diff that is not kernel, physics, sampling, ABI or process),
#   an `evidence:` comment when the task has `Look at:`; squash, confirm on origin/main, clean up.
# -DryRun: no push, no gh call, no commit, no gate run; prints what would happen (fixture test).
# The gate is `check gate` (task 030); its last summary line is the status description.
param(
  [Parameter(Mandatory, Position = 0)][ValidateSet('prepare', 'merge')][string]$Mode,
  [Parameter(Mandatory, Position = 1)][ValidatePattern('^\d{3}$')][string]$Number,
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib/lock.ps1')
. (Join-Path $PSScriptRoot 'lib/python.ps1')
. (Join-Path $PSScriptRoot 'lib/blender.ps1')
$root = (Resolve-Path ((& git rev-parse --show-toplevel).Trim())).Path
Set-Location $root

function Refuse([string]$why) { Write-Host "REFUSE land: $why"; exit 1 }
function GitX {
  $o = & git @args 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0) { throw "git $($args -join ' ') failed: $($o -join ' ')" }
  return $o
}
function GhX {
  $o = & gh @args 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0) { throw "gh $($args -join ' ') failed: $($o -join ' ')" }
  return ($o -join "`n")
}

# Items of a Canary: line (0009 d28; task 010 step 3). Returns objects {Kind exe|py|cmd; Target; Name}.
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

function Invoke-CanaryItem($it) {
  # Returns {Label; Ran; Failed; Error}. Ran: the named test exists and ran. Failed: a real test failure.
  # Error: the item could not be judged (missing script, crash, timeout); it is never counted as red.
  function New-CanaryResult($label, $ran, $failed, $err = '') { [pscustomobject]@{ Label = $label; Ran = $ran; Failed = $failed; Error = $err } }
  if ($it.Kind -eq 'cmd') {
    $tok = @($it.Target -split '\s+')
    foreach ($t in $tok) {
      if ($t -match '\.(py|ps1)$' -and -not (Test-Path (Join-Path $root $t))) { return New-CanaryResult $it.Target $false $false "script '$t' does not exist" }
    }
    if ($tok[0] -notmatch '\.py$' -and -not (Get-Command $tok[0] -ErrorAction SilentlyContinue)) { return New-CanaryResult $it.Target $false $false "command '$($tok[0])' not found" }
    if ($tok[0] -match '\.py$') { & (Get-ArPython) -I @tok 2>&1 | Out-Null }
    else { & pwsh -NoProfile -Command $it.Target 2>&1 | Out-Null }
    return New-CanaryResult $it.Target $true ($LASTEXITCODE -ne 0)
  }
  if ($it.Kind -eq 'exe') {
    # The .cmd fallback lets the land fixture stand in for a test binary.
    $p = Join-Path $root "out/bin/$($it.Target).exe"
    if (-not (Test-Path $p)) { $p = Join-Path $root "out/bin/$($it.Target).cmd" }
    if (-not (Test-Path $p)) { return New-CanaryResult $it.Name $false $false "missing out/bin/$($it.Target).exe after the build" }
    $out = & $p --tier=all ('-tc=' + ($it.Name -replace ',', '\,')) 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $txt = $out -join "`n"
    $m = [regex]::Match($txt, 'test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed')
    if (-not $m.Success) { return New-CanaryResult $it.Name $false $false "$($it.Target) printed no doctest summary (crash?)" }
    $nFail = [int]$m.Groups[3].Value
    if ($code -ne 0 -and $nFail -eq 0) { return New-CanaryResult $it.Name $true $false "$($it.Target) exited $code with no failed case" }
    return New-CanaryResult $it.Name ([int]$m.Groups[1].Value -gt 0) ($nFail -gt 0)
  }
  $rel = $it.Target -replace '\\', '/'
  if ($rel -like 'tests/abi/*') {
    $out = & (Get-ArPython) -I -m unittest discover -s (Split-Path $rel) -p (Split-Path $rel -Leaf) -k $it.Name 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $txt = $out -join "`n"
    if ($txt -match '_FailedTest|ModuleNotFoundError|ImportError|SyntaxError') { return New-CanaryResult $it.Name $false $false 'test module failed to import' }
    $m = [regex]::Match($txt, 'Ran (\d+) test')
    return New-CanaryResult $it.Name ($m.Success -and [int]$m.Groups[1].Value -gt 0) ($code -ne 0)
  }
  if ($rel -like 'tests/blender/*') {
    $bargs = @{ Script = $rel; Args = @('-k', $it.Name) }
    if ((Split-Path $rel -Leaf) -like 'gui_*') {
      if (-not (Get-Command Invoke-ArBlender).Parameters.ContainsKey('Gui')) { return New-CanaryResult $it.Name $false $false "$rel needs Invoke-ArBlender -Gui (task 085)" }
      $bargs.Gui = $true
    }
    $r = Invoke-ArBlender @bargs
    if ($r.ExitCode -eq -1) { return New-CanaryResult $it.Name $false $false 'Blender timed out' }
    $log = ''
    foreach ($f in $r.Log, "$($r.Log).err") { if (Test-Path $f) { $log += Get-Content $f -Raw } }
    # --python-exit-code 1 is how a failing test script exits; any other code is a crash.
    if ($r.ExitCode -ne 0 -and $r.ExitCode -ne 1) { return New-CanaryResult $it.Name $false $false "Blender exited $($r.ExitCode) (crash?)" }
    return New-CanaryResult $it.Name $log.Contains($it.Name) ($r.ExitCode -ne 0)
  }
  return New-CanaryResult $it.Name $false $false "no runner for canary test file '$rel' (tests/abi and tests/blender only)"
}

function Get-TaskField([string]$text, [string]$field) {
  $m = [regex]::Match($text, "(?m)^$field\s*(.*)$")
  if ($m.Success) { return $m.Groups[1].Value.Trim() } else { return '' }
}

function Invoke-Prepare {
  $branch = (GitX branch --show-current)
  if ($branch -notmatch "^task/$Number-") { Refuse "branch '$branch' is not task/$Number-*" }
  if ((GitX status --porcelain)) { Refuse 'working tree is not clean' }
  $null = GitX fetch origin
  # A re-prepare (main moved after a first prepare) first drops this script's own
  # task-file deletion commit, so the canary and the task file are back for the checks.
  $headMsg = (GitX log -1 --format=%s)
  if ($headMsg -match ' \(land\)$' -and -not (Test-Path (Join-Path $root "tasks/$Number.canary.patch")) -and -not @(Get-ChildItem (Join-Path $root 'tasks') -Filter "$Number-*.md").Count) {
    Write-Host "re-prepare: dropping previous land commit '$headMsg'"
    $null = GitX reset -q --hard HEAD~1
  }
  & git rebase origin/main 2>&1 | ForEach-Object { Write-Host $_ }
  if ($LASTEXITCODE -ne 0) { & git rebase --abort 2>&1 | Out-Null; Refuse 'rebase on origin/main failed; resolve by hand' }

  $taskFile = @(Get-ChildItem (Join-Path $root 'tasks') -Filter "$Number-*.md")
  if ($taskFile.Count -ne 1) { Refuse "expected one tasks/$Number-*.md, found $($taskFile.Count)" }
  $taskText = Get-Content $taskFile[0].FullName -Raw
  $title = ($taskText -split "`n")[0].TrimStart('# ').Trim()
  $names = @(GitX diff --name-only origin/main HEAD)
  # A header under tests/ that defines no test case (e.g. the precompiled header) is
  # support code, not a test; every other changed file under tests/ needs the canary.
  $testsChanged = @($names | Where-Object {
    $_ -like 'tests/*' -and -not ($_ -like '*.h' -and (Test-Path (Join-Path $root $_)) -and
      -not (Select-String -Path (Join-Path $root $_) -Pattern 'TEST_CASE|AR_RT_CASE' -Quiet))
  }).Count -gt 0
  $caseLines = @(GitX diff -U0 origin/main HEAD | Where-Object { $_ -match '^[+-][^+-].*TEST_CASE' }).Count -gt 0
  $patch = Join-Path $root "tasks/$Number.canary.patch"
  $canaryLine = Get-TaskField $taskText 'Canary:'
  if (($testsChanged -or $caseLines) -and -not (Test-Path $patch)) { Refuse 'missing canary patch' }
  if ((Test-Path $patch) -and $canaryLine -notmatch 'must turn red:') { Refuse 'canary patch present but the Canary: line names no tests' }

  if (Test-Path $patch) {
    $items = @(Get-CanaryItems $canaryLine)
    if ($items.Count -eq 0) { Refuse 'canary names no test' }
    $unnamed = @($items | Where-Object { $_.Kind -ne 'cmd' -and -not $_.Name })
    if ($unnamed.Count -gt 0) { Refuse "canary names no test: $($unnamed[0].Target)" }
    & git apply --check $patch 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Refuse "tasks/$Number.canary.patch does not apply" }
    $cpuOnly = @($items | Where-Object { $_.Kind -ne 'exe' -or $_.Target -ne 'astroray_tests_cpu' }).Count -eq 0
    $targets = if (@($items | Where-Object { $_.Kind -ne 'exe' }).Count) { @('all') } else { @($items.Target | Sort-Object -Unique) }
    $problems = @()
    $applied = $false
    function Build-Canary {
      foreach ($t in $targets) {
        $b = @('-NoProfile', '-File', (Join-Path $root 'tools/build.ps1'), '-Target', $t)
        if ($cpuOnly) { $b += '-NoMachineLock' }
        & pwsh @b 2>&1 | Select-Object -Last 3 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "canary build of '$t' failed" }
      }
    }
    try {
      if (-not $cpuOnly) { $null = Enter-ArMachineLock }
      # Baseline: every item must run and pass without the patch, or "red" means nothing.
      Build-Canary
      foreach ($it in $items) {
        $r = Invoke-CanaryItem $it
        if ($r.Error) { $problems += "canary item errored: $($r.Label): $($r.Error)" }
        elseif (-not $r.Ran) { $problems += "canary names no test: $($r.Label)" }
        elseif ($r.Failed) { $problems += "canary item fails without the patch: $($r.Label)" }
      }
      if ($problems.Count -eq 0) {
        $null = GitX apply $patch
        $applied = $true
        Build-Canary
        foreach ($it in $items) {
          $r = Invoke-CanaryItem $it
          if ($r.Error) { $problems += "canary item errored: $($r.Label): $($r.Error)" }
          elseif (-not $r.Ran) { $problems += "canary names no test: $($r.Label)" }
          elseif (-not $r.Failed) { $problems += "canary inert: $($r.Label) still passes" }
          else { Write-Host "red: $($r.Label)" }
        }
      }
    } finally {
      Exit-ArMachineLock
      if ($applied) {
        & git apply -R $patch 2>&1 | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL land: git apply -R of the canary patch failed; the tree is dirty'; exit 1 }
      }
    }
    if ((GitX status --porcelain)) { Refuse 'tree not clean after the canary check' }
    if ($problems.Count) {
      $problems | ForEach-Object { Write-Host "REFUSE land: $_" }
      exit 1
    }
    # Applying and reverting the canary rewrote source files, so rebuild them now: the gate's fast
    # tier then times the incremental build of the change itself (B-08), not this tool's own churn.
    & pwsh -NoProfile -File (Join-Path $root 'tools/build.ps1') -Target astroray_tests_cpu 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { Refuse 'build after the canary check failed' }
  } else {
    Write-Host 'canary: none (no tests changed, no patch)'
  }

  $dir = Join-Path $root "out/land/$Number"
  New-Item -ItemType Directory -Force $dir | Out-Null
  Copy-Item $taskFile[0].FullName (Join-Path $dir 'task.md') -Force
  $null = GitX diff origin/main HEAD "--output=$(Join-Path $dir 'diff.patch')"
  Write-Host "reviewer inputs: out/land/$Number/task.md, out/land/$Number/diff.patch"
  if ($DryRun) {
    Write-Host "DRYRUN would: commit deletion of tasks/$Number*, push $branch, run check gate, post astroray/gate"
    Write-Host "PASS prepare $Number (dry run: canary accepted)"
    return
  }

  $rm = @($taskFile[0].FullName)
  if (Test-Path $patch) { $rm += $patch }
  $null = GitX rm -q -- @rm
  $null = GitX commit -q -m "$title (land)"
  $sha = (GitX rev-parse HEAD)
  $null = GitX push --force-with-lease -u origin HEAD
  $out = & pwsh -NoProfile -File (Join-Path $root 'tools/check.ps1') gate 2>&1 | ForEach-Object { "$_" }
  $state = if ($LASTEXITCODE -eq 0) { 'success' } else { 'failure' }
  $out | Select-Object -Last 4 | ForEach-Object { Write-Host $_ }
  $desc = ($out | Select-Object -Last 1)
  if ($desc.Length -gt 140) { $desc = $desc.Substring(0, 140) }
  $null = GhX api "repos/{owner}/{repo}/statuses/$sha" -f "state=$state" -f 'context=astroray/gate' -f "description=$desc"
  Set-Content -Path (Join-Path $dir 'gate.json') -Value (@{ sha = $sha; state = $state; description = $desc } | ConvertTo-Json)
  $pr = GhX pr list --head $branch --state open --json number | ConvertFrom-Json
  if (-not $pr) { Write-Host "no PR yet: gh pr create --base main --head $branch --fill" }
  Write-Host "$(if ($state -eq 'success') { 'PASS' } else { 'FAIL' }) prepare $Number sha $($sha.Substring(0, 9)) astroray/gate $state"
  if ($state -ne 'success') { exit 1 }
}

function Invoke-Merge {
  $branch = @(GitX for-each-ref --format='%(refname:short)' "refs/heads/task/$Number-*")
  if ($branch.Count -ne 1) { Refuse "expected one branch task/$Number-*, found $($branch.Count)" }
  $branch = $branch[0]
  $wt = $null; $main = $null; $cur = $null
  foreach ($l in (GitX worktree list --porcelain)) {
    if ($l -like 'worktree *') { $cur = $l.Substring(9); if (-not $main) { $main = $cur } }
    if ($l -eq "branch refs/heads/$branch") { $wt = $cur }
  }
  if ($DryRun) {
    Write-Host "DRYRUN would: fetch; require origin/main ancestor of the gated sha (else 're-run prepare')"
    Write-Host "DRYRUN would: require PR head == gated sha, ci and astroray/gate success, lead comment REVIEW PASS <sha> (or REVIEW SKIP <sha> (O31)), evidence: comment if Look at: is not none"
    Write-Host "DRYRUN would: gh pr merge --squash, confirm on origin/main, delete $branch, worktree $wt and its out/"
    return
  }
  $gj = if ($wt) { Join-Path $wt "out/land/$Number/gate.json" } else { '' }
  if (-not $gj -or -not (Test-Path $gj)) { Refuse "no out/land/$Number/gate.json in the worktree; run prepare first" }
  $gate = Get-Content $gj -Raw | ConvertFrom-Json
  $sha = $gate.sha
  $null = GitX fetch origin
  & git merge-base --is-ancestor origin/main $sha 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Write-Host 're-run prepare: origin/main moved past the gated sha'; exit 1 }
  $pr = @(GhX pr list --head $branch --state open --json 'number,headRefOid' | ConvertFrom-Json)
  if ($pr.Count -ne 1) { Refuse "expected one open PR for $branch, found $($pr.Count)" }
  if ($pr[0].headRefOid -ne $sha) { Refuse "PR head $($pr[0].headRefOid) is not the gated sha $sha" }
  $n = $pr[0].number
  $ci = $false
  $runs = GhX api "repos/{owner}/{repo}/commits/$sha/check-runs" | ConvertFrom-Json
  foreach ($r in $runs.check_runs) { if ($r.name -eq 'ci' -and $r.conclusion -eq 'success') { $ci = $true } }
  $st = @(GhX api "repos/{owner}/{repo}/commits/$sha/statuses" | ConvertFrom-Json)
  $g = $st | Where-Object { $_.context -eq 'astroray/gate' } | Select-Object -First 1
  if (-not $ci) { $c = $st | Where-Object { $_.context -eq 'ci' } | Select-Object -First 1; $ci = ($c -and $c.state -eq 'success') }
  if (-not $ci) { Refuse "ci is not success on $sha" }
  if (-not $g -or $g.state -ne 'success') { Refuse "astroray/gate is not success on $sha" }
  Write-Host "ci success; astroray/gate success: $($g.description)"
  $lead = (GhX repo view --json owner --jq .owner.login)
  $cm = (GhX pr view $n --json comments | ConvertFrom-Json).comments | Where-Object { $_.author.login -eq $lead }
  $body = @($cm | ForEach-Object { $_.body }) -join "`n"
  $pass = [regex]::Matches($body, '(?m)^REVIEW PASS ([0-9a-f]{7,40})\b') | Where-Object { $sha.StartsWith($_.Groups[1].Value) }
  $skip = [regex]::Matches($body, '(?m)^REVIEW SKIP ([0-9a-f]{7,40}) \(O31\)') | Where-Object { $sha.StartsWith($_.Groups[1].Value) }
  if (-not $pass -and -not $skip) { Refuse "no lead comment 'REVIEW PASS $($sha.Substring(0, 9))' (or 'REVIEW SKIP <sha> (O31)')" }
  $look = Get-TaskField (Get-Content (Join-Path $wt "out/land/$Number/task.md") -Raw) 'Look at:'
  if ($look -and $look -notmatch '^none\b' -and $body -notmatch '(?m)^evidence:') { Refuse 'task has Look at: but no lead comment starting evidence:' }

  Set-Location $main
  $null = GhX pr merge $n --squash --match-head-commit $sha
  $null = GitX fetch origin
  $mc = (GhX pr view $n --json mergeCommit | ConvertFrom-Json).mergeCommit.oid
  & git merge-base --is-ancestor $mc origin/main 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Refuse "squash commit $mc is not on origin/main" }
  # Report exactly what was and was not removed.
  $bad = 0
  $remote = @(& git ls-remote --heads origin $branch 2>&1)
  if ($LASTEXITCODE -ne 0) { Write-Host "cleanup: ls-remote failed: $($remote -join ' ')"; $bad++ }
  elseif ($remote.Count -eq 0) { Write-Host "cleanup: remote branch $branch already gone" }
  else {
    $o = & git push origin --delete $branch 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -eq 0) { Write-Host "cleanup: remote branch $branch deleted" } else { Write-Host "cleanup: NOT removed remote branch ${branch}: $($o -join ' ')"; $bad++ }
  }
  $o = & git worktree remove --force $wt 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -eq 0) { Write-Host "cleanup: worktree $wt and its out/ removed" } else { Write-Host "cleanup: NOT removed worktree ${wt}: $($o -join ' ')"; $bad++ }
  $o = & git branch -D $branch 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -eq 0) { Write-Host "cleanup: local branch $branch deleted" } else { Write-Host "cleanup: NOT removed local branch ${branch}: $($o -join ' ')"; $bad++ }
  if ($bad -gt 0) { Write-Host "FAIL merge ${Number}: squash $($mc.Substring(0, 9)) is on origin/main but $bad cleanup step(s) failed"; exit 1 }
  Write-Host "PASS merge $Number squash $($mc.Substring(0, 9)) on origin/main; branch and worktree removed"
}

try {
  if ($Mode -eq 'prepare') { Invoke-Prepare } else { Invoke-Merge }
} catch {
  Write-Host "FAIL land $Mode ${Number}: $($_.Exception.Message)"
  exit 1
}
