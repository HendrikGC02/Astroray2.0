# SPDX-License-Identifier: Apache-2.0
# Python for tools (0009 d2, ruling 32). Dot-source this file.
function Get-ArBlenderRoot {
  # Agent and CI shells may not inherit the user-level variable, so it is read as a fallback.
  $b = if ($env:ASTRORAY_BLENDER) { $env:ASTRORAY_BLENDER } else { [Environment]::GetEnvironmentVariable('ASTRORAY_BLENDER', 'User') }
  if (-not $b) { throw 'ASTRORAY_BLENDER is not set; set it to the Blender 5.3 install folder' }
  return $b
}

function Get-ArPython {
  # Returns the interpreter path; callers pass -I first.
  if ($env:CI -eq 'true') { return 'python' }
  $py = Join-Path (Get-ArBlenderRoot) '5.3\python\bin\python.exe'
  if (-not (Test-Path $py)) {
    throw "Blender's Python not found at '$py'; set ASTRORAY_BLENDER to the Blender 5.3 install folder"
  }
  return $py
}
