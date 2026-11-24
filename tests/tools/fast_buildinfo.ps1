# SPDX-License-Identifier: Apache-2.0
# The build identity rule (cmake/buildinfo.cmake, astroray_add_buildinfo_rule) and build.ps1's no-work-left check
# (Test-ArNoWorkLeft, loaded from the real script) on a throwaway git repository with a compiler-free Ninja
# project: an index stat refresh alone (git status after a file's mtime moved) makes `ninja -n` list build_info
# but leaves nothing to do for Test-ArNoWorkLeft, and the real build after it rebuilds nothing; a content change of
# a tracked source, a changed dirty flag and a commit still report work and regenerate the identity; an unknown
# stamp fails loudly. Run by tests/tools/run_fast.ps1 (gate step `tools tests`).
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$bad = 0
function Check([string]$what, [bool]$ok, $out = @()) {
  if (-not $ok) { Write-Host "FAIL fast_buildinfo: $what"; @($out) | Select-Object -Last 6 | ForEach-Object { Write-Host "  $_" }; $script:bad++ }
}

# cmake and ninja as build.ps1 finds them (PATH, else the Visual Studio copy), so a bare shell works too.
function Find-Tool([string]$name, [string[]]$extra) {
  $c = Get-Command $name -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($p in $extra) { if (Test-Path $p) { return $p } }
  return $null
}
$vsRoots = @()
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) { $vsRoots = @(& $vswhere -latest -products '*' -property installationPath) }
$cmake = Find-Tool 'cmake' @('C:\Program Files\CMake\bin\cmake.exe')
$ninja = Find-Tool 'ninja' @($vsRoots | ForEach-Object { Join-Path $_ 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' })
if (-not $cmake -or -not $ninja) { Write-Host "FAIL fast_buildinfo: cmake ($cmake) or ninja ($ninja) not found"; exit 1 }
$env:PATH = "$(Split-Path $cmake);$(Split-Path $ninja);$env:PATH"

# The check under test, taken from the real build.ps1 rather than copied.
$ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'tools/build.ps1'), [ref]$null, [ref]$null)
$fn = $ast.Find({ param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Test-ArNoWorkLeft' }, $true)
if (-not $fn) { Write-Host 'FAIL fast_buildinfo: tools/build.ps1 has no Test-ArNoWorkLeft'; exit 1 }
. ([scriptblock]::Create($fn.Extent.Text))

$tmp = Join-Path ([IO.Path]::GetTempPath()) "fast_buildinfo_$PID"
$repo = Join-Path $tmp 'fix'
$bld = Join-Path $repo 'build'
function G { $o = @(& git -C $repo -c user.name=t -c user.email=t@t @args 2>&1 | ForEach-Object { "$_" }); if ($LASTEXITCODE -ne 0) { throw "git $args failed: $o" }; return $o }
function Ticks([string]$p) { return (Get-Item $p).LastWriteTimeUtc.Ticks }
function N { return @(& ninja -C $bld @args 2>&1 | ForEach-Object { "$_" }) }
try {
  New-Item -ItemType Directory -Force $repo | Out-Null
  $null = & git init -q -b main $repo
  Set-Content -Path (Join-Path $repo 'src.txt') -Value 'one'
  Set-Content -Path (Join-Path $repo 'touched.txt') -Value 'two'
  # A consumer of build_info.cpp (stands for the object file) and of a tracked source; no compiler needed.
  $cm = @"
cmake_minimum_required(VERSION 3.31)
project(fix NONE)
include($($root -replace '\\', '/')/cmake/buildinfo.cmake)
astroray_add_buildinfo_rule(CPP `${CMAKE_BINARY_DIR}/gen "1.0" "Test")
add_custom_command(OUTPUT `${CMAKE_BINARY_DIR}/consumer.txt COMMAND `${CMAKE_COMMAND} -E copy `${CPP} `${CMAKE_BINARY_DIR}/consumer.txt
  DEPENDS `${CPP} VERBATIM)
add_custom_command(OUTPUT `${CMAKE_BINARY_DIR}/src_copy.txt COMMAND `${CMAKE_COMMAND} -E copy `${CMAKE_SOURCE_DIR}/src.txt `${CMAKE_BINARY_DIR}/src_copy.txt
  DEPENDS `${CMAKE_SOURCE_DIR}/src.txt VERBATIM)
add_custom_target(all_fix ALL DEPENDS `${CMAKE_BINARY_DIR}/consumer.txt `${CMAKE_BINARY_DIR}/src_copy.txt)
add_dependencies(all_fix astroray_buildinfo_gen)
"@
  Set-Content -Path (Join-Path $repo 'CMakeLists.txt') -Value $cm
  $null = G add -A
  $null = G commit -q -m first
  $c = @(& cmake -S $repo -B $bld -G Ninja 2>&1 | ForEach-Object { "$_" })
  Check 'configure the fixture' ($LASTEXITCODE -eq 0) $c
  $b = N all_fix
  Check 'first build' ($LASTEXITCODE -eq 0) $b
  $cpp = Join-Path $bld 'gen/build_info.cpp'
  $cons = Join-Path $bld 'consumer.txt'
  Check 'build_info.cpp is generated with this commit and a clean tree' ((Get-Content -Raw $cpp) -match ('"1.0", "' + @(G rev-parse HEAD)[0] + '", 0,')) @(Get-Content $cpp)
  $r = Test-ArNoWorkLeft $bld @('all_fix')
  Check 'after a build: no work left' $r.Ok $r.Output

  # An index stat refresh: a tracked file's mtime moves, then git status rewrites the index.
  (Get-Item (Join-Path $repo 'touched.txt')).LastWriteTime = (Get-Date).AddSeconds(5)
  $null = G status --short
  $dry = N -n all_fix
  Check 'precondition: ninja -n alone lists the generator after an index refresh (the race)' (@($dry | Where-Object { $_ -match 'build_info' }).Count -gt 0) $dry
  $cppT = Ticks $cpp; $consT = Ticks $cons
  $r = Test-ArNoWorkLeft $bld @('all_fix')
  Check 'index refresh alone: no work left' $r.Ok $r.Output
  Check 'index refresh alone: build_info.cpp not rewritten' ((Ticks $cpp) -eq $cppT)
  $null = N all_fix
  Check 'index refresh alone: the real build rebuilds nothing after it' ((Ticks $cpp) -eq $cppT -and (Ticks $cons) -eq $consT)

  # A content change of a tracked source is work, whatever the settle step does.
  # The dirty flag follows the index, which only a git command that refreshes it moves (here git status).
  Set-Content -Path (Join-Path $repo 'src.txt') -Value 'changed'
  $null = G status --short
  $r =Test-ArNoWorkLeft $bld @('all_fix')
  Check 'a changed tracked source: work left' (-not $r.Ok) $r.Output
  Check 'a changed tracked source: the work is named' (($r.Output -join "`n") -match 'Generating src_copy') $r.Output
  Check 'a changed tracked source: the dirty flag moved, so the identity was regenerated' ((Get-Content -Raw $cpp) -match '", 1,') @(Get-Content $cpp)
  $null = N all_fix
  $r = Test-ArNoWorkLeft $bld @('all_fix')
  Check 'after rebuilding that source: no work left' $r.Ok $r.Output

  # A commit moves the sha and clears the dirty flag: regenerated, consumer rebuilt, then nothing left.
  $null = G commit -q -a -m second
  $head = @(G rev-parse HEAD)[0]
  $consT = Ticks $cons
  $null = N all_fix
  Check 'a commit regenerates build_info.cpp with the new sha' ((Get-Content -Raw $cpp) -match ('"1.0", "' + $head + '", 0,')) @(Get-Content $cpp)
  Check 'a commit rebuilds the consumer of build_info.cpp' ((Ticks $cons) -gt $consT)
  $r = Test-ArNoWorkLeft $bld @('all_fix')
  Check 'after a commit and its build: no work left' $r.Ok $r.Output

  # Failing loudly: a stamp that is not a target of this tree is a failure with ninja's message.
  $r = Test-ArNoWorkLeft $bld @('all_fix') 'gen/no_such.stamp'
  Check 'an unknown stamp fails and says why' (-not $r.Ok -and "$($r.First)" -match 'unknown target') @($r.First)
} finally {
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($bad) { Write-Host "FAIL fast_buildinfo: $bad check(s)"; exit 1 }
Write-Host 'PASS fast_buildinfo'
exit 0
