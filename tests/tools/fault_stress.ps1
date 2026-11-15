# SPDX-License-Identifier: Apache-2.0
# Stress for the fault tests under concurrent load. Not part of any tier (minutes).
# Usage: pwsh tests/tools/fault_stress.ps1 -Runs 40 -Parallel 4
param([int]$Runs = 40, [int]$Parallel = 4, [int]$TimeoutSec = 20)
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$exe = Join-Path $root 'out/bin/astroray_tests_cpu.exe'
if (-not (Test-Path $exe)) { Write-Error "missing $exe (run tools/build.ps1)"; exit 2 }
$jobs = 1..$Parallel | ForEach-Object {
  Start-Job -ArgumentList $exe, $Runs, $TimeoutSec -ScriptBlock {
    param($exe, $runs, $timeout)
    $hang = 0; $fail = 0
    for ($i = 0; $i -lt $runs; $i++) {
      $p = Start-Process -FilePath $exe -ArgumentList '--tier=gate', '-tc=fault*' -PassThru -NoNewWindow -RedirectStandardOutput 'NUL'
      if (-not $p.WaitForExit($timeout * 1000)) { $hang++; & taskkill /F /T /PID $p.Id | Out-Null }
      elseif ($p.ExitCode -ne 0) { $fail++ }
    }
    [pscustomobject]@{ Hangs = $hang; Failures = $fail }
  }
}
$res = $jobs | Wait-Job | Receive-Job
$jobs | Remove-Job
$h = ($res | Measure-Object Hangs -Sum).Sum
$f = ($res | Measure-Object Failures -Sum).Sum
"runs=$($Runs * $Parallel) hangs=$h failures=$f"
if ($h -or $f) { exit 1 }
