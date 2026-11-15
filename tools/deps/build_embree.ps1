# SPDX-License-Identifier: Apache-2.0
# tools/deps/build_embree.ps1
# Recorded dependency build (0001 D9.2, 0009 d6): builds our Embree archive
# (static library, no TBB, static CRT) once. NOT part of the normal build or any
# check tier. The lead publishes the zip as a release asset (task 007 step 4).
# Ref: RenderKit/embree README and CMakeLists.txt (Apache-2.0), build options.
$ErrorActionPreference = 'Stop'
$ver = '4.4.1'   # pinned in 0009 decision 2
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root
. (Join-Path $PSScriptRoot '../lib/lock.ps1')
$clock = [Diagnostics.Stopwatch]::StartNew()
$null = Enter-ArMachineLock
try {
  $cmakeBin = 'C:\Program Files\CMake\bin'
  if (-not (Get-Command cmake -ErrorAction SilentlyContinue) -and (Test-Path $cmakeBin)) {
    $env:PATH = "$cmakeBin;$env:PATH"
  }
  # Same MSVC environment as tools/build.ps1 (0009 d2: toolset 14.44).
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  $vsArgs = @('-latest', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath')
  $vs = & $vswhere @vsArgs -products Microsoft.VisualStudio.Product.BuildTools | Select-Object -First 1
  if (-not $vs) { $vs = & $vswhere @vsArgs -products '*' | Select-Object -First 1 }
  if (-not $vs) { throw 'no Visual Studio with the C++ x64 tools' }
  $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
  $verArg = if ($env:CI -eq 'true') { '' } else { '-vcvars_ver=14.44' }
  $envDump = & cmd.exe /c "`"$vcvars`" x64 $verArg >nul 2>&1 && set"
  if ($LASTEXITCODE -ne 0) { throw 'vcvarsall failed (toolset 14.44 installed?)' }
  foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
  }

  $cache = Join-Path $root 'third_party/_cache'
  $src = Join-Path $cache "src/embree-$ver"
  $bld = Join-Path $cache "build/embree-$ver"
  $stage = Join-Path $cache "stage/embree-$ver"
  $outDir = Join-Path $root 'out/deps'
  $log = Join-Path $outDir "embree-$ver-configure.log"
  New-Item -ItemType Directory -Force $outDir | Out-Null
  if (Test-Path $bld) { Remove-Item -Recurse -Force $bld }
  if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
  if (-not (Test-Path (Join-Path $src 'CMakeLists.txt'))) {
    if (Test-Path $src) { Remove-Item -Recurse -Force $src }
    New-Item -ItemType Directory -Force (Split-Path $src) | Out-Null
    & git clone --depth 1 --branch "v$ver" https://github.com/RenderKit/embree.git $src
    if ($LASTEXITCODE -ne 0) { throw 'git clone failed' }
  }

  # Geometry types and the ISA set stay at Embree's defaults (triangle, user,
  # curve, point; runtime ISA dispatch). RAY_MASK is needed by rtcSetGeometryMask.
  # Embree's cmake policy is below CMP0091, so CMAKE_MSVC_RUNTIME_LIBRARY is
  # ignored; its own EMBREE_STATIC_RUNTIME rewrites /MD to /MT (msvc_post.cmake).
  $cfg = @(
    '-S', $src, '-B', $bld, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$stage",
    '-DEMBREE_STATIC_LIB=ON', '-DEMBREE_TASKING_SYSTEM=INTERNAL',
    '-DEMBREE_ISPC_SUPPORT=OFF', '-DEMBREE_TUTORIALS=OFF',
    '-DEMBREE_TESTING_INTEGRATION=OFF', '-DEMBREE_RAY_MASK=ON',
    '-DEMBREE_STATIC_RUNTIME=ON'
  )
  # The summary line proves the option reached the cache.
  & cmake @cfg 2>&1 | Out-File -Encoding ascii $log
  if ($LASTEXITCODE -ne 0) { throw "configure failed (see $log)" }
  Add-Content -Path $log -Value (Select-String -Path (Join-Path $bld 'CMakeCache.txt') -Pattern '^EMBREE_(RAY_MASK|STATIC_LIB|TASKING_SYSTEM)' | ForEach-Object { $_.Line })
  Select-String -Path (Join-Path $bld 'CMakeCache.txt') -Pattern '^EMBREE_RAY_MASK' | ForEach-Object { $_.Line }
  & cmake --build $bld --target install 2>&1 | Out-File -Encoding ascii (Join-Path $outDir "embree-$ver-build.log")
  if ($LASTEXITCODE -ne 0) { throw 'build failed (see out/deps/embree-build.log)' }

  $lic = Join-Path $src 'LICENSE.txt'
  if (-not (Test-Path $lic)) { $lic = Join-Path $src 'LICENSE' }
  Copy-Item $lic (Join-Path $stage 'LICENSE')
  $zip = Join-Path $outDir "embree-$ver-astroray-win64-mt.zip"
  if (Test-Path $zip) { Remove-Item $zip }
  Compress-Archive -Path (Join-Path $stage 'include'), (Join-Path $stage 'lib'), (Join-Path $stage 'LICENSE') -DestinationPath $zip
  $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
  $bytes = (Get-Item $zip).Length
  "run time $([int]$clock.Elapsed.TotalSeconds) s"
  "embree-$ver-astroray-win64-mt.zip sha256=$hash bytes=$bytes"
} finally {
  Exit-ArMachineLock
}
