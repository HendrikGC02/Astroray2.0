# SPDX-License-Identifier: Apache-2.0
# tools/deps/build_oidn.ps1
# Recorded dependency build (0001 D9.2, D9.4; 0009 d6): builds our OIDN with renamed
# modules (astroray_oidn*), the static CRT and a static oneTBB inside the CPU device
# module, plus the CUDA device. NOT part of the normal build or any check tier. The lead
# publishes the zip as a release asset (task 098). Prints the dumpbin and smoke results.
# Ref: RenderKit/oidn v2.5.0 README (prerequisites) and CMake options OIDN_LIBRARY_NAME,
# OIDN_DEVICE_CPU, OIDN_DEVICE_CUDA, OIDN_APPS; uxlfoundation/oneTBB build options
# (BUILD_SHARED_LIBS, TBB_TEST); ISPC 1.30.0 (OIDN README, CPU device prerequisite).
$ErrorActionPreference = 'Stop'
$ver = '2.5.0'       # pinned in 0009 decision 2
$tbbVer = '2022.3.0' # OIDN only says "2017 or newer"; this is the oneTBB Blender 5.3 ships
$ispcVer = '1.30.0'
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
  $cuda = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8'
  if (-not (Test-Path "$cuda\bin\nvcc.exe")) { throw 'CUDA 12.8 not found' }
  $env:PATH = "$cuda\bin;$env:PATH"

  $cache = Join-Path $root 'third_party/_cache'
  $outDir = Join-Path $root 'out/deps'
  $work = Join-Path $cache 'build/oidn-work'
  $srcO = Join-Path $cache "src/oidn-$ver"
  $srcT = Join-Path $cache "src/oneTBB-$tbbVer"
  $ispcDir = Join-Path $cache "tools/ispc-v$ispcVer-windows"
  $stageT = Join-Path $work 'tbb-stage'
  $stage = Join-Path $work 'stage'
  New-Item -ItemType Directory -Force $outDir, (Join-Path $cache 'tools'), (Join-Path $cache 'src') | Out-Null
  if (Test-Path $work) { Remove-Item -Recurse -Force $work }
  New-Item -ItemType Directory -Force $work | Out-Null

  function Invoke-Step($log, $exe, $a) {
    & $exe @a 2>&1 | Out-File -Encoding ascii (Join-Path $outDir $log)
    if ($LASTEXITCODE -ne 0) { throw "$exe failed (see out/deps/$log)" }
  }

  if (-not (Test-Path (Join-Path $srcO 'CMakeLists.txt'))) {
    if (Test-Path $srcO) { Remove-Item -Recurse -Force $srcO }
    & git clone --depth 1 --branch "v$ver" --recurse-submodules --shallow-submodules https://github.com/RenderKit/oidn.git $srcO
    if ($LASTEXITCODE -ne 0) { throw 'oidn clone failed' }
  }
  if (-not (Test-Path (Join-Path $srcT 'CMakeLists.txt'))) {
    if (Test-Path $srcT) { Remove-Item -Recurse -Force $srcT }
    & git clone --depth 1 --branch "v$tbbVer" https://github.com/uxlfoundation/oneTBB.git $srcT
    if ($LASTEXITCODE -ne 0) { throw 'oneTBB clone failed' }
  }
  if (-not (Test-Path (Join-Path $ispcDir 'bin/ispc.exe'))) {
    $zipI = Join-Path $cache "tools/ispc-v$ispcVer-windows.zip"
    Invoke-WebRequest "https://github.com/ispc/ispc/releases/download/v$ispcVer/ispc-v$ispcVer-windows.zip" -OutFile $zipI
    Expand-Archive $zipI -DestinationPath (Join-Path $cache 'tools') -Force
    if (-not (Test-Path (Join-Path $ispcDir 'bin/ispc.exe'))) { throw "ispc.exe not at $ispcDir/bin after extract" }
  }

  # Old upstream cmake policy versions ignore CMAKE_MSVC_RUNTIME_LIBRARY (lesson of
  # the Embree build); CMP0091=NEW makes it effective, dumpbin /directives proves it.
  # OIDN builds its CUDA device as a separate sub-project that forwards only the
  # toolchain file, so the static CRT is set there (and reaches nvcc's host compiler).
  $tc = Join-Path $work 'static_crt.cmake'
  @('cmake_policy(SET CMP0091 NEW)', 'set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)') | Set-Content -Encoding ascii $tc
  $crt = @('-DCMAKE_POLICY_DEFAULT_CMP0091=NEW', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', "-DCMAKE_TOOLCHAIN_FILE=$tc")
  $bldT = Join-Path $work 'tbb'
  Invoke-Step 'oidn-tbb-configure.log' cmake (@('-S', $srcT, '-B', $bldT, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$stageT", '-DBUILD_SHARED_LIBS=OFF', '-DTBB_TEST=OFF', '-DTBB_STRICT=OFF',
    '-DTBB_EXAMPLES=OFF', '-DTBBMALLOC_BUILD=OFF') + $crt)
  Invoke-Step 'oidn-tbb-build.log' cmake @('--build', $bldT, '--target', 'install')

  $bld = Join-Path $work 'oidn'
  Invoke-Step 'oidn-configure.log' cmake (@('-S', $srcO, '-B', $bld, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_INSTALL_PREFIX=$stage", '-DOIDN_LIBRARY_NAME=astroray_oidn',
    '-DOIDN_DEVICE_CPU=ON', '-DOIDN_DEVICE_CUDA=ON', '-DOIDN_APPS=OFF',
    "-DISPC_EXECUTABLE=$ispcDir/bin/ispc.exe", "-DTBB_ROOT=$stageT", "-DTBB_DIR=$stageT/lib/cmake/TBB",
    "-DCMAKE_CUDA_COMPILER=$cuda/bin/nvcc.exe", '-DCMAKE_CUDA_RUNTIME_LIBRARY=Static') + $crt)
  Invoke-Step 'oidn-build.log' cmake @('--build', $bld, '--target', 'install')

  # Check 1: dependents of every produced DLL.
  $bin = Join-Path $stage 'bin'
  $dlls = Get-ChildItem $bin -Filter *.dll
  '== dumpbin /dependents'
  $bad = @()
  foreach ($d in $dlls) {
    "-- $($d.Name)"
    $deps = & dumpbin /dependents $d.FullName | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
    $deps
    foreach ($x in $deps) {
      if ($x -match '^(tbb|vcruntime|msvcp|vcomp|concrt|cudart)') { $bad += "$($d.Name) -> $x" }
    }
  }
  '== dumpbin /directives (static CRT: expect 0 dynamic-CRT directives)'
  foreach ($l in (Get-ChildItem (Join-Path $stage 'lib') -Filter *.lib)) {
    $n = (& dumpbin /directives $l.FullName | Select-String -CaseSensitive:$false 'DEFAULTLIB:"?(MSVCRT|MSVCPRT)' | Measure-Object).Count
    "$($l.Name): dynamic-CRT directives = $n"
  }
  if ($bad.Count) { "FORBIDDEN DEPENDENTS: $($bad -join '; ')" }

  # Check 2: smoke program, not committed. Denoises 64x64 on each device.
  $smoke = Join-Path $work 'smoke'
  New-Item -ItemType Directory -Force $smoke | Out-Null
  @'
#include <OpenImageDenoise/oidn.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
static int run(OIDNDeviceType t, const char* name) {
  OIDNDevice dev = oidnNewDevice(t);
  oidnCommitDevice(dev);
  const char* msg = nullptr;
  if (oidnGetDeviceError(dev, &msg) != OIDN_ERROR_NONE) { printf("%s: device error: %s\n", name, msg); return 1; }
  const int n = 64 * 64 * 3;
  std::vector<float> in(n), out(n, -1.f);
  srand(1);
  for (int i = 0; i < n; i++) in[i] = 0.5f + (rand() / (float)RAND_MAX - 0.5f);
  OIDNBuffer bi = oidnNewBuffer(dev, n * 4), bo = oidnNewBuffer(dev, n * 4);
  oidnWriteBuffer(bi, 0, n * 4, in.data());
  OIDNFilter f = oidnNewFilter(dev, "RT");
  oidnSetFilterImage(f, "color", bi, OIDN_FORMAT_FLOAT3, 64, 64, 0, 0, 0);
  oidnSetFilterImage(f, "output", bo, OIDN_FORMAT_FLOAT3, 64, 64, 0, 0, 0);
  oidnCommitFilter(f);
  oidnExecuteFilter(f);
  oidnSyncDevice(dev);
  oidnReadBuffer(bo, 0, n * 4, out.data());
  if (oidnGetDeviceError(dev, &msg) != OIDN_ERROR_NONE) { printf("%s: exec error: %s\n", name, msg); return 1; }
  double vi = 0, vo = 0, mi = 0, mo = 0;
  for (int i = 0; i < n; i++) { mi += in[i]; mo += out[i]; }
  mi /= n; mo /= n;
  for (int i = 0; i < n; i++) { vi += (in[i]-mi)*(in[i]-mi); vo += (out[i]-mo)*(out[i]-mo); if (!std::isfinite(out[i])) { printf("%s: non-finite\n", name); return 1; } }
  printf("%s: variance in=%g out=%g\n", name, vi / n, vo / n);
  return vo < vi ? 0 : 1;
}
int main() { int r = run(OIDN_DEVICE_TYPE_CPU, "CPU"); r |= run(OIDN_DEVICE_TYPE_CUDA, "CUDA"); printf(r ? "SMOKE FAIL\n" : "SMOKE PASS\n"); return r; }
'@ | Set-Content -Encoding ascii (Join-Path $smoke 'smoke.cpp')
  Copy-Item (Join-Path $bin '*.dll') $smoke
  Push-Location $smoke
  & cl /nologo /MT /EHsc /O2 "/I$stage/include" smoke.cpp /link "$stage/lib/astroray_oidn.lib" | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'smoke compile failed' }
  '== smoke'
  & .\smoke.exe
  $smokeRc = $LASTEXITCODE
  Pop-Location
  "smoke exit code $smokeRc"

  # Package: DLLs, import lib, headers, licences.
  Copy-Item (Join-Path $srcO 'LICENSE.txt') (Join-Path $stage 'LICENSE-oidn.txt')
  Copy-Item (Join-Path $srcO 'third-party-programs-oneTBB.txt') $stage -ErrorAction SilentlyContinue
  Copy-Item (Join-Path $srcT 'LICENSE.txt') (Join-Path $stage 'LICENSE-oneTBB.txt')
  $zip = Join-Path $outDir "oidn-$ver-astroray-win64-mt.zip"
  if (Test-Path $zip) { Remove-Item $zip }
  $items = @('bin', 'lib', 'include') | ForEach-Object { Join-Path $stage $_ }
  $items += Get-ChildItem $stage -File | ForEach-Object { $_.FullName }
  Compress-Archive -Path $items -DestinationPath $zip
  $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
  $bytes = (Get-Item $zip).Length
  "run time $([int]$clock.Elapsed.TotalSeconds) s"
  "oidn-$ver-astroray-win64-mt.zip sha256=$hash bytes=$bytes"
} finally {
  Exit-ArMachineLock
}
