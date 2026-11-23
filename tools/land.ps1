# SPDX-License-Identifier: Apache-2.0
# tools/land.ps1 prepare|merge NNN [-DryRun]   |   submit NNN PASS|SKIP [-Title t] [-Evidence text] [-CiDeadlineMin 45] [-DryRun]   (lead only)
# Ref: docs/decisions/0009-build-test-process.md d25 (gate status), d26 (landing), d28 (task and canary format).
# prepare (in the task worktree): refusals (another land.ps1 running, out/bin/astroray_core.dll held), branch and tree
#   checks, rebase on origin/main, canary verification (lib/canary.ps1, as tools/canary.ps1; every build is followed by
#   tools/scenes.py), the Blender suites for a diff under blender/ or tests/blender/ (lib/blender.ps1, the list `check
#   full` runs), reviewer inputs in out/land/NNN/, deletion commit, push, gate, status `astroray/gate`. A failure after
#   the commit resets the branch to the sha before it.
# merge: PR head == gated sha, `ci` and `astroray/gate` success, lead comment `REVIEW PASS <sha>`
#   (or `REVIEW SKIP <sha> (O31)` for a diff that is not kernel, physics, sampling, ABI or process),
#   an `evidence:` comment when the task has `Look at:`; squash, confirm on origin/main, PASS, then best-effort cleanup
#   (lib/landing.ps1; a leftover is printed as LEFTOVER and never fails the landing).
# submit: prepare, open or reuse the PR, post the verdict (and evidence) comments, wait for hosted `ci` on that sha,
#   merge; main moved ("re-run prepare") repeats everything, at most 3 attempts. One timing line per landing goes to
#   out/land/history.jsonl of the main checkout. A failed ci run is printed with its id and never rerun here.
# -DryRun: no push, no gh call, no commit, no gate run; prints what would happen (fixture test).
# The gate is `check gate` (task 030); its last summary line is the status description.
param(
  [Parameter(Mandatory, Position = 0)][ValidateSet('prepare', 'merge', 'submit')][string]$Mode,
  [Parameter(Mandatory, Position = 1)][ValidatePattern('^\d{3}$')][string]$Number,
  [Parameter(Position = 2)][string]$Verdict = '',
  [string]$Title = '',
  [string]$Evidence = '',
  [int]$CiDeadlineMin = 45,
  [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib/canary.ps1')
. (Join-Path $PSScriptRoot 'lib/landing.ps1')
$root = (Resolve-Path ((& git rev-parse --show-toplevel).Trim())).Path
Set-Location $root
$mainCheckout = Get-LandingMain $root

# Every machine-lock wait this process sees (canary, Blender suites) is summed into the lock_wait phase.
$script:OrigEnterLock = ${function:Enter-ArMachineLock}
function Enter-ArMachineLock { $w = & $script:OrigEnterLock; $script:LandTimes['lock_wait_s'] += $w; return $w }

$script:UndoTo = $null      # the sha before this script's "(land)" commit; set while that commit may be undone
$script:TaskTitle = ''
$script:TaskLook = ''

function Undo-LandCommit {
  if (-not $script:UndoTo) { return }
  & git reset -q --hard $script:UndoTo 2>&1 | Out-Null
  if ($LASTEXITCODE -eq 0) { Write-Host 'undid (land) commit' } else { Write-Host "WARN land: could not reset the branch to $($script:UndoTo); drop the (land) commit by hand" }
  $script:UndoTo = $null
}
function Refuse([string]$why) { Undo-LandCommit; Write-Host "REFUSE land: $why"; exit 1 }
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

function Get-TaskField([string]$text, [string]$field) {
  $m = [regex]::Match($text, "(?m)^$field\s*(.*)$")
  if ($m.Success) { return $m.Groups[1].Value.Trim() } else { return '' }
}

function Assert-Idle {
  $run = Find-LandRunning
  if ($run) { Refuse "another land.ps1 is running ($run)" }
  $holder = Find-DllHolder $root
  if ($holder) { Refuse "out/bin/astroray_core.dll is loaded by $holder; close it (nothing is killed here)" }
}

function Invoke-Scenes {
  # Recordings before anything that renders; a warm machine-wide cache makes this cost well under a second.
  if (-not (Test-Path (Join-Path $root 'tools/scenes.py'))) { return }
  Measure-LandPhase 'scenes_s' {
    $o = & (Get-ArPython) -I (Join-Path $root 'tools/scenes.py') 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { Refuse "tools/scenes.py failed: $(($o | Select-Object -Last 2) -join ' ')" }
  }
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
  Measure-LandPhase 'rebase_s' {
    & git rebase origin/main 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) {
      $files = @(& git diff --name-only --diff-filter=U) -join ', '
      & git rebase --abort 2>&1 | Out-Null
      Refuse "rebase on origin/main conflicts in: $files; resolve by hand"
    }
  }

  $taskFile = @(Get-ChildItem (Join-Path $root 'tasks') -Filter "$Number-*.md")
  if ($taskFile.Count -ne 1) { Refuse "expected one tasks/$Number-*.md, found $($taskFile.Count)" }
  $taskText = Get-Content $taskFile[0].FullName -Raw
  $title = ($taskText -split "`n")[0].TrimStart('# ').Trim()
  $script:TaskTitle = $title
  $script:TaskLook = Get-TaskField $taskText 'Look at:'
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
    # The canary run is lib/canary.ps1's, the one tools/canary.ps1 gives implementers. Its builds run tools/scenes.py;
    # that time is reported as the scenes phase, not canary.
    $s0 = $script:CanaryScenesSeconds
    Measure-LandPhase 'canary_s' { $script:cr = Invoke-Canary -Number $Number -Root $root }
    $ds = $script:CanaryScenesSeconds - $s0
    $script:LandTimes['scenes_s'] += $ds; $script:LandTimes['canary_s'] -= $ds
    $cr = $script:cr
    $cr.Lines | ForEach-Object { Write-Host $_ }
    if ($cr.Dirty) { Write-Host 'FAIL land: git apply -R of the canary patch failed; the tree is dirty'; exit 1 }
    if ($cr.Problems.Count) {
      $cr.Problems | ForEach-Object { Write-Host "REFUSE land: $_" }
      exit 1
    }
  } else {
    Write-Host 'canary: none (no tests changed, no patch)'
  }
  # The canary's last build holds a broken or patched tree and the rebase brought in every change landed since the
  # branch began; rebuild (and regenerate recordings) before the Blender suites and the gate, so those run on this
  # tree and the gate's fast tier (B-08) times neither this tool's churn nor other tasks' changes.
  & pwsh -NoProfile -File (Join-Path $root 'tools/build.ps1') -Target astroray_tests_cpu 2>&1 | Select-Object -Last 2 | ForEach-Object { Write-Host $_ }
  if ($LASTEXITCODE -ne 0) { Refuse 'build before the gate failed' }
  Invoke-Scenes

  # A diff under blender/ or tests/blender/ runs the Blender suites `check full` runs (the gate has no Blender suite).
  if (@($names | Where-Object { $_ -like 'blender/*' -or $_ -like 'tests/blender/*' }).Count) {
    Measure-LandPhase 'blender_suites_s' {
      foreach ($su in (Get-ArFullBlenderSuites -Changed $names -Root $root)) {
        $b = Invoke-ArBlender -Gui:([bool]$su.Gui) -Script (Join-Path $root $su.Script) -TimeoutS $su.TimeoutS 6>$null
        if ($b.ExitCode -ne 0) { Refuse "blender suite $($su.Name) failed (exit $($b.ExitCode)); log $($b.Log)" }
        Write-Host "blender suite $($su.Name): ok"
      }
    }
  } else {
    Write-Host 'blender suites: none (no blender/ or tests/blender/ in the diff)'
  }

  $dir = Join-Path $root "out/land/$Number"
  New-Item -ItemType Directory -Force $dir | Out-Null
  Copy-Item $taskFile[0].FullName (Join-Path $dir 'task.md') -Force
  $null = GitX diff origin/main HEAD "--output=$(Join-Path $dir 'diff.patch')"
  $ri = "out/land/$Number/task.md, out/land/$Number/diff.patch"
  if (Test-Path $patch) { Copy-Item $patch (Join-Path $dir 'canary.patch') -Force; $ri += ", out/land/$Number/canary.patch" }
  Write-Host "reviewer inputs: $ri"
  if ($DryRun) {
    Write-Host "DRYRUN would: commit deletion of tasks/$Number*, push $branch, run check gate, post astroray/gate"
    Write-Host "PASS prepare $Number (dry run: canary accepted)"
    return
  }

  $rm = @($taskFile[0].FullName)
  if (Test-Path $patch) { $rm += $patch }
  $script:UndoTo = (GitX rev-parse HEAD)
  $null = GitX rm -q -- @rm
  $null = GitX commit -q -m "$title (land)"
  $sha = (GitX rev-parse HEAD)
  $null = GitX push --force-with-lease -u origin HEAD
  $out = @()
  Measure-LandPhase 'gate_s' { $script:gateOut = & pwsh -NoProfile -File (Join-Path $root 'tools/check.ps1') gate 2>&1 | ForEach-Object { "$_" } }
  $out = @($script:gateOut)
  $state = if ($LASTEXITCODE -eq 0) { 'success' } else { 'failure' }
  $out | Select-Object -Last 4 | ForEach-Object { Write-Host $_ }
  $desc = ($out | Select-Object -Last 1)
  if ($desc -match 'waited ([\d.]+) s') { $script:LandTimes['lock_wait_s'] += [double]$Matches[1] }
  if ($desc.Length -gt 140) { $desc = $desc.Substring(0, 140) }
  $null = GhX api "repos/{owner}/{repo}/statuses/$sha" -f "state=$state" -f 'context=astroray/gate' -f "description=$desc"
  Set-Content -Path (Join-Path $dir 'gate.json') -Value (@{ sha = $sha; state = $state; description = $desc } | ConvertTo-Json)
  if ($state -ne 'success') {
    Write-Host "FAIL prepare $Number sha $($sha.Substring(0, 9)) astroray/gate $state"
    Undo-LandCommit
    exit 1
  }
  if ($Mode -eq 'prepare') {
    $pr = GhX pr list --head $branch --state open --json number | ConvertFrom-Json
    if (-not $pr) { Write-Host "no PR yet: gh pr create --base main --head $branch --fill" }
  }
  $script:GatedSha = $sha
  Write-Host "PASS prepare $Number sha $($sha.Substring(0, 9)) astroray/gate $state"
}

function Find-CiRun([string]$branch, [string]$sha) {
  $runs = @(GhX run list --branch $branch --workflow ci --json 'databaseId,headSha' | ConvertFrom-Json)
  $m = @($runs | Where-Object { $_.headSha -eq $sha })
  if ($m.Count) { return $m[0].databaseId }
  return $null
}

function Wait-Ci([string]$branch, [string]$sha, [int]$pr) {
  # The hosted run must register within 5 min (else the PR is closed and reopened once, 3 min more), then
  # complete before the deadline. A failed run stops the landing with its id; nothing reruns it here.
  $run = $null
  $until = (Get-Date).AddMinutes(5)
  while (-not $run -and (Get-Date) -lt $until) { $run = Find-CiRun $branch $sha; if (-not $run) { Start-Sleep -Seconds 10 } }
  if (-not $run) {
    Write-Host "ci: no run registered for $($sha.Substring(0, 9)) in 5 min; closing and reopening PR #$pr once"
    $null = GhX pr close $pr
    $null = GhX pr reopen $pr
    $until = (Get-Date).AddMinutes(3)
    while (-not $run -and (Get-Date) -lt $until) { $run = Find-CiRun $branch $sha; if (-not $run) { Start-Sleep -Seconds 10 } }
  }
  if (-not $run) { Refuse "ci: no hosted run registered for $sha after reopening PR #$pr" }
  $deadline = (Get-Date).AddMinutes($CiDeadlineMin)
  while ($true) {
    $r = GhX run view $run --json 'status,conclusion,url' | ConvertFrom-Json
    if ($r.status -eq 'completed') {
      if ($r.conclusion -eq 'success') { Write-Host "ci: run $run success"; return }
      Refuse "ci run $run concluded $($r.conclusion): $($r.url) (not rerun; read the log, fix, resubmit)"
    }
    if ((Get-Date) -gt $deadline) { Refuse "ci run $run still $($r.status) after $CiDeadlineMin min: $($r.url) (not rerun)" }
    Start-Sleep -Seconds 20
  }
}

function Invoke-Merge {
  # Returns 'OK' (squash on origin/main, PASS printed) or 'RERUN' (main moved past the gated sha).
  $task = Find-LandingTask $mainCheckout $Number
  if (-not $task) { Refuse "expected one branch task/$Number-*, found none" }
  $script:Task = $task
  $branch = $task.Branch; $wt = $task.Worktree
  if ($DryRun) {
    Write-Host "DRYRUN would: fetch; require origin/main ancestor of the gated sha (else 're-run prepare')"
    Write-Host "DRYRUN would: require PR head == gated sha, ci and astroray/gate success, lead comment REVIEW PASS <sha> (or REVIEW SKIP <sha> (O31)), evidence: comment if Look at: is not none"
    Write-Host "DRYRUN would: gh pr merge --squash, confirm on origin/main, PASS, then clean up $branch, worktree $wt and its out/"
    return 'OK'
  }
  $gj = if ($wt) { Join-Path $wt "out/land/$Number/gate.json" } else { '' }
  if (-not $gj -or -not (Test-Path $gj)) { Refuse "no out/land/$Number/gate.json in the worktree; run prepare first" }
  $gate = Get-Content $gj -Raw | ConvertFrom-Json
  $sha = $gate.sha
  $null = GitX fetch origin
  & git merge-base --is-ancestor origin/main $sha 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Write-Host 're-run prepare: origin/main moved past the gated sha'; return 'RERUN' }
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

  Set-Location $mainCheckout
  $script:UndoTo = $null   # from here the branch is being merged; never reset it
  $null = GhX pr merge $n --squash --match-head-commit $sha
  $null = GitX fetch origin
  $mc = (GhX pr view $n --json mergeCommit | ConvertFrom-Json).mergeCommit.oid
  & git merge-base --is-ancestor $mc origin/main 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { Refuse "squash commit $mc is not on origin/main" }
  $script:MergeSha = $mc; $script:MergePr = $n
  Write-Host "PASS merge $Number squash $($mc.Substring(0, 9)) on origin/main"
  return 'OK'
}

function Invoke-Submit {
  if ($DryRun) {
    Write-Host "DRYRUN plan: prepare (rebase, canary with scenes, Blender suites for a Blender diff, rebuild, gate); open or reuse the PR"
    Write-Host "DRYRUN plan: comment REVIEW $Verdict <sha>$(if ($Evidence) { '; comment evidence:' }); wait for hosted ci on that sha (deadline $CiDeadlineMin min, no rerun)"
    Write-Host "DRYRUN plan: merge (on 're-run prepare' prepare again, at most 3 attempts); PASS, best-effort cleanup, timing line, history"
    Invoke-Prepare
    Write-Host "PASS submit $Number (dry run: nothing pushed)"
    return
  }
  $attempts = 0
  $result = 'RERUN'
  while ($result -eq 'RERUN') {
    if ($attempts -ge 3) { Refuse "main moved on each of $attempts attempts; submit again when it is quiet" }
    $attempts++
    if ($attempts -gt 1) { Write-Host "submit: attempt $attempts of 3" }
    Invoke-Prepare
    $sha = $script:GatedSha
    $branch = (GitX branch --show-current)
    $pr = @(GhX pr list --head $branch --state open --json number | ConvertFrom-Json)
    if ($pr.Count) { $n = $pr[0].number }
    else {
      $t = if ($Title) { $Title } else { ($script:TaskTitle -replace "^$Number\s+", "${Number}: ") }
      $bodyText = "Task $Number. Landed through tools/land.ps1.`n`n🤖 Generated with [Claude Code](https://claude.com/claude-code)"
      $url = GhX pr create --base main --head $branch --title $t --body $bodyText
      $n = [regex]::Match($url, '(\d+)\s*$').Groups[1].Value
      if (-not $n) { Refuse "gh pr create printed no PR number: $url" }
    }
    $null = GhX pr comment $n --body $(if ($Verdict -eq 'PASS') { "REVIEW PASS $sha" } else { "REVIEW SKIP $sha (O31)" })
    if ($Evidence) { $null = GhX pr comment $n --body $(if ($Evidence -match '^evidence:') { $Evidence } else { "evidence: $Evidence" }) }
    Measure-LandPhase 'ci_wait_s' { Wait-Ci $branch $sha ([int]$n) }
    Measure-LandPhase 'merge_s' { $script:mergeResult = Invoke-Merge }
    $result = $script:mergeResult
  }
  Complete-Submit $attempts
}

function Complete-Submit([int]$attempts) {
  # After the PASS line: timing, history, the PR body table, then cleanup. None of it fails a merged landing.
  $times = $script:LandTimes
  $prev = Get-LandingLastGate $mainCheckout
  Write-Host (Format-LandingTiming $times $attempts)
  $gate = [math]::Round($times['gate_s'], 1)
  if ($prev -and $null -ne $prev.gate_s) { Write-Host ("gate {0:N1} s ({1:+0.0;-0.0;+0.0} s against task {2})" -f $gate, ($gate - [double]$prev.gate_s), $prev.task) }
  else { Write-Host "gate $('{0:N1}' -f $gate) s (no earlier landing line)" }
  try {
    $rec = [ordered]@{ task = [int]$Number; sha = $script:MergeSha; date = (Get-Date -Format 's'); verdict = $Verdict; attempts = $attempts }
    foreach ($k in 'rebase_s', 'scenes_s', 'canary_s', 'blender_suites_s', 'gate_s', 'ci_wait_s', 'merge_s', 'lock_wait_s') { $rec[$k] = [math]::Round($times[$k], 1) }
    Add-LandingHistory $mainCheckout $rec
  } catch { Write-Host "WARN land: history line not written: $($_.Exception.Message)" }
  try {
    $table = "`n`n| phase | seconds |`n|---|---|`n" + ((@($times.Keys) | ForEach-Object { "| $($_ -replace '_s$', '') | $('{0:N1}' -f $times[$_]) |" }) -join "`n") + "`n| attempts | $attempts |"
    $old = (GhX pr view $script:MergePr --json body | ConvertFrom-Json).body
    $null = GhX pr edit $script:MergePr --body ($old + $table)
  } catch { Write-Host "WARN land: PR body not updated with timings: $($_.Exception.Message)" }
}

try {
  if ($Mode -ne 'merge') {
    if ($Mode -eq 'submit') {
      if ($Verdict -notin 'PASS', 'SKIP') { Refuse "submit needs a verdict: land.ps1 submit $Number PASS|SKIP" }
      $tf = @(Get-ChildItem (Join-Path $root 'tasks') -Filter "$Number-*.md")
      if ($tf.Count -eq 1) {
        $look = Get-TaskField (Get-Content $tf[0].FullName -Raw) 'Look at:'
        if ($look -and $look -notmatch '^none\b' -and -not $Evidence) { Refuse "task $Number has Look at: ($look); view the images and pass -Evidence 'what you saw'" }
      }
    }
    Assert-Idle
  }
  if (-not $env:ASTRORAY_DEPS_CACHE) { $env:ASTRORAY_DEPS_CACHE = Join-Path $mainCheckout 'third_party/_cache' }
  switch ($Mode) {
    'prepare' { Invoke-Prepare }
    'merge' {
      if ((Invoke-Merge) -eq 'RERUN') { exit 1 }
      if (-not $DryRun) { $null = Invoke-LandingCleanup -Main $mainCheckout -Task $script:Task }
    }
    'submit' {
      Invoke-Submit
      if (-not $DryRun) { $null = Invoke-LandingCleanup -Main $mainCheckout -Task $script:Task }
    }
  }
} catch {
  Undo-LandCommit
  Write-Host "FAIL land $Mode ${Number}: $($_.Exception.Message)"
  exit 1
}
