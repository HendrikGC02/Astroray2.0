# SPDX-License-Identifier: Apache-2.0
# Runs named cases of one test file or executable and judges each from its output. The one runner behind
# tools/test.ps1 and the canary (lib/canary.ps1). Dot-source this file. Locks and builds are the caller's;
# a Blender start takes the machine lock itself (Invoke-ArBlender).
. (Join-Path $PSScriptRoot 'blender.ps1')
. (Join-Path $PSScriptRoot 'python.ps1')

function New-ArTestResult([string]$case, [bool]$ran, [bool]$failed, [string]$err, [string]$output) {
  [pscustomobject]@{ Case = $case; Ran = $ran; Failed = $failed; Error = $err; Output = $output }
}

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
# Invoke-ArBlender's timeout, any other code is a crash.
function Get-BlenderExitError([int]$code) {
  if ($code -eq -1) { return 'Blender timed out' }
  if ($code -ne 0 -and $code -ne 1) { return "Blender exited $code (crash?)" }
  return ''
}

# One result per name from one unittest run's text; a non-empty $err marks every case errored.
function Get-ArCaseResults([string[]]$names, [string]$txt, [string]$err) {
  return @($names | ForEach-Object {
    if ($err) { New-ArTestResult $_ $false $false $err $txt }
    else { $o = Get-UnittestOutcome $txt $_; New-ArTestResult $_ $o.Ran $o.Failed '' $txt }
  })
}

function Read-ArBlenderLog($r) {
  $txt = ''
  foreach ($f in $r.Log, "$($r.Log).err") { if (Test-Path $f) { $txt += (Get-Content $f -Raw) } }
  return $txt
}

function Invoke-ArTestCase {
  # Returns one {Case; Ran; Failed; Error; Output} per -Case, in order. Ran: the case exists and ran.
  # Failed: a real test failure. Error: the case could not be judged (missing binary, crash, timeout).
  # exe: out/bin/<Target>.exe (or the .cmd a fixture stands in with), doctest -tc per case. py: a
  # tests/abi file, all cases in one unittest start. blender: a tests/blender file, all cases in one
  # Blender start; a gui_* script reads a single -k, so one windowed start per case.
  param(
    [Parameter(Mandatory)][ValidateSet('exe', 'py', 'blender')][string]$Kind,
    [Parameter(Mandatory)][string]$Target,
    [Parameter(Mandatory)][string[]]$Case,
    [ValidateSet('fast', 'gate', 'full', 'all')][string]$Tier = 'all',
    [string]$Root = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
  )
  # Test files are named from the root and the fixture stand-in reads files relative to it.
  Push-Location $Root
  try {
    if ($Kind -eq 'exe') {
      $p = Join-Path $Root "out/bin/$Target.exe"
      if (-not (Test-Path $p)) { $p = Join-Path $Root "out/bin/$Target.cmd" }
      if (-not (Test-Path $p)) { return @($Case | ForEach-Object { New-ArTestResult $_ $false $false "missing out/bin/$Target.exe after the build" '' }) }
      return @(foreach ($c in $Case) {
        $out = & $p "--tier=$Tier" ('-tc=' + ($c -replace ',', '\,')) 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $txt = $out -join "`n"
        $m = [regex]::Match($txt, 'test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed')
        if (-not $m.Success) { New-ArTestResult $c $false $false "$Target printed no doctest summary (crash?)" $txt }
        elseif ($code -ne 0 -and [int]$m.Groups[3].Value -eq 0) { New-ArTestResult $c $true $false "$Target exited $code with no failed case" $txt }
        else { New-ArTestResult $c ([int]$m.Groups[1].Value -gt 0) ([int]$m.Groups[3].Value -gt 0) '' $txt }
      })
    }
    $rel = $Target -replace '\\', '/'
    if (-not (Test-Path $rel)) { return @($Case | ForEach-Object { New-ArTestResult $_ $false $false "test file '$rel' does not exist" '' }) }
    if ($Kind -eq 'py') {
      if ($rel -notlike 'tests/abi/*') { throw "Invoke-ArTestCase -Kind py runs tests/abi files, not '$rel'" }
      $kargs = @($Case | ForEach-Object { '-k'; $_ })
      $txt = (& (Get-ArPython) -I -m unittest discover -s (Split-Path $rel) -p (Split-Path $rel -Leaf) -v @kargs 2>&1 | ForEach-Object { "$_" }) -join "`n"
      $err = if ($txt -match '_FailedTest|ModuleNotFoundError|ImportError|SyntaxError') { 'test module failed to import' } else { '' }
      return Get-ArCaseResults $Case $txt $err
    }
    if ($rel -notlike 'tests/blender/*') { throw "Invoke-ArTestCase -Kind blender runs tests/blender files, not '$rel'" }
    if ((Split-Path $rel -Leaf) -like 'gui_*') {
      return @(foreach ($c in $Case) {
        $r = Invoke-ArBlender -Script $rel -Args @('-k', $c) -Gui 6>$null
        $txt = Read-ArBlenderLog $r
        $be = Get-BlenderExitError $r.ExitCode
        if ($be) { New-ArTestResult $c $false $false $be $txt }
        else { New-ArTestResult $c $txt.Contains($c) ($r.ExitCode -ne 0) '' $txt }
      })
    }
    $r = Invoke-ArBlender -Script $rel -Args @($Case | ForEach-Object { '-k'; $_ }) 6>$null
    $txt = Read-ArBlenderLog $r
    return Get-ArCaseResults $Case $txt (Get-BlenderExitError $r.ExitCode)
  } finally { Pop-Location }
}

# PASS only when every case ran and none failed or errored; zero matching cases is a FAIL.
function Get-ArTestVerdict($results) {
  foreach ($r in @($results)) {
    if ($r.Error) { return [pscustomobject]@{ Pass = $false; Reason = "$($r.Case): $($r.Error)" } }
    if (-not $r.Ran) { return [pscustomobject]@{ Pass = $false; Reason = "$($r.Case): no matching test case" } }
    if ($r.Failed) { return [pscustomobject]@{ Pass = $false; Reason = "$($r.Case): failed" } }
  }
  return [pscustomobject]@{ Pass = $true; Reason = '' }
}

# The lines worth showing from one run, at most $max: the doctest or unittest summary, then the first
# failing lines.
function Select-ArTestLines([string]$txt, [int]$max) {
  $lines = @($txt -split "`r?`n")
  $sum = @($lines | Where-Object { $_ -match 'test cases:\s*\d+|^Ran \d+ test|^(OK|FAILED)\b' })
  $bad = @($lines | Where-Object { $_ -match '\b(ERROR|FAIL|FAILED|Error|Traceback|error:)\b|^\s*CHECK|REQUIRE' -and $sum -notcontains $_ })
  return @(@($sum) + @($bad) | Select-Object -First $max)
}
