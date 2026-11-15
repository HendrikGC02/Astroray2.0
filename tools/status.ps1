# SPDX-License-Identifier: Apache-2.0
# tools/status.ps1: live state, at most 20 lines (0009 d29): task branches, open PRs with their
# `ci` and `astroray/gate` states, worktrees, last out/check/*.json summaries. Reads only.
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path ((& git rev-parse --show-toplevel).Trim())).Path
Set-Location $root
$lines = New-Object System.Collections.Generic.List[string]

$br = @(& git for-each-ref --format='%(refname:short)' 'refs/heads/task/*' | ForEach-Object { "$_" })
$lines.Add("task branches ($($br.Count)): " + (($br | ForEach-Object { $_ -replace '^task/', '' }) -join ' '))

# gh failures are shown, never hidden.
$prs = $null
try {
  $j = & gh pr list --state open --json number,headRefName,headRefOid 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0) { throw ($j -join ' ') }
  $prs = @(($j -join "`n") | ConvertFrom-Json)
} catch {
  $lines.Add("PRs: gh unavailable: $($_.Exception.Message)")
}
if ($null -ne $prs) {
  if ($prs.Count -eq 0) { $lines.Add('PRs: none open') }
  foreach ($p in $prs) {
    $ci = 'none'; $gate = 'none'
    try {
      $runs = (& gh api "repos/{owner}/{repo}/commits/$($p.headRefOid)/check-runs" | ConvertFrom-Json).check_runs
      $r = $runs | Where-Object { $_.name -eq 'ci' } | Select-Object -First 1
      if ($r) { $ci = if ($r.conclusion) { $r.conclusion } else { $r.status } }
      $st = @(& gh api "repos/{owner}/{repo}/commits/$($p.headRefOid)/statuses" | ConvertFrom-Json)
      $g = $st | Where-Object { $_.context -eq 'astroray/gate' } | Select-Object -First 1
      if ($g) { $gate = "$($g.state) [$(($g.description -split ':')[0])]" }
    } catch { $ci = 'error'; $gate = 'error' }
    $lines.Add("PR #$($p.number) $($p.headRefName) $($p.headRefOid.Substring(0, 7)) ci=$ci gate=$gate")
  }
}

$wt = @(& git worktree list --porcelain | Where-Object { $_ -like 'branch *' } | ForEach-Object { $_ -replace '^branch refs/heads/', '' })
$lines.Add("worktrees ($($wt.Count)): " + ($wt -join ' '))

foreach ($f in Get-ChildItem (Join-Path $root 'out/check') -Filter *.json -ErrorAction SilentlyContinue) {
  try {
    $c = Get-Content $f.FullName -Raw | ConvertFrom-Json
    $age = [int]((Get-Date) - $f.LastWriteTime).TotalMinutes
    $lines.Add("check $($c.tier): $(if ($c.pass) { 'PASS' } else { 'FAIL' }) $($c.seconds) s sha $($c.sha) ${age} min ago")
  } catch { $lines.Add("check $($f.BaseName): unreadable json") }
}

if ($lines.Count -gt 20) {
  $n = $lines.Count - 19
  $lines = @($lines[0..18]) + @("... $n more lines omitted")
}
$lines | ForEach-Object { Write-Host $(if ($_.Length -gt 200) { $_.Substring(0, 197) + '...' } else { $_ }) }
