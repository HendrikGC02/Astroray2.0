# SPDX-License-Identifier: Apache-2.0
# Python for tools (0009 d2, ruling 32). Dot-source this file.
function Get-ArPython {
  # Returns the interpreter path; callers pass -I first.
  if ($env:CI -eq 'true') { return 'python' }
  $py = Join-Path $env:ASTRORAY_BLENDER '5.3\python\bin\python.exe'
  if (-not $env:ASTRORAY_BLENDER -or -not (Test-Path $py)) {
    throw "Blender's Python not found at '$py'; set ASTRORAY_BLENDER to the Blender 5.3 install folder"
  }
  return $py
}
