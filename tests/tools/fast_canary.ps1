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

# Per-case outcome in verbosity-2 output: one line, docstring on a second line, text the test printed in between.
$ut = @'
test_plain (m.T.test_plain) ... ok
test_doc (m.T.test_doc)
First line of the docstring ... ok
test_printy (m.T.test_printy)
some text the test printed
more text
test_printy done ... ok
test_bad (m.T.test_bad) ... FAIL
test_broken (m.T.test_broken)
Docstring of a broken test ... ERROR
test_second (m.T.test_second) ... ok
'@
function Out-Str($o) { "$($o.Ran)/$($o.Failed)" }
Check 'single-line ok' ((Out-Str (Get-UnittestOutcome $ut 'T.test_plain')) -eq 'True/False')
Check 'docstring two-line ok' ((Out-Str (Get-UnittestOutcome $ut 'T.test_doc')) -eq 'True/False')
Check 'interleaved print ok' ((Out-Str (Get-UnittestOutcome $ut 'T.test_printy')) -eq 'True/False')
Check 'FAIL is failed' ((Out-Str (Get-UnittestOutcome $ut 'T.test_bad')) -eq 'True/True')
Check 'docstring ERROR is failed' ((Out-Str (Get-UnittestOutcome $ut 'T.test_broken')) -eq 'True/True')
Check 'absent case did not run' ((Out-Str (Get-UnittestOutcome $ut 'T.test_missing')) -eq 'False/False')
Check 'an outcome never leaks into the next test' ((Out-Str (Get-UnittestOutcome "test_a (m.T.test_a)`ntest_b (m.T.test_b) ... ok" 'T.test_a')) -eq 'False/False')

# A crash or timeout marks every case of a file group errored; a failing script (exit 1) does not.
Check 'exit 0 and 1 are not crashes' ((Get-BlenderExitError 0) -eq '' -and (Get-BlenderExitError 1) -eq '')
Check 'timeout and crash codes error' ((Get-BlenderExitError -1) -match 'timed out' -and (Get-BlenderExitError 3) -match 'exited 3')
$grp = @([pscustomobject]@{ Name = 'T.test_plain' }, [pscustomobject]@{ Name = 'T.test_bad' })
$gr = @(Get-CanaryGroupResults $grp $ut (Get-BlenderExitError 3))
Check 'crash errors every case of the group' ($gr.Count -eq 2 -and @($gr | Where-Object { $_.Error -match 'crash' -and -not $_.Ran }).Count -eq 2)
$gr = @(Get-CanaryGroupResults $grp $ut '')
Check 'no error: each case from its own lines' ($gr[0].Ran -and -not $gr[0].Failed -and $gr[1].Failed -and -not $gr[0].Error)

# Baseline record: only a clean tree with the same tree hash and the same items may skip the baseline.
$rec = [pscustomobject]@{ tree = 'abc'; key = 'k1' }
Check 'matching record on a clean tree is used' (Test-CanaryBaselineRecord $rec 'abc' 'k1' $true)
Check 'a dirty tree never uses the record' (-not (Test-CanaryBaselineRecord $rec 'abc' 'k1' $false))
Check 'a different item key never uses the record' (-not (Test-CanaryBaselineRecord $rec 'abc' 'k2' $true))
Check 'a different tree never uses the record' (-not (Test-CanaryBaselineRecord $rec 'abd' 'k1' $true))
Check 'no record, no skip' (-not (Test-CanaryBaselineRecord $null 'abc' 'k1' $true))

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
