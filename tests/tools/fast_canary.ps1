# SPDX-License-Identifier: Apache-2.0
# CPU-only checks of tools/lib/canary.ps1: the Canary: line parse, the hunk splitter and the patch refusals.
# No build, no git history needed; run by tests/tools/run_fast.ps1 (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $root 'tools/lib/canary.ps1')
$bad = 0
function Check([string]$what, [bool]$ok) { if (-not $ok) { Write-Host "FAIL fast_canary: $what"; $script:bad++ } }
function Key($items) { (@($items) | ForEach-Object { "$($_.Kind)|$($_.Target)|$($_.Name)" }) -join ' ; ' }

# Items: executable and names, .py and names, backticked command with parenthesised text, unnamed .py.
Check 'exe plus names' ((Key (Get-CanaryItems 'x must turn red: astroray_tests_cpu "a/b", "c d".')) -eq 'exe|astroray_tests_cpu|a/b ; exe|astroray_tests_cpu|c d')
Check '.py plus names' ((Key (Get-CanaryItems 'must turn red: tests/x/test_y.py "T.a", "T.b"')) -eq 'py|tests/x/test_y.py|T.a ; py|tests/x/test_y.py|T.b')
$mixed = 'must turn red: `pwsh -NoProfile -File t.ps1 "q"` (flips GOOD, which, also "breaks" cases), astroray_tests_cpu "gate/a" and "b/c".'
Check 'backticked command and parenthesised text' ((Key (Get-CanaryItems $mixed)) -eq 'cmd|pwsh -NoProfile -File t.ps1 "q"| ; exe|astroray_tests_cpu|gate/a ; exe|astroray_tests_cpu|b/c')
Check 'unnamed .py has an empty name' ((Key (Get-CanaryItems 'must turn red: tests/x/test_y.py')) -eq 'py|tests/x/test_y.py|')
Check 'no marker, no items' (@(Get-CanaryItems 'none').Count -eq 0)
$threw = $false
try { $null = Get-CanaryItems 'must turn red: "orphan"' } catch { $threw = $true }
Check 'a name with no file throws' $threw

# Hunk splitter: two files, three hunks.
$patch = @'
diff --git a/src/a.txt b/src/a.txt
index 1..2 100644
--- a/src/a.txt
+++ b/src/a.txt
@@ -1,3 +1,3 @@
 one
-two
+TWO
 three
@@ -20,3 +20,3 @@
 x
-y
+Y
 z
diff --git a/src/b.txt b/src/b.txt
index 3..4 100644
--- a/src/b.txt
+++ b/src/b.txt
@@ -7,2 +7,2 @@
-p
+P
 q
'@.Replace("`r`n", "`n") + "`n"
$h = @(Split-CanaryPatch $patch)
Check 'three hunks' ($h.Count -eq 3)
Check 'hunk labels' ((($h | ForEach-Object { $_.Where }) -join ' ') -eq 'src/a.txt:1 src/a.txt:20 src/b.txt:7')
Check 'each hunk has one @@ and its file header' (@($h | Where-Object { ([regex]::Matches($_.Text, '(?m)^@@ ')).Count -eq 1 -and $_.Text.StartsWith('diff --git ') }).Count -eq 3)
Check 'second hunk keeps its own lines' ($h[1].Text.Contains('+Y') -and -not $h[1].Text.Contains('+TWO') -and $h[1].Text.Contains('+++ b/src/a.txt'))

# Refusals: a patch that changes tasks/, and a patch with CR bytes (both before git is asked).
$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_canary_$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
  $self = Join-Path $tmp 'self.patch'
  [IO.File]::WriteAllText($self, "diff --git a/tasks/001-x.md b/tasks/001-x.md`n--- a/tasks/001-x.md`n+++ b/tasks/001-x.md`n@@ -1 +1 @@`n-a`n+b`n")
  $p = @(Get-CanaryPatchProblems $self $root '001')
  Check 'tasks/ refusal' ($p.Count -eq 1 -and $p[0] -match 'under tasks/' -and $p[0] -match 'git diff --output')
  $cr = Join-Path $tmp 'cr.patch'
  [IO.File]::WriteAllText($cr, "diff --git a/src/a.txt b/src/a.txt`r`n--- a/src/a.txt`r`n+++ b/src/a.txt`r`n@@ -1 +1 @@`r`n-a`r`n+b`r`n")
  $p = @(Get-CanaryPatchProblems $cr $root '001')
  Check 'CR refusal' ($p.Count -eq 1 -and $p[0] -match 'CR bytes' -and $p[0] -match 'never with >')
} finally { Remove-Item -Recurse -Force $tmp }

if ($bad) { Write-Host "FAIL fast_canary: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_canary'
