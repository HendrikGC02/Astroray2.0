# SPDX-License-Identifier: Apache-2.0
# Get-ArNsys: the Nsight Systems CLI (nsys.exe). Nsight Systems is installed beside the driver tools, not on
# PATH, and its folder name carries the version. $env:ASTRORAY_NSYS wins; else the newest nsys.exe under
# $env:ProgramFiles\NVIDIA Corporation\Nsight Systems*. Neither: it throws, naming both ways to set it.
# Ref: tasks/160; PR 94 (123's capture method).
function Get-ArNsys {
  if ($env:ASTRORAY_NSYS) {
    if (-not (Test-Path -LiteralPath $env:ASTRORAY_NSYS -PathType Leaf)) { throw "ASTRORAY_NSYS is set to '$($env:ASTRORAY_NSYS)', which is not a file" }
    return (Resolve-Path -LiteralPath $env:ASTRORAY_NSYS).Path
  }
  $found = @()
  foreach ($d in @(Get-ChildItem -Path $env:ProgramFiles -Directory -Filter 'NVIDIA Corporation' -ErrorAction SilentlyContinue)) {
    foreach ($ns in @(Get-ChildItem -Path $d.FullName -Directory -Filter 'Nsight Systems*' -ErrorAction SilentlyContinue)) {
      $found += @(Get-ChildItem -Path $ns.FullName -Recurse -Filter 'nsys.exe' -File -ErrorAction SilentlyContinue)
    }
  }
  if (-not $found.Count) {
    throw "nsys.exe not found: install Nsight Systems under '$env:ProgramFiles\NVIDIA Corporation\Nsight Systems*', or set ASTRORAY_NSYS to the full path of nsys.exe"
  }
  # Newest by the version in the folder name ('Nsight Systems 2024.6.2'), not by file time.
  $best = $found | Sort-Object { $v = [regex]::Match($_.FullName, 'Nsight Systems ([\d.]+)').Groups[1].Value; if ($v) { [version]$v } else { [version]'0.0' } } | Select-Object -Last 1
  return $best.FullName
}
